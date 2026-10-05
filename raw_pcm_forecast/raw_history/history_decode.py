
import math
import torch
import torch.nn.functional as F


def decode_origins(pcm, weight, history, rows, target_offset, columns):

    if pcm.ndim!=2 or pcm.shape[1]!=2 or len(pcm)<history+rows-1:
        raise ValueError("Direct decoder requires every raw stereo history sample")
    if rows<1 or columns<1 or target_offset<0 or weight.shape[0]!=2:
        raise ValueError("Invalid direct decoder source/target geometry")
    kernel=weight/math.sqrt(history)


    length=max(history+kernel.shape[1],history+target_offset+columns)
    fft_size=1 << (length-1).bit_length()
    source_spectrum=torch.fft.rfft(pcm[:history].T,n=fft_size)
    kernel_spectrum=torch.fft.rfft(F.pad(kernel,(1,0)),n=fft_size)
    convolution=torch.fft.irfft(source_spectrum*kernel_spectrum,n=fft_size)
    base=convolution[:,history+target_offset:history+target_offset+columns].T
    if rows==1:
        return base.unsqueeze(0).contiguous()
    target=target_offset+torch.arange(columns,device=pcm.device,dtype=torch.int64)
    incoming=torch.arange(rows-1,device=pcm.device,dtype=torch.int64)
    lag=target[None,:]-incoming[:,None]
    def read_weights(delay):
        valid=(delay>=1)&(delay<=kernel.shape[1])


        indices=(delay-1).clamp(0,kernel.shape[1]-1)
        return kernel[:,indices].permute(1,2,0)*valid[:,:,None]
    added=read_weights(lag)*pcm[history:history+rows-1,None,:]
    removed=read_weights(lag+history)*pcm[:rows-1,None,:]
    changes=torch.cumsum(added-removed,dim=0)
    return torch.cat((base[None,:,:],base[None,:,:]+changes),dim=0).contiguous()


def decode_one_future(history_pcm, weight):
    history=len(history_pcm)
    horizon=weight.shape[1]-history+1
    return decode_origins(history_pcm,weight,history,1,0,horizon)[0]
