
import hashlib
import json
import os
from pathlib import Path
import uuid
import numpy as np
import torch
from history_model import CHECKPOINT_SCHEMA, ARCHITECTURE


def write_new_file(path, writer):
    path = Path(path)
    if path.exists():
        raise FileExistsError(path)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".partial")
    with temporary.open("xb") as stream:
        writer(stream)
        stream.flush()
        os.fsync(stream.fileno())
    if os.name == "nt":
        import ctypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.MoveFileExW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32]
        kernel.MoveFileExW.restype = ctypes.c_int

        if not kernel.MoveFileExW(str(temporary), str(path), 8):
            raise ctypes.WinError(ctypes.get_last_error())
    else:
        os.rename(temporary, path)


def save_new_json(path, data):
    encoded = json.dumps(data, indent=2, allow_nan=False).encode("utf-8")
    write_new_file(path, lambda stream: stream.write(encoded))


def save_new_array(path, data):
    data = np.asarray(data, dtype="<f4", order="C")
    write_new_file(path, lambda stream: np.save(stream, data, allow_pickle=False))


def model_weights_sha256(model):
    model_state_digest = hashlib.sha256()
    for name, value in model.state_dict().items():
        model_state_digest.update(name.encode())
        model_state_digest.update(value.detach().cpu().numpy().tobytes())
    return model_state_digest.hexdigest()


def checkpoint_contract(model):
    return dict(schema=getattr(model,"checkpoint_schema",CHECKPOINT_SCHEMA),
                architecture=getattr(model,"architecture",ARCHITECTURE),
                history=model.history, horizon=model.horizon, rate=model.rate,
                parameter_count=sum(p.numel() for p in model.parameters()),
                output_activation="tanh", output_range=[-1.0, 1.0])


def save_checkpoint(directory, model, optimizer, step, extra=None):
    path = Path(directory) / f"save_checkpoint-{step:06d}.pt"
    payload = dict(**checkpoint_contract(model),
                   step=step, model_weights_sha256=model_weights_sha256(model),
                   model={k: v.detach().cpu() for k, v in model.state_dict().items()},
                   optimizer=optimizer.state_dict(), extra=extra)
    write_new_file(path, lambda stream: torch.save(payload, stream))
    return path


def restore_checkpoint(path, model, optimizer=None):
    saved = torch.load(path, weights_only=True, map_location=next(model.parameters()).device)
    if saved.get("schema")==1:
        raise ValueError("Legacy unbounded save_checkpoint: use migrate_bounded.py for an explicit weights-only warm start")
    if saved.get("schema")==2 and checkpoint_contract(model)["schema"]==3:
        raise ValueError("Summary-only save_checkpoint: use migrate_direct.py for explicit raw-history migration")
    if any(saved.get(key)!=value for key,value in checkpoint_contract(model).items()):
        raise ValueError("Checkpoint contract does not match this bounded-output architecture")
    model.load_state_dict(saved["model"], strict=True)
    if model_weights_sha256(model) != saved["model_weights_sha256"]:
        raise ValueError("Checkpoint hash mismatch")
    if optimizer is not None:
        optimizer.load_state_dict(saved["optimizer"])
    return saved


def import_linear_state(path, model):

    saved = torch.load(path, weights_only=True, map_location=next(model.parameters()).device)
    expected = dict(schema=1, architecture="finite-window-64-state-16-hidden",
                    history=model.history, horizon=model.horizon, rate=model.rate,
                    parameter_count=sum(p.numel() for p in model.parameters()))
    if any(saved.get(key)!=value for key,value in expected.items()):
        raise ValueError("Expected the matching legacy linear-output architecture")
    model.load_state_dict(saved["model"], strict=True)
    if model_weights_sha256(model)!=saved["model_weights_sha256"]:
        raise ValueError("Checkpoint hash mismatch")
    return saved
