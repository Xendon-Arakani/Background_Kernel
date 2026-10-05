
import argparse
import json
from pathlib import Path
import torch
from context_model import ContextForecaster as ContextSource
from history_model import HistoryForecaster
from checkpoint_store import restore_checkpoint,save_checkpoint


def migrate_context(source,output):
    legacy=ContextSource()
    saved=restore_checkpoint(source,legacy)
    model=HistoryForecaster()
    missing,unexpected=model.load_state_dict(legacy.state_dict(),strict=False)
    if missing != ["direct_weight"] or unexpected:
        raise RuntimeError("Only the direct raw-history weight may be absent from the context-only checkpoint")


    config=saved["optimizer"]["param_groups"]
    if len(config)!=1:
        raise ValueError("Expected the known single-group Adam save_checkpoint")
    defaults={key:value for key,value in config[0].items() if key!="params"}
    optimizer=torch.optim.Adam(model.parameters(),**defaults)
    output=Path(output)
    output.mkdir(parents=True,exist_ok=False)
    provenance=dict(kind="explicit summary-only to direct raw-history migration",
        source_checkpoint=str(Path(source).resolve()),source_step=saved["step"],
        source_weights_hash=saved["model_weights_sha256"],context_preserved=True,
        direct_init="unit_next_sample_coefficient_with_all_other_lag_factors_zero",
        optimizer_reset=True,raw_training_updates=0)
    path=save_checkpoint(output,model,optimizer,0,provenance)
    other=HistoryForecaster()
    other_optimizer=torch.optim.Adam(other.parameters())
    restored_checkpoint=restore_checkpoint(path,other,other_optimizer)
    if other_optimizer.state:
        raise RuntimeError("The migrated optimizer must begin without historical moments")
    return dict(checkpoint=str(path.resolve()),model_weights_sha256=restored_checkpoint["model_weights_sha256"],**provenance)


if __name__=="__main__":
    parser=argparse.ArgumentParser()
    parser.add_argument("--source-checkpoint",required=True,type=Path)
    parser.add_argument("--output",required=True,type=Path)
    args=parser.parse_args()
    print(json.dumps(migrate_context(args.source_checkpoint,args.output)),flush=True)
