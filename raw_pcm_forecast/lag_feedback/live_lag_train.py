
import argparse,json,time
from pathlib import Path
import numpy as np
import torch
from lag_forecaster import LagForecaster,IssueFeedbackTrain,HISTORY,HORIZON,RATE
from audio_capture import SystemAudioCapture
from model_state_store import save_new_json,save_checkpoint,lead_feedback


def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True)
    p.add_argument('--capture-exe',type=Path,required=True);p.add_argument('--seconds',type=int,default=10)
    p.add_argument('--learning-rate',type=float,default=1e-4);args=p.parse_args()
    if args.seconds<=0 or args.learning_rate<=0:p.error('Positive finite duration and optimizer step required')
    args.output.mkdir(parents=True,exist_ok=False);torch.set_num_threads(1);torch.manual_seed(0)
    model=LagForecaster().cuda();optimizer=torch.optim.Adam(model.parameters(),lr=args.learning_rate)
    capture=SystemAudioCapture(args.capture_exe,args.output,retain_packets=True)
    chunks=[];warm=[];loop=None;first=0;failure=None;started=time.perf_counter()
    limit=HISTORY+args.seconds*RATE;end=limit+HORIZON;receipts=[];bins=[]
    previous_stats=np.zeros((HORIZON,2,8));previous_counts=np.zeros((HORIZON,2),dtype=np.int64)
    log=(args.output/'transactions.jsonl').open('w')
    try:
        while first<end:
            header,pcm=capture.await_packet()
            if header['first']!=first:raise ValueError('Source reorder/gap')
            last=min(first+len(pcm),end);pcm=pcm[:last-first];chunks.append(pcm)
            if first<HISTORY:
                take=min(last,HISTORY)-first;warm.append(pcm[:take])
                if first+take==HISTORY:
                    loop=IssueFeedbackTrain(model,optimizer,torch.tensor(np.concatenate(warm),device='cuda'));warm.clear()
                    print(json.dumps(dict(state='full_real_history',rms=float(np.sqrt(np.mean(np.concatenate(chunks)**2))))),flush=True)
            lo=max(first,HISTORY)
            if lo<last:
                bounds=sorted(set([lo,last]+([limit] if lo<limit<last else [])))
                for a,b in zip(bounds,bounds[1:]):
                    x=torch.tensor(pcm[a-first:b-first],device='cuda')
                    stamps=header['source_qpc']+np.arange(a-first,b-first,dtype=np.float64)*10_000_000/RATE



                    unseen_until=np.minimum(stamps,header['copied_qpc'])
                    issued,receipt=loop.issue_then_train(x,a,target_qpc=torch.tensor(unseen_until,device='cuda'),make_predictions=a<limit)
                    receipt.update(native_packet=header['packet'],native_frames=header['frames'],
                        wall_elapsed=time.perf_counter()-started,source_seconds=(b-HISTORY)/RATE,
                        captured_end=capture.end,backlog_frames=capture.end-b,
                        publication_qpc=issued.published_qpc if issued else None,
                        last_target_qpc=float(stamps[-1]),target_copied_qpc=header['copied_qpc'])
                    receipts.append(receipt);log.write(json.dumps(receipt)+'\n')
                    if (b-HISTORY)//RATE>(a-HISTORY)//RATE or b==end:
                        _,stats,counts=loop.issued_errors()
                        item=dict(source_end=b/RATE,version=loop.version,backlog_seconds=(capture.end-b)/RATE,
                            **lead_feedback(stats-previous_stats,counts-previous_counts))
                        bins.append(item);previous_stats=stats.copy();previous_counts=counts.copy()
                        print(json.dumps(item),flush=True)
            first=last
        if loop.corrected_cells!=loop.forecasts*HORIZON:raise RuntimeError('Uncorrected issued values after final real horizon')
    except BaseException as exc:
        failure=f'{type(exc).__name__}: {exc}';raise
    finally:
        try:capture.close()
        except BaseException as exc:
            failure=f'{type(exc).__name__}: {exc}';raise
        finally:
            log.flush();log.close()
            np.save(args.output/'audio.npy',np.concatenate(chunks) if chunks else np.empty((0,2),np.float32))
            if loop is not None:
                stereo_metrics,stats,counts=loop.issued_errors()
                pending=2*(loop.forecasts*HORIZON-loop.corrected_cells)
                save_checkpoint(args.output/'model_checkpoint.pt',model,optimizer,loop.version,
                    dict(complete=failure is None and first==end,pending_errors=pending,
                         resume='Model/optimizer only; new real history epoch. Pending issued values not persisted.'))
                np.savez(args.output/'per-lead.npz',statistics=stats,counts=counts,target_lead_samples=np.arange(HORIZON)+1)
                summary=dict(complete=failure is None and first==end,failure=failure,learning_rate=args.learning_rate,
                    parameters=sum(p.numel() for p in model.parameters()),training_updates=loop.version,
                    issued_forecasts=loop.forecasts,corrected_errors=2*loop.corrected_cells,
                    pending_errors=pending,processed_frames=loop.end,captured_frames=capture.end,
                    wall_seconds=time.perf_counter()-started,stereo_metrics=stereo_metrics,per_source_second=bins,
                    peak_allocated_bytes=torch.cuda.max_memory_allocated(),
                    source='Live default output, unmodified signed stereo; native packet/GPU completion transactions',
                    timing='Each origin uses only its own past. strict_future requires BOTH source presentation QPC and native capture-copy QPC after completed issuance.')
                save_new_json(args.output/'summary.json',summary)
                print(json.dumps({k:v for k,v in summary.items() if k!='per_source_second'}),flush=True)

if __name__=='__main__':main()
