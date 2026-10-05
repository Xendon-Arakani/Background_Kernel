
import argparse,hashlib,json,time
from pathlib import Path
import numpy as np
import torch
from lag_forecaster import LagForecaster,IssueFeedbackTrain,HISTORY,HORIZON,RATE
from model_state_store import save_new_json,save_checkpoint,restore_checkpoint,lead_feedback,model_state_digest,owned_cpu_copy


def load_audio_packets(directory):
    pcm=np.load(directory/'audio.npy',mmap_mode='r');end=0
    for line in (directory/'source_pcm_packets.jsonl').open():
        header=json.loads(line);first=header['first']
        if first>=len(pcm):break
        count=min(header['frames'],len(pcm)-first)
        if first!=end:raise ValueError('Recorded packet gap; no stitching')
        end+=count
        yield first,np.array(pcm[first:end],copy=True),header
    if end!=len(pcm):raise ValueError('PCM lacks native packet provenance')


def run_recorded_train(args):
    args.output.mkdir(parents=True,exist_ok=False);torch.set_num_threads(1);torch.manual_seed(0)
    model=LagForecaster().cuda();optimizer=torch.optim.Adam(model.parameters(),lr=args.learning_rate)
    version=restore_checkpoint(args.initial_checkpoint,model,optimizer)['version'] if args.initial_checkpoint else 0
    initial_hash=model_state_digest(owned_cpu_copy(model.state_dict()))
    limit=HISTORY+round(args.seconds*RATE);end=limit+HORIZON
    pcm=np.load(args.source/'audio.npy',mmap_mode='r')
    if end>len(pcm):raise ValueError('Full real history, issuance interval AND final horizon required')
    loop=IssueFeedbackTrain(model,optimizer,torch.tensor(pcm[:HISTORY],device='cuda'),version=version)
    receipts=[];bins=[];prior_stats=np.zeros((HORIZON,2,8));prior_counts=np.zeros((HORIZON,2),dtype=np.int64)
    clock=time.perf_counter();failure=None;last=None
    log=(args.output/'transactions.jsonl').open('w')
    try:
        for first,chunk,header in load_audio_packets(args.source):
            if first+len(chunk)<=HISTORY:continue
            if first>=end:break
            lo=max(first,HISTORY);hi=min(first+len(chunk),end)

            boundaries=sorted(set([lo,hi]+([limit] if lo<limit<hi else [])))
            for a,b in zip(boundaries,boundaries[1:]):
                incoming=torch.tensor(chunk[a-first:b-first],device='cuda')
                issued,last=loop.issue_then_train(incoming,a,train=not args.frozen,make_predictions=a<limit)
                last.update(native_packet=header['packet'],native_frames=header['frames'],
                            wall_elapsed=time.perf_counter()-clock,
                            source_seconds=(b-HISTORY)/RATE,source='recorded; source-causal, not wall-clock future')
                receipts.append(last);log.write(json.dumps(last)+'\n')
                if (b-HISTORY)//RATE>(a-HISTORY)//RATE or b==end:
                    _,stats,counts=loop.issued_errors()
                    item=dict(source_end=b/RATE,version=loop.version,
                              **lead_feedback(stats-prior_stats,counts-prior_counts))
                    bins.append(item);prior_stats=stats.copy();prior_counts=counts.copy()
                    print(json.dumps(item),flush=True)
        if loop.end!=end:raise RuntimeError('Missing real target suffix')
        if loop.corrected_cells!=loop.forecasts*HORIZON:raise RuntimeError('Outstanding original issued cells')
    except BaseException as exc:
        failure=f'{type(exc).__name__}: {exc}';raise
    finally:
        log.flush();log.close()
        forecast_metrics,stats,counts=loop.issued_errors()
        output_checkpoint=args.output/'model_checkpoint.pt'
        weight_hash=save_checkpoint(output_checkpoint,model,optimizer,loop.version,
            dict(source=str(args.source.resolve()),source_end=loop.end,feedback='original-issued squared sample errors',
                 complete=failure is None and loop.end==end,pending_errors=2*(loop.forecasts*HORIZON-loop.corrected_cells),
                 resume='model+optimizer; start a new real-history epoch; pending issued values are not checkpointed'))
        np.savez(args.output/'per-lead.npz',statistics=stats,counts=counts,
                 target_lead_samples=np.arange(HORIZON)+1)
        completed=failure is None and loop.end==end
        summary=dict(schema='literal-stereo-issued-v1',complete=completed,failure=failure,
            source=str(args.source.resolve()),source_pcm_sha256=hashlib.sha256((args.source/'audio.npy').read_bytes()).hexdigest(),
            mode='frozen' if args.frozen else 'original-issued delayed-gradient training',
            learning_rate=args.learning_rate,parameters=sum(p.numel() for p in model.parameters()),
            initial_sha=initial_hash,final_weights_sha256=weight_hash,
            initial_version=version,final_version=loop.version,training_updates=loop.version-version,
            issued_forecasts=loop.forecasts,corrected_errors=2*loop.corrected_cells,
            pending_errors=2*(loop.forecasts*HORIZON-loop.corrected_cells),
            trained_errors=2*loop.gradient_cells,source_seconds=(loop.end-HISTORY)/RATE,
            wall_seconds=time.perf_counter()-clock,pending_bytes=loop.pending.numel()*4,
            peak_allocated_bytes=torch.cuda.max_memory_allocated(),stereo_metrics=forecast_metrics,per_source_second=bins,
            source_causality='recorded_audio_packet_boundaries_without_wall_clock_future_claims',
            update_boundaries='recorded_operating_system_packet_boundaries_with_splits_only_at_finite_phase_endpoints')
        save_new_json(args.output/'summary.json',summary)
        print(json.dumps({k:v for k,v in summary.items() if k!='per_source_second'}),flush=True)


def main():
    p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--seconds',type=float,default=20)
    p.add_argument('--learning-rate',type=float,default=1e-3);p.add_argument('--initial-checkpoint',type=Path)
    p.add_argument('--frozen',action='store_true');args=p.parse_args()
    if args.seconds<=0 or args.learning_rate<=0:p.error('Positive finite run_recorded_train duration and learning rate required')
    run_recorded_train(args)

if __name__=='__main__':main()
