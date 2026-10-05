

extern "C" __global__ void forecast(const float* pcm,const float* w,
    const float* bias,const float* base,float* output,int H,int F,int rows) {
    int t=blockIdx.x*blockDim.x+threadIdx.x;
    if(t>=rows+F-1) return;
    int K=H+F-1;
    double sums[2]={base[t*2],base[t*2+1]};
    for(int r=0;r<rows && r<=t;++r) {
        if(r) {
            int lag=t-r+1;
            for(int c=0;c<2;++c) for(int d=0;d<2;++d) {
                if(lag>=1 && lag<=K) sums[c]+=(double)w[(c*2+d)*K+lag-1]*pcm[(H+r-1)*2+d];
                if(lag+H>=1 && lag+H<=K) sums[c]-=(double)w[(c*2+d)*K+lag+H-1]*pcm[(r-1)*2+d];
            }
        }
        int h=t-r;
        if(h<F) for(int c=0;c<2;++c)
            output[((long long)r*F+h)*2+c]=tanhf((float)sums[c]+bias[c]);
    }
}




extern "C" __global__ void feedback(float* pending,const float* issued,
    const float* actual,float* delta,double* stats,long long* counts,
    const double* target_qpc,const double* issue_qpc,long long metadata_first,
    long long first_target,long long first_origin,long long last_origin,
    int F,int rows,int make_predictions,int need_gradient) {
    int h=blockIdx.x*blockDim.x+threadIdx.x;
    if(h>=F) return;
    long long offset=(long long)h*(h+1)/2;
    double sums[2][6]={}; 
    double strict[2][2]={};
    long long count=0,strict_count=0;
    for(int r=0;r<rows;++r) {
        long long t=first_target+r,a=t-h;
        long long cell=offset+t%(h+1);
        bool valid=a>=first_origin && a<=last_origin;
        bool future=valid && target_qpc[r]>issue_qpc[a-metadata_first];
        for(int c=0;c<2;++c) {
            double direction=0.;
            if(valid) {
                float p=pending[cell*2+c],y=actual[r*2+c];
                double e=(double)p-y;
                sums[c][0]+=e*e;sums[c][1]+=(double)y*y;
                sums[c][2]+=(double)p*p;sums[c][3]+=(double)p*y;
                sums[c][4]+=e;sums[c][5]+=2.*e;
                
                direction=2.*e*(1.-(double)p*p);
                if(future) { strict[c][0]+=e*e;strict[c][1]+=(double)y*y; }
            }
            if(need_gradient) delta[((long long)r*F+h)*2+c]=(float)direction;
            if(make_predictions) pending[cell*2+c]=issued[((long long)r*F+h)*2+c];
        }
        if(valid) ++count;
        if(future) ++strict_count;
    }
    counts[h*2]+=count;counts[h*2+1]+=strict_count;
    for(int c=0;c<2;++c) {
        for(int j=0;j<6;++j) stats[(h*2+c)*8+j]+=sums[c][j];
        stats[(h*2+c)*8+6]+=strict[c][0];stats[(h*2+c)*8+7]+=strict[c][1];
    }
}




extern "C" __global__ void boundary_gradient(const double* prefix,const float* pcm,
    float* gradient,const double* scale,int H,int F,int rows,int past) {
    int i=blockIdx.x*blockDim.x+threadIdx.x;
    int edges=2*(F-1);
    if(i>=edges*4) return;
    int pair=i/edges,q=i%edges,c=pair/2,d=pair%2;
    int lag=q<F-1 ? q+1 : H+q-(F-1)+1;
    int high=min(lag,F)-1,low=max(0,lag-H)-1;
    double sum=0.;
    for(int r=0;r<rows;++r) {
        int x=past+r-lag;
        if(x<0) continue; 
        double eligible=prefix[((long long)r*F+high)*2+c];
        if(low>=0) eligible-=prefix[((long long)r*F+low)*2+c];
        sum+=eligible*pcm[x*2+d];
    }
    gradient[(long long)pair*(H+F-1)+lag-1]=(float)(sum*scale[0]);
}
