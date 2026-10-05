#pragma once

namespace bk::render
{
    inline constexpr char DisplayMath[]=R"hlsl(
        float Pq(float nits) {
            float p=pow(saturate(nits/10000),2610.0/16384);
            return pow((3424.0/4096+(2413.0/128)*p)/(1+(2392.0/128)*p),2523.0/32);
        }
        float UnPq(float value) {
            float p=pow(saturate(value),32.0/2523);
            return 10000*pow(max(p-3424.0/4096,0)/max(2413.0/128-(2392.0/128)*p,1e-8),16384.0/2610);
        }
        float3 DisplayTransform(float3 nits) {
            float luminance=dot(nits,float3(0.2627,0.6780,0.0593));
            if(luminance<=0) return BlackNits.xxx/ScRgbWhiteNits;
            float pqBlack=Pq(0),pqWhite=Pq(10000),pqRange=pqWhite-pqBlack;
            float source=saturate((Pq(luminance)-pqBlack)/pqRange);
            float maximum=(Pq(PeakNits)-pqBlack)/pqRange;
            float minimum=(Pq(BlackNits)-pqBlack)/pqRange;
            float knee=clamp(1.5*maximum-0.5,0,0.999999),mapped=source;
            if(source>=knee && maximum<0.999999) {
                float t=saturate((source-knee)/(1-knee));
                float square=t*t;
                float cube=square*t;
                mapped=(2*cube-3*square+1)*knee+(cube-2*square+t)*(1-knee)+(-2*cube+3*square)*maximum;
            }
            mapped+=minimum*pow(1-mapped,4);
            float mappedLuminance=UnPq(clamp(mapped,minimum,maximum)*pqRange+pqBlack);
            nits*=mappedLuminance/luminance;
            float3 scRgbNits=float3(
                dot(nits,float3(1.660491,-0.587641,-0.072850)),
                dot(nits,float3(-0.124550,1.132900,-0.008349)),
                dot(nits,float3(-0.018151,-0.100579,1.118730)));
            return scRgbNits/ScRgbWhiteNits;
        }
    )hlsl";
}
