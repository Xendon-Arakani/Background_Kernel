


extern "C" __global__ void decode_each_origin(
    const float* pcm, const float* weights, const float* base, float* output,
    int history, int horizon, int rows)
{
    const int t=blockIdx.x*blockDim.x+threadIdx.x;
    if (t>=rows+horizon-1) return;
    const int taps=history+horizon-1;
    double left=base[t*2],right=base[t*2+1];
    for (int r=0;r<rows;++r)
    {
        if (r)
        {
            const int delay=t-r+1;
            if (delay>=1 && delay<=taps)
            {
                left+=(double)weights[delay-1]*pcm[(history+r-1)*2];
                right+=(double)weights[taps+delay-1]*pcm[(history+r-1)*2+1];
            }
            const int expired=delay+history;
            if (expired>=1 && expired<=taps)
            {
                left-=(double)weights[expired-1]*pcm[(r-1)*2];
                right-=(double)weights[taps+expired-1]*pcm[(r-1)*2+1];
            }
        }
        const int h=t-r;
        if (h>=0 && h<horizon)
        {
            const long long index=((long long)r*horizon+h)*2;
            output[index]=(float)left;
            output[index+1]=(float)right;
        }
    }
}



extern "C" __global__ void advance_context(
    const double* initial, const float* incoming, const float* expired,
    const double* step, const double* eviction, const double* weights,
    double* states, int rows)
{
    const int mode = threadIdx.x;
    if (mode >= 32) return;
    double real = initial[mode * 2], imag = initial[mode * 2 + 1];
    const double ar = step[mode * 2], ai = step[mode * 2 + 1];
    const double er = eviction[mode * 2], ei = eviction[mode * 2 + 1];
    const double blr = weights[mode * 4], bli = weights[mode * 4 + 1];
    const double brr = weights[mode * 4 + 2], bri = weights[mode * 4 + 3];
    for (int row = 0; row < rows; ++row)
    {
        const double oldr = blr * expired[row * 2] + brr * expired[row * 2 + 1];
        const double oldi = bli * expired[row * 2] + bri * expired[row * 2 + 1];
        const double addr = blr * incoming[row * 2] + brr * incoming[row * 2 + 1] - (er * oldr - ei * oldi);
        const double addi = bli * incoming[row * 2] + bri * incoming[row * 2 + 1] - (ei * oldr + er * oldi);
        const double next = ar * real - ai * imag + addr;
        imag = ai * real + ar * imag + addi;
        real = next;
        states[row * 64 + mode * 2] = real;
        states[row * 64 + mode * 2 + 1] = imag;
    }
}


extern "C" __global__ void factor_decoder(
    const double* states, const float* weights, float* result, int rows)
{
    const int index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index >= rows * 16 * 32) return;
    const int mode = index % 32, hidden = (index / 32) % 16, row = index / (32 * 16);
    const double sr = states[row * 64 + mode * 2], si = states[row * 64 + mode * 2 + 1];
    const double wr = weights[hidden * 64 + mode * 2], wi = weights[hidden * 64 + mode * 2 + 1];
    result[index * 2] = (float)(wr * sr + wi * si);
    result[index * 2 + 1] = (float)(wi * sr - wr * si);
}


extern "C" __global__ void make_future_leads(
    const float* hidden, const float* hidden_bias, const float* output_weights,
    const float* output_bias, const float* direct, float* output, int rows, int horizon)
{
    const int h = blockIdx.x * blockDim.x + threadIdx.x;
    const int row = blockIdx.y;
    if (h >= horizon || row >= rows) return;
    const long long index = ((long long)row * horizon + h) * 2;
    float left = output_bias[0] + direct[index], right = output_bias[1] + direct[index+1];
    #pragma unroll
    for (int j = 0; j < 16; ++j)
    {
        const float activation = tanhf(hidden[(row * 16 + j) * horizon + h] + hidden_bias[j]);
        left = fmaf(output_weights[j], activation, left);
        right = fmaf(output_weights[16 + j], activation, right);
    }
    
    output[index] = tanhf(left);
    output[index + 1] = tanhf(right);
}
