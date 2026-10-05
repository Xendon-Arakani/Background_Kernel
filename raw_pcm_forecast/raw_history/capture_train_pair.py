import argparse
import json
from pathlib import Path
import numpy as np
from audio_capture import SystemAudioCapture
from checkpoint_store import save_new_array, save_new_json

parser = argparse.ArgumentParser()
parser.add_argument("--capture-exe", required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
capture = SystemAudioCapture(args.capture_exe, args.output)
try:
    x, anchor, anchor_qpc, header = capture.begin_forecast()
    y = capture.await_target(anchor)
    save_new_array(args.output / "past.npy", x)
    save_new_array(args.output / "target.npy", y)
    info = dict(anchor=anchor, anchor_qpc=anchor_qpc, packet=header,
                past_shape=list(x.shape), target_shape=list(y.shape),
                past_rms=float(np.sqrt(np.mean(x.astype(np.float64) ** 2))),
                target_rms=float(np.sqrt(np.mean(y.astype(np.float64) ** 2))),
                purpose="known_history_and_target_training_pair")
    save_new_json(args.output / "pair.json", info)
    print(json.dumps(info), flush=True)
finally:
    capture.close()
