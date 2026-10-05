
import math
import torch
from torch.autograd.function import once_differentiable


def window_moments(x, frequency, log_decay, rate, chunk_frames, derivatives):
    pole = torch.complex(-log_decay.exp(), 2 * math.pi * frequency)
    ordinary = torch.zeros((len(frequency), 2), dtype=torch.complex128, device=x.device)
    weighted = torch.zeros_like(ordinary)
    for start in range(0, len(x), chunk_frames):
        end = min(len(x), start + chunk_frames)
        age = torch.arange(len(x) - start - 1, len(x) - end - 1, -1,
                           dtype=torch.float64, device=x.device) / rate
        kernel = torch.exp(pole[:, None] * age[None, :])
        values = x[start:end].to(torch.complex128)
        if derivatives:
            sums = kernel @ torch.cat((values, values * age[:, None]), dim=1)
            ordinary += sums[:, :2]
            weighted += sums[:, 2:]
        else:
            ordinary += kernel @ values
    return ordinary, weighted


class WindowEncodeState(torch.autograd.Function):
    @staticmethod
    def forward(ctx, x, frequency, log_decay, input_weight, rate, chunk_frames):
        ordinary, weighted = window_moments(x, frequency, log_decay, rate, chunk_frames, True)
        b = torch.view_as_complex(input_weight.contiguous())
        ctx.save_for_backward(x, frequency, log_decay, input_weight, ordinary, weighted)
        ctx.rate, ctx.chunk_frames = rate, chunk_frames
        return (b * ordinary).sum(dim=1)

    @staticmethod
    @once_differentiable
    def backward(ctx, gradient):
        x, frequency, log_decay, input_weight, ordinary, weighted = ctx.saved_tensors
        b = torch.view_as_complex(input_weight.contiguous())
        weighted_state = (b * weighted).sum(dim=1)
        conjugate = gradient.conj()
        grad_frequency = (conjugate * (2j * math.pi) * weighted_state).real
        grad_decay = (conjugate * -log_decay.exp() * weighted_state).real
        product = conjugate[:, None] * ordinary
        grad_b = torch.stack((product.real, -product.imag), dim=-1)
        grad_x = None
        if ctx.needs_input_grad[0]:
            grad_x = torch.empty_like(x)
            pole = torch.complex(-log_decay.exp(), 2 * math.pi * frequency)
            for start in range(0, len(x), ctx.chunk_frames):
                end = min(len(x), start + ctx.chunk_frames)
                age = torch.arange(len(x) - start - 1, len(x) - end - 1, -1,
                                   dtype=torch.float64, device=x.device) / ctx.rate
                kernel = torch.exp(pole[:, None] * age[None, :])
                grad_x[start:end] = (kernel.T @ (conjugate[:, None] * b)).real.to(x.dtype)
        return grad_x, grad_frequency, grad_decay, grad_b, None, None


def encode_raw_tiles(x, frequency, log_decay, input_weight, rate, chunk_frames):
    if torch.is_grad_enabled():
        return WindowEncodeState.apply(x, frequency, log_decay, input_weight, rate, chunk_frames)
    ordinary, _ = window_moments(x, frequency, log_decay, rate, chunk_frames, False)
    return (ordinary * torch.view_as_complex(input_weight.contiguous())).sum(dim=1)
