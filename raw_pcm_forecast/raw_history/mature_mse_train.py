
from dataclasses import dataclass
import math
from pathlib import Path
import torch
from torch.autograd.function import once_differentiable
from torch_cuda_runtime import BundledCuda
from window_encode import window_moments
from history_decode import decode_origins


def prior_mature_cells(target_end, first_origin, horizon):
    n = max(0, target_end - first_origin)
    ramp = min(n, horizon)
    return ramp * (ramp + 1) // 2 + (n - ramp) * horizon


@dataclass(frozen=True)
class MatureInterval:
    target_first: int
    target_end: int
    first_origin: int
    history: int
    horizon: int

    @property
    def origin_first(self):
        return max(self.first_origin, self.target_first - self.horizon + 1)

    @property
    def input_first(self):
        return self.origin_first - self.history

    @property
    def scalar_errors(self):
        return 2 * (prior_mature_cells(self.target_end, self.first_origin, self.horizon) -
                    prior_mature_cells(self.target_first, self.first_origin, self.horizon))

    def validate(self, pcm):
        if not self.first_origin <= self.target_first < self.target_end:
            raise ValueError("No invented or repeated target interval")
        if pcm.shape != (self.target_end - self.input_first, 2):
            raise ValueError("PCM does not cover the exact required causal histories and targets")
        if not pcm.is_cuda or pcm.dtype != torch.float32 or pcm.requires_grad:
            raise ValueError("Captured raw stereo must be CUDA float32 without input gradients")


class WindowStates(torch.autograd.Function):
    @staticmethod
    def forward(ctx, pcm, frequency, log_decay, weights, history, rows, rate, horizon, module):
        ordinary, weighted = window_moments(pcm[:history], frequency, log_decay, rate, horizon, True)
        pole = torch.complex(-log_decay.exp(), 2 * math.pi * frequency)
        step = torch.view_as_real(torch.exp(pole / rate)).contiguous()
        eviction = torch.view_as_real(torch.exp(pole * (history / rate))).contiguous()
        all_ordinary = torch.empty((rows, 32, 2), device=pcm.device, dtype=torch.complex128)
        weighted_state = torch.empty((rows, 32), device=pcm.device, dtype=torch.complex128)
        states = torch.empty_like(weighted_state)
        updates, warps = rows-1, (rows-1+31)//32
        local_o = torch.empty((updates,32,2), device=pcm.device, dtype=torch.complex128)
        local_v = torch.empty_like(local_o)
        sums = torch.empty((4,warps,32,2), device=pcm.device, dtype=torch.complex128)
        if updates:
            module.launch("scan_moment_warps", (warps,8,1), (256,1,1),
                          [pcm.contiguous(),step,eviction,local_o,local_v,sums[0],sums[1],
                           history,updates,rate])
            module.launch("carry_moment_warps", (1,1,1), (64,1,1),
                          [ordinary,weighted,step,sums[0],sums[1],sums[2],sums[3],warps,rate])
        powers = torch.exp(pole[:,None] * (torch.arange(1,33,device=pcm.device,dtype=torch.float64)/rate))
        module.launch("finish_moment_scan", ((rows*32+255)//256,1,1), (256,1,1),
                      [ordinary,weighted,powers,local_o,local_v,sums[2],sums[3],weights,
                       all_ordinary,weighted_state,states,rows,rate])
        ctx.save_for_backward(all_ordinary, weighted_state, log_decay)
        return states

    @staticmethod
    @once_differentiable
    def backward(ctx, gradient):
        ordinary, weighted, log_decay = ctx.saved_tensors
        conjugate = gradient.conj()
        gf = (conjugate * (2j * math.pi) * weighted).real.sum(0)
        gd = (conjugate * -log_decay.exp()[None, :] * weighted).real.sum(0)
        gb = (conjugate[:, :, None] * ordinary).sum(0)
        return None, gf, gd, torch.stack((gb.real, -gb.imag), -1), None, None, None, None, None


class MatureMseTrainer:
    def __init__(self, model, optimizer, version=0, target_first=None):
        self.model, self.optimizer, self.version = model, optimizer, int(version)
        self.first_origin = model.history + 1
        self.target_cursor = self.first_origin if target_first is None else int(target_first)
        self.stream = torch.cuda.current_stream()
        self.module = BundledCuda(Path(__file__).with_name("mature_mse_kernels.cu"))
        self.total_scalar_errors = 0
        torch.backends.cuda.matmul.allow_tf32 = False

    def find_mature_interval(self, target_end):
        return MatureInterval(self.target_cursor, int(target_end), self.first_origin,
                        self.model.history, self.model.horizon)

    def accum_mature_grad(self, pcm, feedback):

        if torch.cuda.current_stream() != self.stream:
            raise RuntimeError("A trainer has one GPU owner stream")
        feedback.validate(pcm)
        if feedback.target_first != self.target_cursor:
            raise ValueError("MatureInterval gap or replay")
        m = self.model
        if (feedback.history,feedback.horizon)!=(m.history,m.horizon):
            raise ValueError("MatureInterval geometry does not match the model")
        rows = feedback.target_end - feedback.origin_first
        states = WindowStates.apply(pcm, m.frequency_hz, m.log_decay, m.input_weight,
                                      m.history, rows, m.rate, m.horizon, self.module)
        origins = torch.arange(feedback.origin_first, feedback.target_end,
                               dtype=torch.int64, device=pcm.device)
        targets = torch.arange(feedback.target_first, feedback.target_end,
                               dtype=torch.int64, device=pcm.device)
        pole = m.continuous_poles()

        phase = (feedback.target_first - origins).double()[:, None] / m.rate
        shifted = states * torch.exp(phase * pole[None, :])
        basis = torch.view_as_real(torch.exp(pole[:, None] *
                    ((targets - feedback.target_first).double()[None, :] / m.rate)))
        basis = basis.permute(0, 2, 1).reshape(64, len(targets)).float()

        s = torch.view_as_real(shifted).detach().contiguous()
        b = basis.detach().contiguous()
        ds = torch.empty_like(s)
        db = torch.zeros_like(b)
        for parameter in (m.hidden.weight, m.hidden.bias, m.output.weight, m.output.bias):
            parameter.grad = torch.zeros_like(parameter)
        m.direct_weight.grad=None
        target = pcm[feedback.target_first - feedback.input_first:]
        cache = torch.cuda.get_device_properties(pcm.device).L2_cache_size




        per_row = len(targets) * (16 * 3 + 2 + 1 + 24) * 4 + 2 * 16 * 64 * 4
        tile_rows = max(1, (cache - basis.numel() * 4) // per_row)
        loss_sum = torch.zeros((), device=pcm.device, dtype=torch.float64)
        inverse = torch.tensor(1. / feedback.scalar_errors, device=pcm.device, dtype=torch.float64)
        with torch.no_grad():
            for start in range(0, rows, tile_rows):
                count = min(tile_rows, rows - start)
                part = s[start:start+count]
                lo=max(0,feedback.origin_first+start-feedback.target_first)
                hi=min(len(targets),feedback.origin_first+start+count-1+m.horizon-feedback.target_first)
                columns=hi-lo
                basis_part=b[:,lo:hi]
                with torch.enable_grad():
                    raw_logits=decode_origins(
                        pcm[start:start+m.history+count-1],m.direct_weight,m.history,count,
                        feedback.target_first-feedback.origin_first+lo-start,columns)
                coeff = torch.empty((count*16, 64), device=pcm.device)
                self.module.launch("factor_hidden_decoder_coefficients_for_consecutive_origins", ((count*16*32+255)//256,1,1), (256,1,1),
                                   [part,m.hidden.weight,coeff,count])
                hidden = coeff @ basis_part
                activation = torch.empty((count,16,columns), device=pcm.device)
                dz = torch.empty_like(activation)
                error = torch.empty((count,2,columns), device=pcm.device)
                squared = torch.empty((count,columns), device=pcm.device)
                self.module.launch("mature_loss_adjoint", ((columns+255)//256,count,1), (256,1,1),
                    [hidden,m.hidden.bias,m.output.weight,m.output.bias,
                     raw_logits,target[lo:hi],inverse,
                     activation,dz,error,squared,count,columns,
                     feedback.origin_first+start-feedback.target_first-lo,m.horizon])
                flat_dz=dz.reshape(count*16,columns)
                dc=flat_dz @ basis_part.T
                db[:,lo:hi].addmm_(coeff.T,flat_dz)
                self.module.launch("state_adjoint", ((count*32+255)//256,1,1), (256,1,1),
                                   [dc,m.hidden.weight,ds[start:start+count],count])
                dw=torch.empty_like(m.hidden.weight)
                self.module.launch("hidden_weight_adjoint", (16,32,1), (256,1,1), [dc,part,dw,count])
                m.hidden.weight.grad.add_(dw)
                m.hidden.bias.grad.add_(dz.sum((0,2)))
                m.output.weight.grad.add_(torch.bmm(error,activation.transpose(1,2)).sum(0))
                m.output.bias.grad.add_(error.sum((0,2)))


                raw_logits.backward(error.permute(0,2,1).contiguous())
                loss_sum.add_(squared.sum().double()*inverse)
        torch.autograd.backward((torch.view_as_real(shifted),basis),(ds,db))
        return loss_sum

    def commit_optimizer(self, pcm, feedback):
        self.optimizer.zero_grad(set_to_none=True)
        loss = self.accum_mature_grad(pcm, feedback)



        finite = torch.isfinite(loss)
        for parameter in self.model.parameters():
            finite = finite & torch.isfinite(parameter.grad).all()
        self.optimizer.step()
        for parameter in self.model.parameters():
            finite = finite & torch.isfinite(parameter).all()
        done = torch.cuda.Event(blocking=True)
        done.record()
        done.synchronize()
        if not bool(finite):
            raise FloatingPointError("Nonfinite transaction; abort without publishing or checkpointing it")
        before = self.version
        self.version += 1
        self.target_cursor = feedback.target_end
        self.total_scalar_errors += feedback.scalar_errors
        return dict(version_before=before, version_after=self.version,
                    target_first=feedback.target_first, target_end=feedback.target_end,
                    first_origin=feedback.origin_first, origin_end=feedback.target_end,
                    scalar_errors=feedback.scalar_errors, stereo_raw_mse=float(loss),
                    total_scalar_errors=self.total_scalar_errors)
