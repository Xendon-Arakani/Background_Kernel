
from __future__ import annotations

import collections
import json
from pathlib import Path
import struct
import subprocess
import threading
import numpy as np

RATE = 48_000
HISTORY = 480_000
HORIZON = 48_000
HEADER = struct.Struct("<II8QII")


class StereoHistory:
    def __init__(self, capacity=HISTORY):
        self.capacity = capacity
        self.samples = np.empty((capacity, 2), dtype=np.float32)
        self.end = 0

    def append(self, first, samples):
        if first != self.end:
            raise ValueError("Noncontiguous PCM; a new session is required")
        if samples.ndim != 2 or samples.shape[1] != 2 or not np.isfinite(samples).all():
            raise ValueError("Invalid stereo PCM")
        original_count = len(samples)
        if original_count > self.capacity:
            samples = samples[-self.capacity:]
            first += original_count - self.capacity
        position = first % self.capacity
        count = min(len(samples), self.capacity - position)
        self.samples[position:position + count] = samples[:count]
        self.samples[:len(samples) - count] = samples[count:]
        self.end += original_count

    def read(self, start, end):
        if start < max(0, self.end - self.capacity) or end > self.end or start > end:
            raise ValueError("Requested samples are unavailable; never pad or stitch")
        position, length = start % self.capacity, end - start
        count = min(length, self.capacity - position)
        return np.concatenate((self.samples[position:position + count],
                               self.samples[:length - count])).copy()


def read_exact_bytes(pipe, count):
    result = bytearray()
    while len(result) < count:
        data = pipe.read(count - len(result))
        if not data:
            raise EOFError("Native capture ended")
        result.extend(data)
    return result


class SystemAudioCapture:
    def __init__(self, executable, directory, retain_packets=False):
        self.executable = str(Path(executable).resolve())
        self.condition = threading.Condition()
        self.history = None if retain_packets else StereoHistory()
        self.end = 0
        self.error = None
        self.closed = False
        self.last_header = None
        self.packet_count = 0
        self.pending = None


        self.packet_queue = collections.deque() if retain_packets else None
        self.tags = collections.deque()
        self.directory = Path(directory)
        self.capture_stderr = open(self.directory / "capture_stderr.log", "wb")
        self.packet_journal = open(self.directory / "source_pcm_packets.jsonl", "w", encoding="utf-8")
        self.process = subprocess.Popen([self.executable], stdout=subprocess.PIPE,
                                        stderr=self.capture_stderr, creationflags=subprocess.CREATE_NO_WINDOW)
        self.thread = threading.Thread(target=self._read_source_packets, name="complete-source-pcm-packet-reader")
        self.thread.start()

    def _read_source_packets(self):
        try:
            previous_device_end = None
            previous_qpc = None
            while True:
                values = HEADER.unpack(read_exact_bytes(self.process.stdout, HEADER.size))
                magic, version, packet, trigger, device, source, copied, released, lookahead, first, frames, flags = values
                if magic != 0x31574B42 or version != 1 or trigger != packet + 2 or not frames:
                    raise ValueError("Invalid two-packet wire identity")
                if packet != self.packet_count or flags & 4:
                    raise ValueError("Packet identity/timestamp fault")
                if previous_device_end is not None and (
                    device != previous_device_end or source <= previous_qpc or flags & 1
                ):
                    raise ValueError("Source continuity fault")
                pcm = np.frombuffer(read_exact_bytes(self.process.stdout, frames * 8), dtype="<f4").reshape(-1, 2)
                record = dict(packet=packet, trigger=trigger, first=first, frames=frames,
                              device=device, source_qpc=source, copied_qpc=copied,
                              released_qpc=released, lookahead=lookahead, flags=flags)
                with self.condition:
                    if first != self.end or not np.isfinite(pcm).all():
                        raise ValueError("Noncontiguous or nonfinite PCM")
                    if self.history is not None:
                        self.history.append(first, pcm)
                    self.end = first + frames
                    if self.pending is not None:
                        pending = self.pending
                        lo = max(first, pending["anchor"])
                        hi = min(first + frames, pending["anchor"] + HORIZON)
                        if hi > lo:
                            pending["target"][lo - pending["anchor"]:hi - pending["anchor"]] = pcm[lo - first:hi - first]
                            pending["qpc"][lo - pending["anchor"]:hi - pending["anchor"]] = (
                                source + np.arange(lo - first, hi - first, dtype=np.float64) * 10_000_000 / RATE)
                            pending["count"] += hi - lo
                    self.last_header = record
                    if self.packet_queue is not None:
                        self.packet_queue.append((record, pcm))
                    else:
                        self.tags.append(record)
                        while self.tags and self.tags[0]["first"] + self.tags[0]["frames"] <= self.end - HISTORY:
                            self.tags.popleft()
                    self.packet_count += 1
                    self.condition.notify_all()
                self.packet_journal.write(json.dumps(record) + "\n")
                previous_device_end, previous_qpc = device + frames, source
        except BaseException as exc:
            with self.condition:
                self.error = exc
                self.condition.notify_all()
        finally:
            self.packet_journal.flush()
            self.packet_journal.close()

            self.process.stdout.close()

    def _raise_capture_error(self):
        if self.error is not None:
            raise RuntimeError("Audio capture failed") from self.error

    def await_packet(self):
        if self.packet_queue is None:
            raise RuntimeError("Packet retention was not requested")
        with self.condition:
            while not self.packet_queue:
                self._raise_capture_error()
                self.condition.wait()
            self._raise_capture_error()
            return self.packet_queue.popleft()

    def take_source_packets(self):

        if self.packet_queue is None:
            raise RuntimeError("Packet retention was not requested")
        with self.condition:
            while not self.packet_queue:
                self._raise_capture_error()
                self.condition.wait()
            self._raise_capture_error()
            ready = list(self.packet_queue)
            self.packet_queue.clear()
            return ready

    def fail_waiters(self, error):

        with self.condition:
            self.error = error
            self.condition.notify_all()

    def begin_forecast(self):

        with self.condition:
            if self.history is None:
                raise RuntimeError("Use packet handoff for a packet-retaining capture")
            while self.history.end < HISTORY:
                self._raise_capture_error()
                self.condition.wait()
            self._raise_capture_error()
            if self.pending is not None:
                raise RuntimeError("An existing forecast target is still pending")
            anchor = self.history.end
            x = self.history.read(anchor - HISTORY, anchor)
            header = dict(self.last_header)
            anchor_qpc = header["source_qpc"] + (anchor - header["first"]) * 10_000_000 / RATE
            self.pending = dict(anchor=anchor, target=np.empty((HORIZON, 2), np.float32),
                                qpc=np.empty(HORIZON, np.float64), count=0)
            return x, anchor, anchor_qpc, header

    def await_target(self, anchor, with_timestamps=False):
        with self.condition:
            if self.pending is None or self.pending["anchor"] != anchor:
                raise RuntimeError("Forecast/target identity mismatch")
            while self.pending["count"] < HORIZON:
                self._raise_capture_error()
                self.condition.wait()
            self._raise_capture_error()
            result = self.pending["target"].copy()
            timestamps = self.pending["qpc"].copy()
            self.pending = None
            return (result, timestamps) if with_timestamps else result

    def close(self):
        if self.closed:
            return
        self.closed = True
        if self.process.poll() is None:
            subprocess.run([self.executable, "--stop", str(self.process.pid)], check=True,
                           creationflags=subprocess.CREATE_NO_WINDOW)
        self.thread.join()
        self.process.wait()
        self.process.stdout.close()
        self.capture_stderr.close()
        if self.process.returncode:
            raise RuntimeError(f"Native capture exited with {self.process.returncode}")
