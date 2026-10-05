
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import time
import numpy as np
import torch
from audio_capture import SystemAudioCapture, HISTORY, HORIZON, RATE
from history_model import HistoryForecaster, stereo_raw_mse, stereo_metrics
from checkpoint_store import save_checkpoint, save_new_array, save_new_json, model_weights_sha256


def qpc_hns():
    frequency, count = ctypes.c_int64(), ctypes.c_int64()
    ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(frequency))
    ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(count))
    return count.value * 10_000_000 / frequency.value


def lead_metrics(rows):
    mse = sum(r["mse"] for r in rows)
    baseline = sum(r["silence_mse"] for r in rows)
    return dict(forecasts=len(rows), mean_mse=mse / len(rows),
                mean_silence_mse=baseline / len(rows),
                silence_skill=None if baseline == 0 else 1 - mse / baseline,
                beat_silence_count=sum(r["mse"] < r["silence_mse"] for r in rows),
                repeat_second_mse=sum(r["repeat_mse"] for r in rows) / len(rows))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--capture-exe", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--forecasts", type=int, default=60)
    parser.add_argument("--frozen-forecasts", type=int, default=30)
    parser.add_argument("--learning-rate", type=float, default=0.001)
    args = parser.parse_args()
    if args.forecasts < 1 or args.frozen_forecasts < 0 or args.learning_rate <= 0:
        parser.error("Positive training, frozen-evaluation, and learning-rate values are required")
    args.output.mkdir(parents=True, exist_ok=False)
    torch.backends.cuda.matmul.allow_tf32 = False
    model = HistoryForecaster(seed=1).cuda()
    optimizer = torch.optim.Adam(model.parameters(), lr=args.learning_rate)
    save_checkpoint(args.output, model, optimizer, 0, dict(kind="fresh_live_causal_training"))
    save_new_json(args.output / "contract.json", dict(
        history=HISTORY, horizon=HORIZON, channels=2, rate=RATE,
        parameters=sum(p.numel() for p in model.parameters()), loss="raw PCM MSE",
        imported_checkpoint=None, seed=1, optimizer="Adam, no weight decay",
        learning_rate=args.learning_rate, train_forecast_cap=args.forecasts,
        frozen_eval_budget=args.frozen_forecasts,
        scheduling="OS packet readiness, target maturity, GPU completion; no cadence timer",
        decoder_dtype="float32", state_phase_dtype="float64",
        silence_is_data=True, input_normalization=False, output_clipping=False,
        output_activation="tanh", output_range=[-1.,1.], checkpoint_schema=model.checkpoint_schema))
    capture = SystemAudioCapture(args.capture_exe, args.output)
    rows, trained_through, completed = [], 0, False
    started = time.perf_counter()
    try:
        with (args.output / "forecasts.jsonl").open("x", encoding="utf-8") as log:
            for step in range(args.forecasts + args.frozen_forecasts):
                should_train = step < args.forecasts
                model_step = min(step, args.forecasts)
                x_np, anchor, source_qpc, header = capture.begin_forecast()
                if trained_through > anchor:
                    raise RuntimeError("Training crossed forecast boundary")
                prefix = args.output / f"forecast-{step:04d}"
                x = torch.from_numpy(x_np).cuda()
                issue_qpc = qpc_hns()
                state_hash = model_weights_sha256(model)
                torch.cuda.synchronize()
                forward_start = time.perf_counter()
                with torch.no_grad():

                    prediction = model(x)
                torch.cuda.synchronize()
                compute_seconds = time.perf_counter() - forward_start
                p_np = prediction.cpu().numpy().copy()
                if not np.isfinite(p_np).all():
                    raise RuntimeError("Non-finite forecast; no replacement or clipping")
                completion_qpc = qpc_hns()
                save_new_array(prefix.with_suffix(".prediction.npy"), p_np)
                sealed_qpc = qpc_hns()
                forecast_record = dict(
                    forecast=step, model_step=model_step, model_weights_sha256=state_hash,
                    phase="prequential_training" if should_train else "frozen_evaluation",
                    input_start=anchor - HISTORY, input_end_exclusive=anchor,
                    target_start=anchor, target_end_exclusive=anchor + HORIZON,
                    trained_until=trained_through,
                    issue_qpc=issue_qpc, complete_qpc=completion_qpc,
                    sealed_qpc=sealed_qpc, source_anchor_qpc=source_qpc,
                    compute_seconds=compute_seconds,
                    input_sha256=hashlib.sha256(x_np.tobytes()).hexdigest(),
                    prediction_sha256=hashlib.sha256(p_np.tobytes()).hexdigest(),
                    input_rms=float(np.sqrt(np.mean(x_np.astype(np.float64) ** 2))),
                    source_packet=header)
                save_new_json(prefix.with_suffix(".sealed.json"), forecast_record)

                y_np, sample_qpc = capture.await_target(anchor, with_timestamps=True)
                if model_weights_sha256(model) != state_hash:
                    raise RuntimeError("Weights changed while a forecast was pending")
                y = torch.from_numpy(y_np).cuda()
                score = stereo_metrics(prediction, y)
                repeat_mse = float(np.mean((x_np[-HORIZON:].astype(np.float64) - y_np) ** 2))

                mask = sample_qpc > sealed_qpc
                future_count = int(mask.sum())
                strict = None
                if future_count:
                    strict = stereo_metrics(torch.from_numpy(p_np[mask]), torch.from_numpy(y_np[mask]))
                row = dict(forecast_record, **score, repeat_mse=repeat_mse,
                           future_at_publish=future_count,
                           strict_future=strict)
                save_new_array(prefix.with_suffix(".target.npy"), y_np)

                if step == 0:
                    save_new_array(args.output / "first-past.npy", x_np)
                row["training_seconds"] = 0.
                if should_train:
                    optimizer.zero_grad(set_to_none=True)
                    train_start = time.perf_counter()
                    training_prediction = model(x)

                    if not torch.equal(training_prediction.detach(), prediction):
                        raise RuntimeError("The training pass must reproduce the immutable issued forecast")
                    loss = stereo_raw_mse(training_prediction, y)
                    if not torch.isfinite(loss):
                        raise RuntimeError("Non-finite raw training MSE")
                    loss.backward()
                    if not all(p.grad is not None and torch.isfinite(p.grad).all() for p in model.parameters()):
                        raise RuntimeError("Missing/non-finite gradient")
                    optimizer.step()
                    if not all(torch.isfinite(p).all() for p in model.parameters()):
                        raise RuntimeError("Non-finite parameter")
                    torch.cuda.synchronize()
                    row["training_seconds"] = time.perf_counter() - train_start
                    trained_through = anchor + HORIZON
                    save_checkpoint(args.output, model, optimizer, model_step + 1,
                               dict(trained_until=trained_through, score=row))
                rows.append(row)
                log.write(json.dumps(row, allow_nan=False) + "\n")
                log.flush()
                os.fsync(log.fileno())
                print(json.dumps(dict(forecast=step, model_step=model_step + int(should_train),
                                      phase=row["phase"],
                                      rms=score["target_rms"], skill=score["skill_vs_silence"],
                                      prediction_rms=score["predicted_rms"],
                                      forward_ms=compute_seconds * 1000,
                                      train_ms=row["training_seconds"] * 1000,
                                      future_frames=future_count)), flush=True)
        completed = True
    finally:
        capture.close()
        trained = [r for r in rows if r["phase"] == "prequential_training"]
        frozen = [r for r in rows if r["phase"] == "frozen_evaluation"]
        summary = dict(completed=completed, elapsed_seconds=time.perf_counter() - started,
                       capture_packets=capture.packet_count, training_updates=len(trained),
                       capture_released=capture.history.end,
                       peak_cuda_bytes=torch.cuda.max_memory_allocated())
        if rows:
            section = min(10, len(rows))
            summary.update(overall=lead_metrics(rows), first=lead_metrics(rows[:section]),
                           last=lead_metrics(rows[-section:]),
                           median_forward_ms=float(np.median([r["compute_seconds"] * 1000 for r in rows])),
                           median_training_ms=float(np.median([r["training_seconds"] * 1000 for r in rows])),
                           future_fraction=sum(r["future_at_publication_frames"] for r in rows) / (len(rows) * HORIZON))
            if trained:
                summary["training_forecasts"] = lead_metrics(trained)
                summary["median_training_ms"] = float(np.median([r["training_seconds"] * 1000 for r in trained]))
            if frozen:
                summary["frozen_evaluation"] = lead_metrics(frozen)
        save_new_json(args.output / "summary.json", summary)
        print(json.dumps(summary), flush=True)


if __name__ == "__main__":
    main()
