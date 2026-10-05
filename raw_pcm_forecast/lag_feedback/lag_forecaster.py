
from collections import deque
import ctypes as ct
from dataclasses import dataclass
import math
from pathlib import Path
import sys
import time
import numpy as np
import torch
from torch import nn
import torch.nn.functional as functional


sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"raw_history"))
from torch_cuda_runtime import BundledCuda

RATE=48_000
HISTORY=480_000
HORIZON=48_000
SCHEMA="literal-stereo-issued-v1"


def qpc_hns():
    frequency,count=ct.c_int64(),ct.c_int64()
    if not ct.windll.kernel32.QueryPerformanceFrequency(ct.byref(frequency)):
        raise OSError("QPC frequency unavailable")
    if not ct.windll.kernel32.QueryPerformanceCounter(ct.byref(count)):
        raise OSError("QPC unavailable")
    return count.value*10_000_000/frequency.value


def prior_mature_cells(end,first,horizon):
    n=max(0,end-first)
    ramp=min(n,horizon)
    return ramp*(ramp+1)//2+(n-ramp)*horizon


def mature_target_cells(first,end,origin_first,origin_last,horizon):
    def total(t):
        return prior_mature_cells(t,origin_first,horizon)-prior_mature_cells(t,origin_last+1,horizon)
    return total(end)-total(first)


class LagForecaster(nn.Module):
    def __init__(self,history=HISTORY,horizon=HORIZON):
        super().__init__()
        if not 1<=horizon<=history: raise ValueError("Require 1 <= horizon <= history")
        self.history,self.horizon=int(history),int(horizon)
        self.weight=nn.Parameter(torch.zeros(2,2,history+horizon-1))
        self.bias=nn.Parameter(torch.zeros(2))

    @property
    def raw_stereo_scale(self): return 1/math.sqrt(2*self.history)

    def decode_raw_logits(self,history,columns):
        if history.shape!=(self.history,2): raise ValueError("Full signed stereo history required")
        kernel=functional.pad(self.weight*self.raw_stereo_scale,(1,0))
        n=1<<(max(self.history+kernel.shape[-1]-1,self.history+columns)-1).bit_length()
        spectrum=torch.fft.rfft(history.T,n=n)
        convolution=torch.fft.irfft((torch.fft.rfft(kernel,n=n)*spectrum[None]).sum(1),n=n)
        return convolution[:,self.history:self.history+columns].T.contiguous()

    def forward(self,history):
        return (self.decode_raw_logits(history,self.horizon)+self.bias).tanh()


@dataclass
class IssuedForecasts:
    first_origin:int
    version:int
    trained_through:int
    published_qpc:float
    points:torch.Tensor


class IssueFeedbackTrain:
    def __init__(self,model,optimizer,history,*,version=0,module=None):
        if not history.is_cuda or history.dtype!=torch.float32 or history.shape!=(model.history,2):
            raise ValueError("Exactly H real float32 stereo frames on CUDA are required")
        if model.weight.device!=history.device or not bool(torch.isfinite(history).all() & (history.abs()<=1).all()):
            raise ValueError("Unmodified finite PCM in [-1,1] on the model device required; never clamp")
        self.model,self.optimizer=model,optimizer
        self.H,self.F=model.history,model.horizon
        self.stream=torch.cuda.current_stream(history.device)
        self.module=module or BundledCuda(Path(__file__).with_name("lag_forecast_cuda.cu"))
        self.raw=history.clone().contiguous()
        self.end=self.H
        self.first_origin=self.H+1
        self.last_origin=self.H
        self.version=int(version)
        self.trained_through=0
        self.forecasts=0
        self.corrected_cells=0
        self.gradient_cells=0
        self.draining=False

        self.pending=torch.empty((self.F*(self.F+1)//2,2),device=history.device)
        self.stats=torch.zeros((self.F,2,8),device=history.device,dtype=torch.float64)
        self.counts=torch.zeros((self.F,2),device=history.device,dtype=torch.int64)
        self.issues=deque()
        torch.backends.cuda.matmul.allow_tf32=False

    def _make_issue_times(self,first,last):
        result=np.empty(max(0,last-first+1),dtype=np.float64)
        written=0
        for begin,end,version,qpc in self.issues:
            lo,hi=max(first,begin),min(last+1,end)
            if hi>lo:
                result[lo-first:hi-first]=qpc;written+=hi-lo
        if written!=len(result): raise RuntimeError("Missing original forecast publication identity")
        return torch.from_numpy(result).to(device=self.raw.device)

    @torch.no_grad()
    def _issue_before_future(self,incoming,published_qpc=None):
        m=self.model
        rows=len(incoming)
        pcm=torch.cat((self.raw[-self.H:],incoming))[1:].contiguous()
        base=m.decode_raw_logits(pcm[:self.H],rows+self.F-1)
        scaled=(m.weight*m.raw_stereo_scale).contiguous()
        points=torch.empty((rows,self.F,2),device=self.raw.device)
        self.module.launch("forecast",((rows+self.F-1+255)//256,1,1),(256,1,1),
            [pcm,scaled,m.bias,base,points,self.H,self.F,rows])
        done=torch.cuda.Event(blocking=True);done.record();done.synchronize()
        stamp=qpc_hns() if published_qpc is None else float(published_qpc)
        issued=IssuedForecasts(self.end+1,self.version,self.trained_through,stamp,points)
        self.issues.append((issued.first_origin,issued.first_origin+rows,issued.version,stamp))
        self.last_origin=self.end+rows
        self.forecasts+=rows
        return issued

    @torch.no_grad()
    def accum_issue_grad(self,delta,pcm,past,scalar_errors):
        prefix=torch.cumsum(delta,dim=1,dtype=torch.float64)
        total=prefix[:,-1,:]
        n=1<<(len(pcm)+len(delta)-2).bit_length()
        sx=torch.fft.rfft(pcm.T.double(),n=n)
        sd=torch.fft.rfft(total.T,n=n)
        correlation=torch.fft.irfft(sd[:,None,:].conj()*sx[None,:,:],n=n)
        scale=self.model.raw_stereo_scale/scalar_errors
        result=torch.empty_like(self.model.weight)
        result[:,:,self.F-1:self.H]=correlation[:,:,past-self.H:past-self.F+1].flip(-1)*scale
        if self.F>1:
            scale_tensor=torch.tensor(scale,device=pcm.device,dtype=torch.float64)
            self.module.launch("boundary_gradient",((8*(self.F-1)+255)//256,1,1),(256,1,1),
                [prefix,pcm,result,scale_tensor,self.H,self.F,len(delta),past])
        return result,(total.sum(0)/scalar_errors).float()

    def issue_then_train(self,incoming,first,*,target_qpc=None,train=True,make_predictions=True,published_qpc=None):
        if torch.cuda.current_stream()!=self.stream: raise RuntimeError("One GPU owner stream")
        if first!=self.end: raise ValueError("Source gap or replay")
        if incoming.dtype!=torch.float32 or incoming.device!=self.raw.device or incoming.ndim!=2 or incoming.shape[1]!=2 or not len(incoming):
            raise ValueError("Nonempty unchanged float32 stereo required")
        if not bool(torch.isfinite(incoming).all() & (incoming.abs()<=1).all()):
            raise ValueError("Input outside signed unit PCM domain; refusing to clamp or normalize")
        if self.draining and make_predictions: raise ValueError("Cannot resume issuance after draining")
        if train and self.optimizer is None: raise ValueError("Training requires an optimizer")
        rows=len(incoming)
        if target_qpc is None:
            target_qpc=torch.full((rows,),float("-inf"),device=incoming.device,dtype=torch.float64)
        if target_qpc.shape!=(rows,) or target_qpc.dtype!=torch.float64 or target_qpc.device!=incoming.device:
            raise ValueError("One float64 future-eligibility time per actual frame; min(source QPC, capture-copy QPC) for live data")
        if not make_predictions: self.draining=True
        started=time.perf_counter()
        issued=self._issue_before_future(incoming,published_qpc) if make_predictions else None
        issued_seconds=time.perf_counter()-started
        target_end=first+rows
        cells=mature_target_cells(first,target_end,self.first_origin,self.last_origin,self.F)
        need_gradient=bool(train and cells)
        first_metadata=max(self.first_origin,first-self.F+1)
        last_metadata=min(target_end-1,self.last_origin)
        metadata=self._make_issue_times(first_metadata,last_metadata)
        delta=torch.empty((rows,self.F,2) if need_gradient else (0,),device=incoming.device)
        no_output=torch.empty(0,device=incoming.device)
        self.module.launch("feedback",((self.F+255)//256,1,1),(256,1,1),
            [self.pending,issued.points if issued else no_output,incoming.contiguous(),delta,self.stats,self.counts,
             target_qpc.contiguous(),metadata,ct.c_int64(first_metadata),ct.c_int64(first),
             ct.c_int64(self.first_origin),ct.c_int64(self.last_origin),self.F,rows,int(make_predictions),int(need_gradient)])
        version_before=self.version
        pcm=torch.cat((self.raw,incoming)).contiguous()
        if need_gradient:
            gw,gb=self.accum_issue_grad(delta,pcm,len(self.raw),2*cells)
            self.optimizer.zero_grad(set_to_none=True)
            self.model.weight.grad=gw;self.model.bias.grad=gb
            finite=torch.isfinite(gw).all() & torch.isfinite(gb).all()
            if not bool(finite): raise FloatingPointError("Nonfinite original-issued gradient")
            self.optimizer.step()
            if not bool(torch.isfinite(self.model.weight).all() & torch.isfinite(self.model.bias).all()):
                raise FloatingPointError("Nonfinite model update")
            self.version+=1
            self.trained_through=target_end
            self.gradient_cells+=cells
        done=torch.cuda.Event(blocking=True);done.record();done.synchronize()
        self.corrected_cells+=cells
        self.end=target_end
        self.raw=pcm[-(self.H+self.F-1):].clone()
        keep=max(self.first_origin,self.end-self.F+1)
        while self.issues and self.issues[0][1]<=keep: self.issues.popleft()
        return issued,dict(target_first=first,target_end=target_end,issued=rows if issued else 0,
            version_before=version_before,version_after=self.version,scalar_errors=2*cells,
            trained_errors=2*cells if need_gradient else 0,total_forecasts=self.forecasts,
            total_scalar_errors=2*self.corrected_cells,pending_errors=2*(self.forecasts*self.F-self.corrected_cells),
            inference_seconds=issued_seconds,step_seconds=time.perf_counter()-started)

    def issued_errors(self):
        stats=self.stats.cpu().numpy();counts=self.counts.cpu().numpy()
        count=int(counts[:,0].sum())*2
        strict_count=int(counts[:,1].sum())*2
        sums=stats.sum((0,1))
        return dict(scalar_errors=count,future_errors=strict_count,
            mse=None if not count else float(sums[0]/count),
            zero_reference_mse=None if not count else float(sums[1]/count),
            error_drop_vs_zero=None if not sums[1] else float(1-sums[0]/sums[1]),
            predicted_rms=None if not count else float(np.sqrt(sums[2]/count)),
            target_rms=None if not count else float(np.sqrt(sums[1]/count)),
            correlation=None if not sums[1]*sums[2] else float(sums[3]/np.sqrt(sums[1]*sums[2])),
            strict_future_mse=None if not strict_count else float(sums[6]/strict_count),
            future_zero_mse=None if not strict_count else float(sums[7]/strict_count)),stats,counts
