

struct Z { double r, i; };
__device__ Z add(Z a, Z b) { return {a.r+b.r, a.i+b.i}; }
__device__ Z mul(Z a, Z b) { return {a.r*b.r-a.i*b.i, a.r*b.i+a.i*b.r}; }
__device__ Z scale(Z a, double b) { return {a.r*b, a.i*b}; }
__device__ Z get(const double* p, long long i) { return {p[2*i],p[2*i+1]}; }
__device__ void put(double* p, long long i, Z a) { p[2*i]=a.r; p[2*i+1]=a.i; }



extern "C" __global__ void scan_moment_warps(
    const float* pcm,const double* step,const double* eviction,
    double* local_o,double* local_v,double* sum_o,double* sum_v,
    int history,int updates,int rate)
{
    int lane=threadIdx.x%32,combo=blockIdx.y*8+threadIdx.x/32;
    int m=combo/2,c=combo%2,q=blockIdx.x*32+lane;
    Z u={0.,0.},v={0.,0.},power=get(step,m);
    if(q<updates) {
        Z e=get(eviction,m);
        double old=pcm[q*2+c];
        u=add({pcm[(history+q)*2+c],0.},scale(e,-old));
        v=scale(e,-(double)history/rate*old);
    }
    for(int distance=1;distance<32;distance*=2) {
        Z ou={__shfl_up_sync(0xffffffff,u.r,distance),__shfl_up_sync(0xffffffff,u.i,distance)};
        Z ov={__shfl_up_sync(0xffffffff,v.r,distance),__shfl_up_sync(0xffffffff,v.i,distance)};
        if(lane>=distance) {
            v=add(mul(power,add(ov,scale(ou,(double)distance/rate))),v);
            u=add(mul(power,ou),u);
        }
        power=mul(power,power);
    }
    if(q<updates) { put(local_o,(long long)q*64+combo,u); put(local_v,(long long)q*64+combo,v); }
    int count=min(32,updates-(int)blockIdx.x*32);
    if(lane==count-1) { put(sum_o,(long long)blockIdx.x*64+combo,u); put(sum_v,(long long)blockIdx.x*64+combo,v); }
}

extern "C" __global__ void carry_moment_warps(
    const double* initial,const double* weighted,const double* step,
    const double* sum_o,const double* sum_v,double* carry_o,double* carry_v,
    int warps,int rate)
{
    int combo=threadIdx.x,m=combo/2;
    Z o=get(initial,combo),v=get(weighted,combo),power=get(step,m);
    for(int i=0;i<5;++i) power=mul(power,power);
    for(int w=0;w<warps;++w) {
        long long p=(long long)w*64+combo;
        put(carry_o,p,o); put(carry_v,p,v);
        v=add(mul(power,add(v,scale(o,32./rate))),get(sum_v,p));
        o=add(mul(power,o),get(sum_o,p));
    }
}

extern "C" __global__ void finish_moment_scan(
    const double* initial,const double* weighted,const double* powers,
    const double* local_o,const double* local_v,const double* carry_o,const double* carry_v,
    const double* weights,double* ordinary,double* weighted_state,double* state,int rows,int rate)
{
    int index=blockIdx.x*blockDim.x+threadIdx.x;
    if(index>=rows*32) return;
    int row=index/32,m=index%32;
    Z s={0.,0.},ws={0.,0.};
    for(int c=0;c<2;++c) {
        int combo=m*2+c;
        Z o,v;
        if(!row) { o=get(initial,combo); v=get(weighted,combo); }
        else {
            int q=row-1,w=q/32,n=q%32+1;
            Z power=get(powers,m*32+n-1),co=get(carry_o,(long long)w*64+combo);
            o=add(mul(power,co),get(local_o,(long long)q*64+combo));
            v=add(mul(power,add(get(carry_v,(long long)w*64+combo),scale(co,(double)n/rate))),
                  get(local_v,(long long)q*64+combo));
        }
        put(ordinary,((long long)row*32+m)*2+c,o);
        Z b=get(weights,combo); s=add(s,mul(b,o)); ws=add(ws,mul(b,v));
    }
    put(state,index,s); put(weighted_state,index,ws);
}

extern "C" __global__ void factor_hidden(
    const double* states, const float* weights, float* result, int rows)
{
    int index=blockIdx.x*blockDim.x+threadIdx.x;
    if(index>=rows*16*32) return;
    int m=index%32,j=(index/32)%16,r=index/(32*16);
    double sr=states[r*64+m*2],si=states[r*64+m*2+1];
    double wr=weights[j*64+m*2],wi=weights[j*64+m*2+1];
    result[index*2]=(float)(wr*sr+wi*si);
    result[index*2+1]=(float)(wi*sr-wr*si);
}

extern "C" __global__ void mature_loss_adjoint(
    const float* hidden, const float* hb, const float* ow, const float* ob,
    const float* direct, const float* targets, const double* inverse_count,
    float* activation, float* dz, float* error, float* squared,
    int rows, int columns, int origin_relative, int horizon)
{
    int t=blockIdx.x*blockDim.x+threadIdx.x,r=blockIdx.y;
    if(t>=columns || r>=rows) return;
    const long long direct_index=((long long)r*columns+t)*2;
    float a[16],l=ob[0]+direct[direct_index],rr=ob[1]+direct[direct_index+1];
    #pragma unroll
    for(int j=0;j<16;++j) {
        a[j]=tanhf(hidden[(r*16+j)*columns+t]+hb[j]);
        l=fmaf(ow[j],a[j],l); rr=fmaf(ow[16+j],a[j],rr);
    }
    l=tanhf(l); rr=tanhf(rr);
    int h=t-origin_relative-r;
    bool valid=h>=0 && h<horizon;
    float el=valid ? l-targets[t*2] : 0.f;
    float er=valid ? rr-targets[t*2+1] : 0.f;
    squared[(long long)r*columns+t]=el*el+er*er;
    
    
    float dl=(float)(2.*inverse_count[0]*el)*(1.f-l*l);
    float dr=(float)(2.*inverse_count[0]*er)*(1.f-rr*rr);
    error[((long long)r*2)*columns+t]=dl;
    error[((long long)r*2+1)*columns+t]=dr;
    #pragma unroll
    for(int j=0;j<16;++j) {
        long long i=((long long)r*16+j)*columns+t;
        activation[i]=a[j];
        dz[i]=(ow[j]*dl+ow[16+j]*dr)*(1.f-a[j]*a[j]);
    }
}

extern "C" __global__ void state_adjoint(
    const float* coefficient_grad, const float* weights, double* result, int rows)
{
    int index=blockIdx.x*blockDim.x+threadIdx.x;
    if(index>=rows*32) return;
    int r=index/32,m=index%32;
    double sr=0.,si=0.;
    #pragma unroll
    for(int j=0;j<16;++j) {
        double wr=weights[j*64+m*2],wi=weights[j*64+m*2+1];
        double gr=coefficient_grad[(r*16+j)*64+m*2];
        double gi=coefficient_grad[(r*16+j)*64+m*2+1];
        sr+=wr*gr+wi*gi; si+=wi*gr-wr*gi;
    }
    result[index*2]=sr; result[index*2+1]=si;
}

extern "C" __global__ void hidden_adjoint(
    const float* coefficient_grad, const double* states, float* result, int rows)
{
    int j=blockIdx.x,m=blockIdx.y,tid=threadIdx.x;
    double real=0.,imag=0.;
    for(int r=tid;r<rows;r+=blockDim.x) {
        double sr=states[r*64+m*2],si=states[r*64+m*2+1];
        double gr=coefficient_grad[(r*16+j)*64+m*2];
        double gi=coefficient_grad[(r*16+j)*64+m*2+1];
        real+=sr*gr-si*gi; imag+=si*gr+sr*gi;
    }
    __shared__ double re[256],im[256];
    re[tid]=real; im[tid]=imag; __syncthreads();
    for(int offset=blockDim.x/2;offset;offset/=2) {
        if(tid<offset) { re[tid]+=re[tid+offset]; im[tid]+=im[tid+offset]; }
        __syncthreads();
    }
    if(!tid) { result[j*64+m*2]=(float)re[0]; result[j*64+m*2+1]=(float)im[0]; }
}
