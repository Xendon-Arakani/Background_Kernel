
import argparse
import json
from pathlib import Path
import time
import numpy as np
import torch
from audio_capture import SystemAudioCapture, StereoHistory, HISTORY, RATE
from history_model import HistoryForecaster
from frame_forecast_clock import FrameForecastClock
from checkpoint_store import restore_checkpoint, save_new_json

parser = argparse.ArgumentParser()
parser.add_argument("--capture-exe", required=True)
parser.add_argument("--initial-checkpoint", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--frames", type=int, default=480000)
args = parser.parse_args()
if args.frames <= 0:
    parser.error("--frames must be a positive finite source-frame budget")
args.output.mkdir(parents=True, exist_ok=False)
model = HistoryForecaster().cuda()
restored_checkpoint = restore_checkpoint(args.initial_checkpoint, model)


prepare_started = time.perf_counter()
prepared_module = FrameForecastClock.warm_cuda_forecast(model)
prepare_seconds = time.perf_counter() - prepare_started
capture = SystemAudioCapture(args.capture_exe, args.output, retain_packets=True)
history = StereoHistory()
clock = None
records, generated, checksum = [], 0, 0.
completed = False
wall_start = time.perf_counter()
first_started = None
try:
    with (args.output / "batches.jsonl").open("x", encoding="utf-8") as log:
        while generated < args.frames:
            header, pcm = capture.await_packet()
            first = header["first"]
            if clock is None:
                count = min(len(pcm), HISTORY - history.end)
                history.append(first, pcm[:count])
                pcm, first = pcm[count:], first + count
                if history.end == HISTORY:
                    clock = FrameForecastClock(model, torch.from_numpy(history.read(0, HISTORY)).cuda(),
                                        HISTORY, version=restored_checkpoint["step"],
                                        prepared_module=prepared_module)
                    torch.cuda.synchronize()
                    torch.cuda.reset_peak_memory_stats()
                if not len(pcm):
                    continue
            pcm = pcm[:args.frames - generated].copy()
            start = time.perf_counter()
            if first_started is None:
                first_started = start
            incoming = torch.from_numpy(pcm).cuda()
            batch = clock.forecast_packet(incoming, first)


            sum_value = batch.points.sum()
            done = torch.cuda.Event(blocking=True)
            done.record()
            done.synchronize()
            value = float(sum_value)
            if not np.isfinite(value):
                raise RuntimeError("Nonfinite forecast checksum")
            checksum += value
            elapsed = time.perf_counter() - start
            if batch.rows != len(pcm):
                raise RuntimeError("Every input frame must produce one forecast row")
            if batch.first_anchor != first + 1:
                raise RuntimeError("Each forecast anchor must immediately follow its input frame")
            if batch.points.shape != (len(pcm), 48000, 2):
                raise RuntimeError("Every forecast row must materialize the complete stereo horizon")
            with capture.condition:
                backlog = sum(len(item[1]) for item in capture.packet_queue)
            record = dict(
                first_input_frame=first, input_frames=len(pcm),
                first_anchor=batch.first_anchor,
                last_forecast_anchor=batch.first_anchor + batch.rows - 1,
                forecasts=batch.rows, horizon_frames=48000, channels=2,
                model_version=batch.model_version,
                trained_until=batch.trained_until,
                source_packet=header["packet"],
                source_qpc=header["source_qpc"], lookahead_frames=header["lookahead"],
                output_scalar_points=batch.points.numel(),
                output_bytes=batch.points.numel() * batch.points.element_size(),
                compute_consume_ms=elapsed * 1000,
                source_duration_ms=len(pcm) * 1000 / RATE,
                queued_input_frames=backlog, checksum=value)
            records.append(record)
            log.write(json.dumps(record) + "\n")
            log.flush()
            generated += len(pcm)
            if len(records) == 1 or generated % RATE == 0 or generated == args.frames:
                print(json.dumps(dict(generated_forecasts=generated,
                                      source_seconds=generated / RATE, last_batch_ms=elapsed * 1000,
                                      queued_input_frames=backlog)), flush=True)
            del batch
        completed = True
finally:
    capture.close()
    result = dict(
        completed=completed, expected_forecasts=args.frames, actual_forecasts=generated,
        warmup_frames=HISTORY, source_audio_seconds=generated / RATE,
        gpu_prepare_sec=prepare_seconds,
        total_wall_seconds=time.perf_counter() - wall_start,
        process_seconds=None if first_started is None else time.perf_counter() - first_started,
        horizon_frames=48000, channels=2,
        actual_scalar_points=sum(r["output_scalar_points"] for r in records),
        actual_output_bytes=sum(r["output_bytes"] for r in records),
        missed_input_frames=0 if completed else None,
        point_storage="full FP32 GPU arrays, consumed once; not exported or retained on disk",
        checkpoint_step=restored_checkpoint["step"], checkpoint_hash=restored_checkpoint["model_weights_sha256"],
        parameter_count=sum(p.numel() for p in model.parameters()), training_running=False, checksum=checksum,
        peak_cuda_bytes=torch.cuda.max_memory_allocated())
    if records:
        milliseconds = [r["compute_and_consume_ms"] for r in records]
        result.update(
            batches=len(records), batch_median_ms=float(np.median(milliseconds)),
            batch_p95_ms=float(np.percentile(milliseconds, 95)),
            batch_max_ms=max(milliseconds),
            long_packet_batches=sum(
                r["compute_and_consume_ms"] > r["source_duration_ms"] for r in records),
            max_queued_frames=max(r["queued_input_frames"] for r in records),
            last_queued_frames=records[-1]["queued_input_frames"],
            forecast_rate=generated / (sum(milliseconds) / 1000))
    save_new_json(args.output / "summary.json", result)
    print(json.dumps(result), flush=True)
