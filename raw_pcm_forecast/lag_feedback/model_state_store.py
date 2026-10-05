
import ctypes as ct
import hashlib
import json
import os
from pathlib import Path
import uuid
import numpy as np
import torch
from lag_forecaster import SCHEMA,RATE


def owned_cpu_copy(value):
    if isinstance(value,torch.Tensor):return value.detach().to('cpu',copy=True)
    if isinstance(value,dict):return {k:owned_cpu_copy(v) for k,v in value.items()}
    if isinstance(value,list):return [owned_cpu_copy(v) for v in value]
    if isinstance(value,tuple):return tuple(owned_cpu_copy(v) for v in value)
    return value


def model_state_digest(state):
    h=hashlib.sha256()
    for k,v in state.items():h.update(k.encode());h.update(v.numpy().tobytes())
    return h.hexdigest()


def write_new_file(path,writer):
    path=Path(path)
    if path.exists():raise FileExistsError(path)
    temporary=path.with_name(path.name+'.'+uuid.uuid4().hex+'.partial')
    with temporary.open('xb') as stream:
        writer(stream);stream.flush();os.fsync(stream.fileno())
    move=ct.WinDLL('kernel32',use_last_error=True).MoveFileExW
    move.argtypes=[ct.c_wchar_p,ct.c_wchar_p,ct.c_uint];move.restype=ct.c_int
    if not move(str(temporary),str(path),8):raise ct.WinError(ct.get_last_error())


def save_new_json(path,data):write_new_file(path,lambda f:f.write(json.dumps(data,indent=2,allow_nan=False).encode()))


def save_checkpoint(path,model,optimizer,version,extra):
    state=owned_cpu_copy(model.state_dict())
    payload=dict(schema=SCHEMA,history=model.history,horizon=model.horizon,rate=RATE,
        parameters=sum(p.numel() for p in model.parameters()),version=version,
        model=state,optimizer=owned_cpu_copy(optimizer.state_dict()),weights_sha256=model_state_digest(state),extra=extra)
    write_new_file(path,lambda f:torch.save(payload,f));return payload['weights_sha256']


def restore_checkpoint(path,model,optimizer):
    saved=torch.load(path,map_location='cpu',weights_only=True)
    if (saved['schema'],saved['history'],saved['horizon'],saved['rate'])!=(SCHEMA,model.history,model.horizon,RATE):
        raise ValueError('Different raw-sample architecture; no implicit migration')
    if model_state_digest(saved['model'])!=saved['weights_sha256']:raise ValueError('Checkpoint model hash mismatch')
    model.load_state_dict(saved['model']);optimizer.load_state_dict(saved['optimizer']);return saved


def lead_feedback(stats,counts):
    s=stats.sum((0,1));n=int(counts[:,0].sum())*2;nf=int(counts[:,1].sum())*2
    return dict(scalar_errors=n,future_errors=nf,
        mse=float(s[0]/n) if n else None,zero_reference_mse=float(s[1]/n) if n else None,
        error_drop_vs_zero=float(1-s[0]/s[1]) if s[1] else None,
        predicted_rms=float(np.sqrt(s[2]/n)) if n else None,target_rms=float(np.sqrt(s[1]/n)) if n else None,
        raw_correlation=float(s[3]/np.sqrt(s[1]*s[2])) if s[1]*s[2] else None,
        strict_future_mse=float(s[6]/nf) if nf else None,
        future_zero_mse=float(s[7]/nf) if nf else None)
