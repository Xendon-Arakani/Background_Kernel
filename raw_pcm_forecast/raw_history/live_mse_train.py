
import argparse
import json
from pathlib import Path
import signal
import time
import uuid
import torch
from audio_capture import SystemAudioCapture
from history_model import HistoryForecaster
from mature_mse_train import MatureMseTrainer
from train_state import PendingTrainAudio, CheckpointWriter
from checkpoint_store import restore_checkpoint, save_new_json


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--capture-exe", required=True)
    parser.add_argument("--initial-checkpoint", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--target-frame-limit", type=int)
    args = parser.parse_args()
    if args.target_frame_limit is not None and args.target_frame_limit <= 0:
        parser.error("The optional target-frame limit must be positive")
    args.output.mkdir(parents=True, exist_ok=False)
    model = HistoryForecaster().cuda()
    optimizer = torch.optim.Adam(model.parameters(), lr=.001)
    saved = restore_checkpoint(args.initial_checkpoint, model, optimizer)
    trainer = MatureMseTrainer(model, optimizer, version=saved["step"])
    target_limit_end = None if args.target_frame_limit is None else trainer.first_origin + args.target_frame_limit
    audio = PendingTrainAudio()
    source_id = str(uuid.uuid4())
    save_new_json(args.output / "contract.json", dict(
        source_id=source_id, initial_checkpoint=str(args.initial_checkpoint.resolve()),
        initial_weights_hash=saved["model_weights_sha256"], initial_version=saved["step"],
        sample_rate=model.rate, channels=2, history=model.history, horizon=model.horizon,
        first_origin=trainer.first_origin, loss="raw MSE of every newly mature origin/target pair",
        output_activation="tanh", output_range=[-1.,1.], checkpoint_schema=model.checkpoint_schema,
        scheduling="currently available feedback at actual GPU completion",
        batch_size=None, target_hz=None, replay=False, future_label_wait=False,
        inference_running=False, playback=False,
        target_frame_limit=args.target_frame_limit,
        checkpoint_policy="every successful update, immutable queued CPU snapshot, write-through commit",
        quality_metric=False))
    capture = SystemAudioCapture(args.capture_exe, args.output, retain_packets=True)
    writer = CheckpointWriter(args.output, source_id, capture.fail_waiters)
    stop_requested = False
    def request_stop(signum, frame):
        nonlocal stop_requested
        stop_requested = True
        capture.fail_waiters(InterruptedError("User requested stop"))
    previous_handler = signal.signal(signal.SIGINT, request_stop)
    reason = "stopped"
    try:
        writer.submit_model_state(model, optimizer, trainer.version, None)
        while True:
            if stop_requested:
                reason = "user_stop"
                break
            writer.raise_store_error()
            audio.append_packets(capture.take_source_packets())
            if stop_requested:
                reason = "user_stop"
                break
            if audio.end <= trainer.target_cursor:
                continue
            feedback = trainer.find_mature_interval(audio.end if target_limit_end is None else min(audio.end, target_limit_end))
            pcm = torch.from_numpy(audio.join_train_interval(feedback.input_first, feedback.target_end)).cuda()
            started=time.perf_counter()
            receipt = trainer.commit_optimizer(pcm, feedback)
            receipt["compute_seconds"]=time.perf_counter()-started
            receipt["released_at_completion"]=capture.end
            receipt["backlog_frames_at_completion"]=max(0,capture.end-trainer.target_cursor)
            writer.submit_model_state(model, optimizer, trainer.version, receipt)
            first_needed = max(trainer.first_origin, trainer.target_cursor-model.horizon+1)-model.history
            audio.release_train_before(first_needed)
            print(json.dumps(receipt), flush=True)
            if target_limit_end is not None and trainer.target_cursor == target_limit_end:
                reason = "target_frame_limit_reached"
                break
    except BaseException:
        if stop_requested:
            reason = "user_stop"
        else:
            reason = "fault"
            raise
    finally:
        try:
            capture.close()
        finally:
            try:
                writer.close_after_commit()
            finally:
                durable_target = (writer.last_receipt["target_end"] if writer.last_receipt else
                                  trainer.first_origin)
                save_new_json(args.output / "stopped.json", dict(
                    reason=reason, last_commit=writer.last_committed,
                    last_feedback=writer.last_receipt,
                    source_released_end=capture.end, in_memory_target_end=trainer.target_cursor,
                    durable_target_end=durable_target,
                    pending_targets=max(0,capture.end-durable_target),
                    resident_errors=trainer.total_scalar_errors,
                    checkpoint_error=None if writer.error is None else str(writer.error),
                    feedback_pending=True))
                signal.signal(signal.SIGINT, previous_handler)


if __name__ == "__main__":
    main()
