
import argparse
from pathlib import Path
import json
import torch
from context_model import ContextForecaster
from checkpoint_store import import_linear_state, save_checkpoint, restore_checkpoint


def migrate_tanh(source, output):
    model=ContextForecaster()
    saved=import_linear_state(source,model)

    optimizer=torch.optim.Adam(model.parameters())
    optimizer.load_state_dict(dict(state={},param_groups=saved["optimizer"]["param_groups"]))
    output=Path(output)
    output.mkdir(parents=True,exist_ok=False)
    provenance=dict(kind="explicit linear-to-bounded weights-only warm start",
        source_checkpoint=str(Path(source).resolve()), source_step=saved["step"],
        source_weights_hash=saved["model_weights_sha256"],
        copied_weights=True, optimizer_reset=True,
        optimizer_preserved=True, bounded_updates=0)
    path=save_checkpoint(output,model,optimizer,0,provenance)
    other=ContextForecaster()
    other_optimizer=torch.optim.Adam(other.parameters())
    restored_checkpoint=restore_checkpoint(path,other,other_optimizer)
    if restored_checkpoint["model_weights_sha256"] != saved["model_weights_sha256"] or other_optimizer.state:
        raise RuntimeError("The bounded-output migration must preserve weights and reset optimizer moments")
    return dict(checkpoint=str(path.resolve()),**provenance)


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--source-checkpoint",required=True,type=Path)
    parser.add_argument("--output",required=True,type=Path)
    args=parser.parse_args()
    print(json.dumps(migrate_tanh(args.source_checkpoint,args.output)),flush=True)


if __name__=="__main__": main()
