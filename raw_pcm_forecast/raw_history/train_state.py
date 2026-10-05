
from collections import deque
import hashlib
import queue
import threading
from pathlib import Path
import numpy as np
import torch
from checkpoint_store import write_new_file, checkpoint_contract


class PendingTrainAudio:
    def __init__(self):
        self.source_packets = deque()
        self.end = 0

    def append_packets(self, source_packets):
        for header, pcm in source_packets:
            if header["first"] != self.end:
                raise ValueError("Source gap in pending feedback")
            self.source_packets.append((header["first"], pcm))
            self.end += len(pcm)

    def join_train_interval(self, start, end):
        if not self.source_packets or start < self.source_packets[0][0] or end > self.end or start >= end:
            raise ValueError("Required training PCM is unavailable")
        parts = [pcm[max(start-first,0):min(end-first,len(pcm))]
                 for first,pcm in self.source_packets if first < end and first+len(pcm) > start]
        result = np.concatenate(parts)
        if len(result) != end-start:
            raise ValueError("Missing PCM; never synthesize or skip it")
        return result

    def release_train_before(self, first_needed):
        while self.source_packets and self.source_packets[0][0]+len(self.source_packets[0][1]) <= first_needed:
            self.source_packets.popleft()
        if self.source_packets and self.source_packets[0][0] < first_needed:
            first, pcm = self.source_packets.popleft()
            self.source_packets.appendleft((first_needed, pcm[first_needed-first:]))


def owned_cpu_copy(value):
    if isinstance(value, torch.Tensor):
        return value.detach().to(device="cpu", copy=True)
    if isinstance(value, dict):
        return {key: owned_cpu_copy(item) for key,item in value.items()}
    if isinstance(value, list):
        return [owned_cpu_copy(item) for item in value]
    if isinstance(value, tuple):
        return tuple(owned_cpu_copy(item) for item in value)
    return value


class CheckpointWriter:
    def __init__(self, directory, source_id, on_failure):
        self.directory, self.source_id = Path(directory), source_id
        self.on_failure = on_failure
        self.queue = queue.Queue()
        self.error = None
        self.last_committed = None
        self.last_receipt = None
        self.thread = threading.Thread(target=self._write, name="immutable-model-optimizer-checkpoint-writer")
        self.thread.start()

    def submit_model_state(self, model, optimizer, version, receipt):
        self.raise_store_error()
        state = owned_cpu_copy(model.state_dict())
        model_state_digest = hashlib.sha256()
        for name, value in state.items():
            model_state_digest.update(name.encode())
            model_state_digest.update(value.numpy().tobytes())
        payload = dict(**checkpoint_contract(model),
                       step=version, model_weights_sha256=model_state_digest.hexdigest(), model=state,
                       optimizer=owned_cpu_copy(optimizer.state_dict()),
                       extra=dict(source_id=self.source_id, feedback=receipt,
                                  semantics="current-weight training MSE, not issued-forecast accuracy"))
        self.queue.put(payload)

    def _write(self):
        try:
            while True:
                payload = self.queue.get()
                if payload is None:
                    return
                path = self.directory / f"model_checkpoint-{payload['step']:06d}.pt"
                write_new_file(path, lambda stream: torch.save(payload, stream))


                self.last_committed = payload["step"]
                self.last_receipt = payload["extra"]["feedback"]
        except BaseException as error:
            self.error = error
            self.on_failure(error)

    def raise_store_error(self):
        if self.error is not None:
            raise RuntimeError("Checkpoint commit failed") from self.error

    def close_after_commit(self):
        self.queue.put(None)
        self.thread.join()
        self.raise_store_error()
