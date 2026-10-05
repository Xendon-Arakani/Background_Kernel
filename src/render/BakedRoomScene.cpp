#include "render/BakedRoomScene.hpp"
#include "scene/BakedRoomLight.hpp"
#include "scene/TubeGeometry.hpp"
#include "scene/TubeBvh.hpp"
#include "stroke/RoomWallStroke.hpp"
#include <d3dcompiler.h>
#include <bit>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;
namespace bk::render
{
    namespace
    {
        constexpr char Shader[]=R"hlsl(
            static const float PI=3.14159265358979323846;
            cbuffer Room:register(b0) {
                float ArcWidthMm;
                float RoomHeightMm;
                float CurveRadiusMm;
                float BackWallMm;
                float ViewWidthPx;
                float ViewHeightPx;
                float HalfChordMm;
                float FrontEdgeZmm;
                float3 EyePointMm;
                float EyePad;
                float3 SpherePointMm;
                float SphereRadiusMm;
                float3 SphereCandela;
                float SphereEnabled;
                float SurfaceRoughness;
                float BaseReflectance;
                float NormalStrength;
                float PatchSide;
                float PaneDepthMm;
                float PaneIor;
                float PaneAbsorbPerMm;
                float PanePad;
                uint TubeCount;
                uint WallSide;
                uint TreeCount;
                uint DebugView;
                float WallTimeSec;
                float3 WallBands;
                float WallLevel;
                float WallBalance;
                float WallEntropy;
                uint WallMode;
                uint WallPasses;
                float3 WallPad;
                float StrokeScale;
                float StrokeWidth;
                float StrokeHeight;
                float StrokeOpacity;
                float StrokeTilePx;
                uint StrokeTileCols;
                uint StrokeTileRows;
                uint StrokeShapeCount;
                uint StrokeOn;
                float3 StrokePad;
            };
            struct Patch { float4 positionArea; float4 normal; float4 reflectance; float4 u; float4 v; };
            StructuredBuffer<Patch> Patches:register(t0);
            StructuredBuffer<float4> Response:register(t1);
            RWStructuredBuffer<float4> Direct:register(u0);
            RWStructuredBuffer<float4> Bounce:register(u1);
            struct Tube { float4 startPoint; float4 endPoint; float4 startNitMm; float4 endNitMm; float4 startSource; float4 endSource; };
            StructuredBuffer<Tube> Lines:register(t2);
            RWStructuredBuffer<float4> LineIrradiance:register(u2);
            RWStructuredBuffer<float> LineBudget:register(u3);
            struct WallSample { float4 position; float4 normal; };
            StructuredBuffer<WallSample> WallSamples:register(t3);
            StructuredBuffer<float> WallGather:register(t4);
            RWStructuredBuffer<float4> WallAtlas:register(u4);
            struct BvhNode { float4 lower;float4 upper;uint4 links; };
            StructuredBuffer<BvhNode> TubeTree:register(t5);
            StructuredBuffer<float> Factors:register(t6);
            RWStructuredBuffer<float4> BounceWork:register(u6);
            struct WallStroke {
                float4 start;float4 end;float4 startColor;float4 endColor;
            };
            StructuredBuffer<WallStroke> StrokeShapes:register(t7);
            StructuredBuffer<uint2> StrokeRanges:register(t8);
            StructuredBuffer<uint> StrokeIndices:register(t9);
            float StableLineMoment(float angle,float sineCosine) {
                if(angle>=1) return angle-sineCosine;
                float term=(2.0/3)*angle*angle*angle,sum=term;
                [loop] for(uint order=1;;++order) {
                    term*=(-4*angle*angle)/
                        ((2*order+2)*(2*order+3));
                    float next=sum+term;if(next==sum) return sum;sum=next;
                }
                return sum;
            }
            float3 IntegrateLine(Tube tube,float3 receiver,float3 normal) {
                float3 startOffset=tube.startPoint.xyz-receiver;
                float3 endOffset=tube.endPoint.xyz-receiver;
                float startHeight=dot(startOffset,normal);
                float endHeight=dot(endOffset,normal);
                if(startHeight<=0 && endHeight<=0) return 0;
                if(startHeight<0 || endHeight<0) {
                    float planeRatio=startHeight/(startHeight-endHeight);
                    float3 intersection=lerp(
                        startOffset,endOffset,planeRatio);
                    float3 emission=lerp(
                        tube.startNitMm.rgb,tube.endNitMm.rgb,planeRatio);
                    if(startHeight<0) {
                        startOffset=intersection;
                        tube.startNitMm.rgb=emission;
                    } else {
                        endOffset=intersection;
                        tube.endNitMm.rgb=emission;
                    }
                }
                float length=distance(startOffset,endOffset);
                if(length==0) return 0;
                float3 tangent=(endOffset-startOffset)/length;
                float startAlong=dot(startOffset,tangent);
                float endAlong=startAlong+length;
                float middleAlong=(startAlong+endAlong)/2;
                float3 perpendicular=startOffset-tangent*startAlong;
                float normalDistSq=dot(perpendicular,perpendicular);
                float normalDistance=sqrt(normalDistSq);
                if(normalDistance==0) return 0;
                float angleNumerator=normalDistance*length;
                float angleBase=normalDistSq+startAlong*endAlong;
                float denominator=angleNumerator*angleNumerator+
                    angleBase*angleBase;
                float angle=atan2(angleNumerator,angleBase);
                float scale=angleNumerator/denominator;
                float normalAcross=dot(normal,perpendicular);
                float normalAlong=dot(normal,tangent);
                float totalWeight=max(0,
                    normalAcross*(angle+scale*
                        (normalDistSq-startAlong*endAlong))/normalDistSq+
                    normalAlong*scale*(startAlong+endAlong));
                float moment=(normalAlong-
                    normalAcross*middleAlong/normalDistSq)*
                    StableLineMoment(angle,scale*angleBase)-
                    normalAlong*scale*(length*length/2);
                float endWeight=clamp(
                    totalWeight/2+moment/length,0,totalWeight);
                return tube.startNitMm.rgb*(totalWeight-endWeight)+
                    tube.endNitMm.rgb*endWeight;
            }
            float TriangleOmega(
                float3 firstRay,float3 nextRay,float3 lastRay) {
                return 2*atan2(
                    abs(dot(firstRay,cross(nextRay,lastRay))),
                    1+dot(firstRay,nextRay)+dot(nextRay,lastRay)+
                        dot(lastRay,firstRay));
            }
            float PatchOmega(Patch patch,float3 source) {
                float3 lowerLeft=normalize(
                    patch.positionArea.xyz-patch.u.xyz-patch.v.xyz-source);
                float3 lowerRight=normalize(
                    patch.positionArea.xyz+patch.u.xyz-patch.v.xyz-source);
                float3 upperRight=normalize(
                    patch.positionArea.xyz+patch.u.xyz+patch.v.xyz-source);
                float3 upperLeft=normalize(
                    patch.positionArea.xyz-patch.u.xyz+patch.v.xyz-source);
                return TriangleOmega(lowerLeft,lowerRight,upperRight)+
                    TriangleOmega(lowerLeft,upperRight,upperLeft);
            }
            float NoiseHash(int3 cell) {
                uint hash=(uint)cell.x*0x8da6b343u ^
                    (uint)cell.y*0xd8163841u ^
                    (uint)cell.z*0xcb1ab31fu;
                hash^=hash>>16;hash*=0x7feb352du;
                hash^=hash>>15;hash*=0x846ca68bu;hash^=hash>>16;
                return (hash>>8)/16777215.0;
            }
            float Noise(float3 position) {
                int3 cell=(int3)floor(position);
                float3 blend=frac(position);
                blend=blend*blend*(3-2*blend);
                return lerp(
                    lerp(
                        lerp(NoiseHash(cell),
                            NoiseHash(cell+int3(1,0,0)),blend.x),
                        lerp(NoiseHash(cell+int3(0,1,0)),
                            NoiseHash(cell+int3(1,1,0)),blend.x),blend.y),
                    lerp(
                        lerp(NoiseHash(cell+int3(0,0,1)),
                            NoiseHash(cell+int3(1,0,1)),blend.x),
                        lerp(NoiseHash(cell+int3(0,1,1)),
                            NoiseHash(cell+int3(1,1,1)),blend.x),blend.y),
                    blend.z);
            }
        )hlsl" R"hlsl(
            float2 StrokePoint(float3 surface,uint face) {
                float depth=BackWallMm-FrontEdgeZmm;
                float width=2*HalfChordMm;
                float paneRadius=CurveRadiusMm+PaneDepthMm;
                float paneArc=2*paneRadius*asin(HalfChordMm/paneRadius);
                if(face==0) return StrokeScale*float2(
                    depth+surface.x+HalfChordMm,
                    depth+RoomHeightMm*0.5-surface.y);
                if(face==1) return StrokeScale*float2(
                    surface.z-FrontEdgeZmm,
                    depth+RoomHeightMm*0.5-surface.y);
                if(face==2) return StrokeScale*float2(
                    depth+width+BackWallMm-surface.z,
                    depth+RoomHeightMm*0.5-surface.y);
                if(face==3) return StrokeScale*float2(
                    depth+surface.x+HalfChordMm,
                    surface.z-FrontEdgeZmm);
                if(face==5) {
                    float angle=atan2(surface.x,
                        surface.z+CurveRadiusMm);
                    float u=0.5+angle*paneRadius/paneArc;
                    return StrokeScale*float2(
                        2*depth+width+(1-u)*paneArc,
                        depth+RoomHeightMm*0.5-surface.y);
                }
                return StrokeScale*float2(
                    depth+surface.x+HalfChordMm,
                    depth+RoomHeightMm+BackWallMm-surface.z);
            }
            float4 StrokeAt(float3 surfacePos,uint face) {
                if(StrokeOn==0) return 0;
                float2 location=StrokePoint(surfacePos,face);
                if(any(location<0) || location.x>=StrokeWidth ||
                    location.y>=StrokeHeight) return 0;
                int2 tile=clamp((int2)floor(location/StrokeTilePx),0,
                    int2(StrokeTileCols-1,StrokeTileRows-1));
                uint2 range=StrokeRanges[tile.y*StrokeTileCols+tile.x];
                float3 source=0;
                float cover=0;
                float strongest=0;
                [loop] for(uint entry=range.y;entry>0;--entry) {
                    uint shapeIndex=StrokeIndices[range.x+entry-1];
                    if(shapeIndex>=StrokeShapeCount) continue;
                    WallStroke shape=StrokeShapes[shapeIndex];
                    if(shape.end.w<strongest) break;
                    uint faceMask=(uint)shape.startColor.w;
                    if(faceMask!=0 && (faceMask&(1u<<face))==0) continue;
                    float2 delta=shape.end.xy-shape.start.xy;
                    float extent=max(dot(delta,delta),1e-8);
                    float t=saturate(dot(location-shape.start.xy,delta)/extent);
                    float2 nearest=lerp(shape.start.xy,shape.end.xy,t);
                    float radius=lerp(shape.start.z,shape.end.z,t);
                    if(radius<=0) continue;
                    float distanceToLine=length(location-nearest);
                    float coverage=1-smoothstep(radius-0.7,radius+0.7,
                        distanceToLine);
                    float fade=lerp(shape.start.w,shape.end.w,t);
                    float strength=fade*coverage;
                    if(strength>strongest) {
                        source=lerp(shape.startColor.rgb,shape.endColor.rgb,t)*10000*fade;
                        cover=StrokeOpacity*strength;
                        strongest=strength;
                    }
                }
                return float4(min(source,10000),saturate(cover));
            }
            bool ClipStroke(float start,float delta,float low,float high,
                inout float from,inout float to) {
                if(abs(delta)<1e-6)
                    return start>=low && start<=high;
                float first=(low-start)/delta;
                float last=(high-start)/delta;
                if(first>last) {float swap=first;first=last;last=swap;}
                from=max(from,first);
                to=min(to,last);
                return from<=to;
            }
            float3 StrokePatch(Patch patch,uint face) {
                if(StrokeOn==0) return 0;
                float2 first=StrokePoint(
                    patch.positionArea.xyz-patch.u.xyz-patch.v.xyz,face);
                float2 second=StrokePoint(
                    patch.positionArea.xyz+patch.u.xyz-patch.v.xyz,face);
                float2 third=StrokePoint(
                    patch.positionArea.xyz-patch.u.xyz+patch.v.xyz,face);
                float2 fourth=StrokePoint(
                    patch.positionArea.xyz+patch.u.xyz+patch.v.xyz,face);
                float2 lower=min(min(first,second),min(third,fourth));
                float2 upper=max(max(first,second),max(third,fourth));
                int2 firstTile=clamp((int2)floor(lower/StrokeTilePx),0,
                    int2(StrokeTileCols-1,StrokeTileRows-1));
                int2 lastTile=clamp((int2)floor(upper/StrokeTilePx),0,
                    int2(StrokeTileCols-1,StrokeTileRows-1));
                float patchArea=max((upper.x-lower.x)*(upper.y-lower.y),1e-6);
                float3 emission=0;
                [loop] for(int tileY=firstTile.y;tileY<=lastTile.y;++tileY) {
                    [loop] for(int tileX=firstTile.x;tileX<=lastTile.x;++tileX) {
                        int2 tile=int2(tileX,tileY);
                        uint2 range=StrokeRanges[tileY*StrokeTileCols+tileX];
                        [loop] for(uint entry=0;entry<range.y;++entry) {
                            uint shapeIndex=StrokeIndices[range.x+entry];
                            if(shapeIndex>=StrokeShapeCount) continue;
                            WallStroke shape=StrokeShapes[shapeIndex];
                            uint faceMask=(uint)shape.startColor.w;
                            if(faceMask!=0 && (faceMask&(1u<<face))==0) continue;
                            float radius=max(shape.start.z,shape.end.z);
                            if(radius<=0) continue;
                            float2 shapeLower=min(shape.start.xy,shape.end.xy)-radius;
                            int2 shapeTile=clamp((int2)floor(
                                shapeLower/StrokeTilePx),0,
                                int2(StrokeTileCols-1,StrokeTileRows-1));
                            if(any(tile!=max(firstTile,shapeTile))) continue;
                            float2 delta=shape.end.xy-shape.start.xy;
                            float from=0,to=1;
                            if(!ClipStroke(shape.start.x,delta.x,lower.x,upper.x,
                                from,to) ||
                               !ClipStroke(shape.start.y,delta.y,lower.y,upper.y,
                                from,to)) continue;
                            float middle=(from+to)*0.5;
                            float width=2*lerp(shape.start.z,shape.end.z,middle);
                            float area=length(delta)*(to-from)*width;
                            float fade=lerp(shape.start.w,shape.end.w,middle);
                            float opacity=StrokeOpacity*fade;
                            emission+=lerp(shape.startColor.rgb,
                                shape.endColor.rgb,middle)*10000*fade*opacity*
                                min(area/patchArea,1);
                        }
                    }
                }
                return min(emission,10000);
            }
            struct WallState {
                float3 albedo;
                float roughness;
                float2 normal;
                float normalGain;
                float3 emission;
                float opacity;
            };
            WallState EvalWall(float3 surfacePos,uint face) {
                WallState wall;
                wall.albedo=float3(0.68,0.65,0.60)+
                    0.03*(2*Noise(surfacePos/100)-1);
                wall.roughness=SurfaceRoughness;
                wall.normal=2*float2(
                    Noise(surfacePos/10),
                    Noise(surfacePos/10+float3(17,31,47)))-1;
                wall.normalGain=NormalStrength;
                float4 paint=StrokeAt(surfacePos,face);
                wall.emission=paint.rgb;
                wall.opacity=paint.a;
                return wall;
            }
            float3 PatchReflect(Patch patch,uint face) {
                if(WallMode==0) return patch.reflectance.rgb;
                return (1-BaseReflectance)*
                    EvalWall(patch.positionArea.xyz,face).albedo;
            }
            [numthreads(64,1,1)] void LineBudgetCS(
                uint tubeIndex:SV_DispatchThreadID) {
                if(tubeIndex>=TubeCount) return;
                Tube emitter=Lines[tubeIndex];float3 captured=0;
                uint patchCount=5*(uint)PatchSide*(uint)PatchSide;
                for(uint patchIndex=0;patchIndex<patchCount;++patchIndex) {
                    Patch patch=Patches[patchIndex];
                    captured+=IntegrateLine(
                        emitter,patch.positionArea.xyz,patch.normal.xyz)*
                        patch.positionArea.w/1e6;
                }
                float3 emitted=PI*PI*distance(
                    emitter.startPoint.xyz,emitter.endPoint.xyz)*
                    (emitter.startNitMm.rgb+emitter.endNitMm.rgb)/1e6;
                float factor=1;
                [unroll] for(uint channel=0;channel<3;++channel)
                    if(captured[channel]>0)
                        factor=min(factor,emitted[channel]/captured[channel]);
                LineBudget[tubeIndex]=factor;
            }
            [numthreads(64,1,1)] void DirectCS(
                uint patchIndex:SV_DispatchThreadID) {
                uint patchCount=5*(uint)PatchSide*(uint)PatchSide;
                if(patchIndex>=patchCount) return;
                Patch patch=Patches[patchIndex];
                float3 tubeLight=0;
                for(uint tubeIndex=0;tubeIndex<TubeCount;++tubeIndex)
                    tubeLight+=LineBudget[tubeIndex]*IntegrateLine(
                        Lines[tubeIndex],patch.positionArea.xyz,patch.normal.xyz);
                LineIrradiance[patchIndex]=float4(tubeLight,0);
                uint face=patchIndex/((uint)PatchSide*(uint)PatchSide);
                float3 direct=PatchReflect(patch,face)*(
                    SphereCandela*(1e6*PatchOmega(patch,SpherePointMm)/
                        patch.positionArea.w)*SphereEnabled+tubeLight);
                if(StrokeOn!=0) direct+=PI*StrokePatch(patch,face);
                Direct[patchIndex]=float4(direct,0);
                if(WallMode!=0) Bounce[patchIndex]=float4(direct,0);
            }
            [numthreads(64,1,1)] void BounceCS(
                uint patchIndex:SV_DispatchThreadID) {
                uint patchCount=5*(uint)PatchSide*(uint)PatchSide;
                if(patchIndex>=patchCount) return;
                float3 result=0;
                for(uint sourceIndex=0;sourceIndex<patchCount;++sourceIndex)
                    result+=Response[patchIndex*patchCount+sourceIndex].rgb*
                        Direct[sourceIndex].rgb;
                Bounce[patchIndex]=float4(result,0);
            }
            [numthreads(64,1,1)] void BounceStepCS(
                uint patchIndex:SV_DispatchThreadID) {
                uint patchCount=5*(uint)PatchSide*(uint)PatchSide;
                if(patchIndex>=patchCount) return;
                float3 incident=0;
                for(uint sourceIndex=0;sourceIndex<patchCount;++sourceIndex)
                    incident+=Factors[
                        patchIndex*patchCount+sourceIndex]*
                        Bounce[sourceIndex].rgb;
                Patch patch=Patches[patchIndex];
                uint face=patchIndex/((uint)PatchSide*(uint)PatchSide);
                BounceWork[patchIndex]=float4(
                    Direct[patchIndex].rgb+
                    PatchReflect(patch,face)*incident,0);
            }
            [numthreads(64,1,1)] void GatherCS(
                uint sampleIndex:SV_DispatchThreadID) {
                uint side=WallSide;
                uint patchCount=5*(uint)PatchSide*(uint)PatchSide;
                if(sampleIndex>=5*side*side) return;
                WallSample receiver=WallSamples[sampleIndex];
                float3 irradiance=0;
                for(uint patchIndex=0;patchIndex<patchCount;++patchIndex)
                    irradiance+=WallGather[
                        sampleIndex*patchCount+patchIndex]*
                        (WallMode==0
                            ? Direct[patchIndex].rgb+Bounce[patchIndex].rgb
                            : Bounce[patchIndex].rgb);
                for(uint tubeIndex=0;tubeIndex<TubeCount;++tubeIndex)
                    irradiance+=LineBudget[tubeIndex]*IntegrateLine(
                        Lines[tubeIndex],receiver.position.xyz,
                        receiver.normal.xyz);
                WallAtlas[sampleIndex]=float4(irradiance,0);
            }
        )hlsl" R"hlsl(
            float4 FullscreenVS(uint vertex:SV_VertexID):SV_POSITION {
                return vertex==0?float4(-1,-1,0,1):vertex==1?float4(-1,3,0,1):float4(3,-1,0,1);
            }
            float3 Glass(float2 pixel) {
                float angle=(pixel.x/ViewWidthPx-0.5)*ArcWidthMm/CurveRadiusMm;
                return float3(CurveRadiusMm*sin(angle),(0.5-pixel.y/ViewHeightPx)*RoomHeightMm,CurveRadiusMm*(cos(angle)-1));
            }
            float3 LinearScene(float3 nits) { return nits/80; }
            float HitWalls(float3 origin,float3 direction,out uint face,out float3 normal) {
                float distance=direction.z>0
                    ? (BackWallMm-origin.z)/direction.z:3.402823e38;
                face=0; normal=float3(0,0,-1);
                if(direction.x!=0) {
                    float xDistance=((direction.x<0
                        ? -HalfChordMm:HalfChordMm)-origin.x)/direction.x;
                    if(xDistance>=0 && xDistance<distance) {
                        distance=xDistance; face=direction.x<0?1:2;
                        normal=float3(direction.x<0?1:-1,0,0);
                    }
                }
                if(direction.y!=0) {
                    float yDistance=((direction.y>0
                        ? RoomHeightMm/2:-RoomHeightMm/2)-origin.y)/direction.y;
                    if(yDistance>=0 && yDistance<distance) {
                        distance=yDistance; face=direction.y>0?3:4;
                        normal=float3(0,direction.y>0?-1:1,0);
                    }
                }
                return distance;
            }
            float2 WallUV(float3 wallPoint,uint face) {
                if(face==0) return float2(
                    (wallPoint.x/HalfChordMm+1)*0.5,
                    wallPoint.y/RoomHeightMm+0.5);
                if(face<3) return float2(
                    (wallPoint.z-FrontEdgeZmm)/(BackWallMm-FrontEdgeZmm),
                    wallPoint.y/RoomHeightMm+0.5);
                return float2(
                    (wallPoint.x/HalfChordMm+1)*0.5,
                    (wallPoint.z-FrontEdgeZmm)/(BackWallMm-FrontEdgeZmm));
            }
            float4 SplineWeights(float fraction) {
                float complement=1-fraction;
                return float4(
                    complement*complement*complement,
                    3*fraction*fraction*fraction-
                        6*fraction*fraction+4,
                    -3*fraction*fraction*fraction+
                        3*fraction*fraction+3*fraction+1,
                    fraction*fraction*fraction)/6;
            }
            float3 WallLight(float2 wallUv,uint face) {
                int side=(int)WallSide;
                float2 gridPoint=saturate(wallUv)*side-0.5;
                int2 cell=(int2)floor(gridPoint);
                float4 xWeights=SplineWeights(frac(gridPoint.x));
                float4 yWeights=SplineWeights(frac(gridPoint.y));
                float3 light=0;uint base=face*side*side;
                [unroll] for(int yTap=0;yTap<4;++yTap)
                [unroll] for(int xTap=0;xTap<4;++xTap) {
                    int2 node=clamp(
                        cell+int2(xTap-1,yTap-1),0,side-1);
                    light+=xWeights[xTap]*yWeights[yTap]*
                        WallAtlas[base+node.y*side+node.x].rgb;
                }
                return light;
            }
            float3 WallRadiance(float3 origin,float3 ray) {
                float3 normal;
                uint face;
                float wallDistance=HitWalls(origin,ray,face,normal);
                float3 wallPoint=origin+wallDistance*ray;
                float3 lightDelta=SpherePointMm-wallPoint;
                float lightDistSq=dot(lightDelta,lightDelta);
                float invLightDist=rsqrt(lightDistSq);
                float3 lightDir=lightDelta*invLightDist;
                float3 viewDir=-ray;
                float3 halfVector=normalize(lightDir+viewDir);
                float3 tangent=face==0 || face>=3?float3(1,0,0):float3(0,0,1);
                float3 bitangent=cross(normal,tangent);
                WallState wall=EvalWall(wallPoint,face);
                normal=normalize(normal+wall.normalGain*(
                    tangent*wall.normal.x+bitangent*wall.normal.y));
                float lightCos=saturate(dot(normal,lightDir));
                float viewCos=saturate(dot(normal,viewDir));
                float halfCos=saturate(dot(normal,halfVector));
                float viewHalfCos=saturate(dot(viewDir,halfVector));
                float alpha=wall.roughness*wall.roughness;
                float alphaSq=alpha*alpha;
                float denominator=halfCos*halfCos*(alphaSq-1)+1;
                float distribution=alphaSq/(PI*denominator*denominator);
                float viewMask=2*viewCos/(viewCos+sqrt(
                    alphaSq+(1-alphaSq)*viewCos*viewCos));
                float lightMask=2*lightCos/(lightCos+sqrt(
                    alphaSq+(1-alphaSq)*lightCos*lightCos));
                float fresnel=BaseReflectance+
                    (1-BaseReflectance)*pow(1-viewHalfCos,5);
                float specular=viewCos>0 && lightCos>0
                    ? distribution*viewMask*lightMask*fresnel/
                        (4*viewCos*lightCos):0;
                float3 direct=((1-fresnel)*wall.albedo/PI+specular)*
                    SphereCandela*(1e6*lightCos/lightDistSq)*SphereEnabled;
                float2 wallUv=WallUV(wallPoint,face);
                float3 indirect=(1-BaseReflectance)*wall.albedo*
                    WallLight(wallUv,face)/PI;
                return LinearScene(wall.opacity*wall.emission+
                    (1-wall.opacity)*(direct+indirect));
            }
            float SphereHit(float3 origin,float3 ray) {
                float3 toLight=SpherePointMm-origin;
                float along=dot(toLight,ray);
                float3 perpendicular=cross(toLight,ray);
                float discriminant=SphereRadiusMm*SphereRadiusMm-dot(perpendicular,perpendicular);
                return discriminant>=0?along-sqrt(discriminant):-1;
            }
)hlsl" R"hlsl(
            float2 BallSpan(float3 origin,float3 ray,float4 ball) {
                float3 centerDelta=ball.xyz-origin;
                float along=dot(centerDelta,ray);
                float3 crossDistance=cross(centerDelta,ray);
                float discriminant=ball.w*ball.w-dot(crossDistance,crossDistance);
                if(discriminant<0) return float2(3.402823e38,-3.402823e38);
                float root=sqrt(discriminant);
                return float2(along-root,along+root);
            }
            void AcceptTubeRoot(
                float root,float shift,float axialOffset,float axialRate,
                float radius,float radiusRate,float length,float slopeGap,
                inout float2 span,inout float parameter) {
                float tubeRatio=(axialOffset+root*axialRate+
                    radius*radiusRate)/(length*slopeGap);
                float distance=root+shift;
                if(tubeRatio>=0 && tubeRatio<=1) {
                    if(distance<span.x) parameter=tubeRatio;
                    span=float2(min(span.x,distance),max(span.y,distance));
                }
            }
            float2 TubeSpan(float3 origin,float3 ray,Tube tube,out float parameter) {
                float2 span=BallSpan(origin,ray,tube.startPoint);parameter=0;
                float2 last=BallSpan(origin,ray,tube.endPoint);
                if(last.x<span.x) parameter=1;
                span=float2(min(span.x,last.x),max(span.y,last.y));
                float3 delta=tube.endPoint.xyz-tube.startPoint.xyz;
                float length=sqrt(dot(delta,delta));
                float radiusDelta=tube.endPoint.w-tube.startPoint.w;
                if(length<=abs(radiusDelta)) {
                    span.x=max(span.x,0);
                    return span;
                }
                float3 tangent=delta/length;
                float shift=dot(tube.startPoint.xyz-origin,ray);
                float3 rayOffset=cross(
                    ray,cross(origin-tube.startPoint.xyz,ray));
                float axialOffset=dot(rayOffset,tangent);
                float axialRate=dot(ray,tangent);
                float radiusRate=radiusDelta/length;
                float slopeGap=1-radiusRate*radiusRate;
                float3 perpendicular=rayOffset-tangent*axialOffset;
                float3 perpRay=ray-tangent*axialRate;
                float radius=tube.startPoint.w+radiusRate*axialOffset;
                float radiusDrift=radiusRate*axialRate;
                float quadratic=dot(perpRay,perpRay)-
                    radiusDrift*radiusDrift/slopeGap;
                float linearTerm=dot(perpendicular,perpRay)-
                    radius*radiusDrift/slopeGap;
                float constant=dot(perpendicular,perpendicular)-
                    radius*radius/slopeGap;
                if(quadratic==0) {
                    if(linearTerm!=0) AcceptTubeRoot(
                        -constant/(2*linearTerm),shift,axialOffset,axialRate,
                        tube.startPoint.w,radiusRate,length,slopeGap,
                        span,parameter);
                } else {
                    float discriminant=
                        linearTerm*linearTerm-quadratic*constant;
                    if(discriminant>=0) {
                        float root=sqrt(discriminant);
                        AcceptTubeRoot(
                            (-linearTerm-root)/quadratic,shift,
                            axialOffset,axialRate,tube.startPoint.w,
                            radiusRate,length,slopeGap,span,parameter);
                        AcceptTubeRoot(
                            (-linearTerm+root)/quadratic,shift,
                            axialOffset,axialRate,tube.startPoint.w,
                            radiusRate,length,slopeGap,span,parameter);
                    }
                }
                span.x=max(span.x,0);
                return span;
            }
        )hlsl"
        R"hlsl(

            bool Dielectric(
                float3 incoming,float3 incidentNormal,
                float sourceIor,float targetIor,
                out float3 transmitted,out float reflection) {
                float incidentCos=saturate(-dot(incoming,incidentNormal));
                float iorRatio=sourceIor/targetIor;
                float transmitSinSq=iorRatio*iorRatio*
                    max(0,1-incidentCos*incidentCos);
                transmitted=0;reflection=1;
                if(transmitSinSq>=1) return false;
                float transmitCos=sqrt(1-transmitSinSq);
                float sReflect=(sourceIor*incidentCos-
                    targetIor*transmitCos)/
                    (sourceIor*incidentCos+targetIor*transmitCos);
                float pReflect=(targetIor*incidentCos-
                    sourceIor*transmitCos)/
                    (targetIor*incidentCos+sourceIor*transmitCos);
                reflection=(sReflect*sReflect+pReflect*pReflect)/2;
                transmitted=normalize(iorRatio*incoming+
                    (iorRatio*incidentCos-transmitCos)*incidentNormal);
                return true;
            }
            float CylinderExit(float3 origin,float3 ray,float radius) {
                float2 centerOffset=origin.xz+float2(0,CurveRadiusMm);
                float2 planarRay=ray.xz;
                float rayLengthSq=dot(planarRay,planarRay);
                float offsetRayDot=dot(centerOffset,planarRay);
                float radiusDelta=dot(centerOffset,centerOffset)-radius*radius;
                return -radiusDelta/(offsetRayDot+sqrt(max(0,
                    offsetRayDot*offsetRayDot-rayLengthSq*radiusDelta)));
            }
            bool ThroughPane(float2 pixel,out float3 origin,out float3 ray,out float transmission) {
                float3 front=Glass(pixel),incoming=normalize(front-EyePointMm);
                origin=front;ray=incoming;transmission=0;
                if(DebugView==3) { transmission=1;return true; }
                float3 radial=normalize(
                    float3(front.x,0,front.z+CurveRadiusMm));
                float3 inside;
                float entryReflect;
                if(!Dielectric(
                    incoming,-radial,1,PaneIor,inside,entryReflect))
                    return false;
                float farDist=CylinderExit(front,inside,
                    CurveRadiusMm+PaneDepthMm);
        )hlsl" R"hlsl(
                float edgeDist=farDist;
                uint edgeAxis=2;
                if(inside.x!=0) {
                    float next=((inside.x>0?HalfChordMm:-HalfChordMm)-front.x)/inside.x;
                    if(next>=0 && next<edgeDist) {edgeDist=next;edgeAxis=0;}
                }
                if(inside.y!=0) {
                    float next=((inside.y>0?RoomHeightMm/2:-RoomHeightMm/2)-front.y)/inside.y;
                    if(next>=0 && next<edgeDist) {edgeDist=next;edgeAxis=1;}
                }
                if(edgeAxis<2) {
                    origin=front+inside*edgeDist;
                    if(edgeAxis==0)
                        origin.x=inside.x>0?HalfChordMm:-HalfChordMm;
                    else origin.y=inside.y>0?RoomHeightMm/2:-RoomHeightMm/2;
                    ray=inside;
                    transmission=(1-entryReflect)*
                        exp(-PaneAbsorbPerMm*edgeDist);
                    return true;
                }
                origin=front+inside*farDist;
                float radius=length(origin.xz+float2(0,CurveRadiusMm));
                radial=float3(origin.x,0,origin.z+CurveRadiusMm)/radius;
                float exitReflect;
                if(!Dielectric(
                    inside,-radial,PaneIor,1,ray,exitReflect)) return false;
                transmission=(1-entryReflect)*(1-exitReflect)*
                    exp(-PaneAbsorbPerMm*farDist);
                return true;
            }
            bool BoxHit(
                float3 origin,float3 ray,float3 lower,float3 upper,
                float minimum,float maximum) {
                float first=minimum,last=maximum;
                [unroll] for(uint axis=0;axis<3;++axis) {
                    if(ray[axis]==0) {
                        if(origin[axis]<lower[axis] ||
                            origin[axis]>upper[axis]) return false;
                    }
                    else {
                        float entryDist=(lower[axis]-origin[axis])/ray[axis];
                        float exitDist=(upper[axis]-origin[axis])/ray[axis];
                        first=max(first,min(entryDist,exitDist));
                        last=min(last,max(entryDist,exitDist));
                    }
                }
                return last>=first;
            }
            float3 SceneRay(float3 origin,float3 ray) {
                if(DebugView>=2) {
                    float backScale=(BackWallMm-origin.z)/ray.z;
                    float2 backPoint=origin.xy+backScale*ray.xy;
                    float checker=fmod(abs(
                        floor(backPoint.x/12)+floor(backPoint.y/12)),2);
                    return lerp(float3(0.05,0.10,0.16),float3(0.9,0.75,0.4),checker);
                }
                if(abs(origin.x)>HalfChordMm || abs(origin.y)>RoomHeightMm/2) return 0;
                float3 normal;uint face;float closest=HitWalls(origin,ray,face,normal);
                if(closest<0) return 0;
                float3 background=0;bool sphere=false;
                if(SphereEnabled>0) {
                    float hit=SphereHit(origin,ray);
                    if(hit>0 && hit<=closest) {
                        closest=hit;sphere=true;
                        background=LinearScene(SphereCandela*1e6/(PI*SphereRadiusMm*SphereRadiusMm));
                    }
                }
                float cursor=0,remaining=1;
                float3 result=0;
                [loop] while(cursor<closest && remaining>0) {
                    float first=closest,last=cursor;
                    float4 source=0;
                    uint node=0;
                    [loop] while(node<TreeCount) {
                        BvhNode boundsNode=TubeTree[node];
                        if(!BoxHit(origin,ray,boundsNode.lower.xyz,
                            boundsNode.upper.xyz,cursor,first)) {
                            node=boundsNode.links.x;continue;
                        }
                        if(boundsNode.links.y) {
                            Tube tube=Lines[boundsNode.links.y-1];
                            float parameter;
                            float2 span=TubeSpan(origin,ray,tube,parameter);
                            if(span.x>=cursor && span.y>cursor && span.x<first) {
                                first=span.x;last=span.y;
                                source=lerp(tube.startSource,tube.endSource,parameter);
                            }
                        }
                        ++node;
                    }
                    if(first>=closest) break;
                    float alpha=saturate(source.a);
                    result+=remaining*LinearScene(source.rgb)*alpha;
                    remaining*=1-alpha;
                    if(remaining==0) break;
                    last=min(last,closest);
                    bool expanded=true;
                    [loop] while(expanded && last<closest) {
                        expanded=false;node=0;
                        [loop] while(node<TreeCount) {
                            BvhNode boundsNode=TubeTree[node];
                            if(!BoxHit(origin,ray,boundsNode.lower.xyz,
                                boundsNode.upper.xyz,first,last)) {
                                node=boundsNode.links.x;continue;
                            }
                            if(boundsNode.links.y) {
                                float parameter;
                                float2 span=TubeSpan(origin,ray,
                                    Lines[boundsNode.links.y-1],parameter);
                                if(span.x<=last && span.y>last) {
                                    last=min(span.y,closest);expanded=true;
                                }
                            }
                            ++node;
                        }
                    }
                    cursor=last;
                }
                if(remaining>0)
                    result+=remaining*(sphere?background:WallRadiance(origin,ray));
                return result;
            }
            float4 ScenePS(float4 pixel:SV_POSITION):SV_TARGET {
                if(DebugView==4) {
                    float3 origin,ray;float transmission;
                    if(!ThroughPane(pixel.xy,origin,ray,transmission)) return float4(0,0,0,1);
                    return float4(0.5+ray*0.5,1);
                }
                const float2 coverageOffsets[4]={
                    float2(-0.25,-0.25),float2(0.25,-0.25),
                    float2(-0.25,0.25),float2(0.25,0.25)};
                float3 result=0;
                [unroll] for(uint sampleIndex=0;sampleIndex<4;++sampleIndex) {
                    float2 samplePixel=pixel.xy+coverageOffsets[sampleIndex];
                    float3 origin,ray;float transmission;
                    if(ThroughPane(
                        samplePixel,
                        origin,ray,transmission))
                    {
                        float4 glassPaint=DebugView==0?StrokeAt(origin,5):0;
                        float3 behind=DebugView==1?float3(1,1,1):SceneRay(origin,ray);
                        result+=transmission*(glassPaint.a*
                            LinearScene(glassPaint.rgb)+
                            (1-glassPaint.a)*behind)/4;
                    }
                }
                return float4(result,1);
            }
        )hlsl";
        void Check(HRESULT hr,const char* operation) { if(FAILED(hr)) throw std::runtime_error(std::string(operation)+": "+std::to_string(hr)); }
        ComPtr<ID3DBlob> Compile(const char* entry,const char* profile)
        {
            ComPtr<ID3DBlob> code,error;
            const auto hr=D3DCompile(Shader,std::strlen(Shader),"PhysicalRoom",nullptr,nullptr,entry,profile,
                D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
            if(FAILED(hr)&&error) throw std::runtime_error(std::string(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()));
            Check(hr,"Compile room shader"); return code;
        }
        ComPtr<ID3D12Resource> Buffer(ID3D12Device* device,std::size_t bytes,bool upload,const void* initial=nullptr)
        {
            D3D12_HEAP_PROPERTIES heap{}; heap.Type=upload?D3D12_HEAP_TYPE_UPLOAD:D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=bytes;
            desc.Height=1;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags=upload?D3D12_RESOURCE_FLAG_NONE:D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ComPtr<ID3D12Resource> result;
            Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,upload?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr,IID_PPV_ARGS(&result)),"Create room buffer");
            if(initial) { void* mapped{};D3D12_RANGE read{0,0};Check(result->Map(0,&read,&mapped),"Map room upload");std::memcpy(mapped,initial,bytes);result->Unmap(0,nullptr); }
            return result;
        }
        void Barrier(ID3D12GraphicsCommandList* commands,ID3D12Resource* resource)
        { D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=resource;commands->ResourceBarrier(1,&b); }
        struct Constants {
            float arcWidthMm,roomHeightMm,curveRadiusMm,backWallMm;
            float viewWidthPx,viewHeightPx,halfChordMm,frontEdgeZmm;
            float eyeXmm,eyeYmm,eyeZmm,eyePad;
            float sphereXmm,sphereYmm,sphereZmm,sphereRadiusMm;
            float sphereRedCd,sphereGreenCd,sphereBlueCd,sphereEnabled;
            float roughness,baseReflectance,normalStrength,patchSide;
            float paneDepthMm,paneIor,paneAbsorbPerMm,panePad;
            std::uint32_t tubeCount,wallSide,treeCount,debugView;
            float wallTimeSec,wallBands[3];
            float wallLevel,wallBalance,wallEntropy;
            std::uint32_t wallMode;
            std::uint32_t wallPasses;
            float wallPad[3];
            float strokeScale,strokeWidth,strokeHeight,strokeOpacity;
            float strokeTilePx;
            std::uint32_t strokeTileCols,strokeTileRows,strokeShapeCount,strokeOn;
            float strokePad[3];
        };
        struct GpuLine {
            float startPoint[4],endPoint[4];
            float startNitMm[4],endNitMm[4];
            float startSource[4],endSource[4];
        };
        struct GpuWallSample { float position[4],normal[4]; };
        struct GpuPatch { float position[4],normal[4],reflectance[4],u[4],v[4]; };
    }
    void BakedRoomScene::ValidateShaders()
    { Compile("LineBudgetCS","cs_5_1");Compile("DirectCS","cs_5_1");Compile("BounceCS","cs_5_1");Compile("BounceStepCS","cs_5_1");Compile("GatherCS","cs_5_1");Compile("FullscreenVS","vs_5_1");Compile("ScenePS","ps_5_1"); }
    void BakedRoomScene::Initialize(ID3D12Device* device)
    {
        device_=device;
        const auto& transport=scene::BuildTransport();
        std::vector<GpuPatch> patchData;
        for(const auto& p:transport.patches) patchData.push_back({
            {static_cast<float>(p.center.x),static_cast<float>(p.center.y),static_cast<float>(p.center.z),static_cast<float>(p.area)},
            {static_cast<float>(p.normal.x),static_cast<float>(p.normal.y),static_cast<float>(p.normal.z),0},
            {static_cast<float>(p.reflectance[0]),static_cast<float>(p.reflectance[1]),static_cast<float>(p.reflectance[2]),0},
            {static_cast<float>(p.u.x),static_cast<float>(p.u.y),static_cast<float>(p.u.z),0},{static_cast<float>(p.v.x),static_cast<float>(p.v.y),static_cast<float>(p.v.z),0}});
        const auto immutable=[&](std::size_t bytes,const void* data) {
            auto upload=Buffer(device,bytes,true,data);
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
            auto desc=upload->GetDesc();desc.Flags=D3D12_RESOURCE_FLAG_NONE;
            ComPtr<ID3D12Resource> gpu;
            Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&gpu)),"Create GPU-local transport");
            staticUploads_.push_back({upload,gpu});return gpu;
        };
        patches_=immutable(patchData.size()*sizeof(GpuPatch),patchData.data());
        response_=immutable(transport.bounceResponse.size()*sizeof(transport.bounceResponse[0]),transport.bounceResponse.data());
        std::vector<float> factors;
        factors.reserve(transport.formFactors.size());
        for(const double factor:transport.formFactors)
            factors.push_back(static_cast<float>(factor));
        factors_=immutable(factors.size()*sizeof(float),factors.data());
        wallGather_=immutable(transport.pointFactors.size()*sizeof(float),transport.pointFactors.data());
        std::vector<GpuWallSample> samples;
        for(const auto& p:transport.wallSamples) samples.push_back({
            {static_cast<float>(p.center.x),static_cast<float>(p.center.y),static_cast<float>(p.center.z),0},
            {static_cast<float>(p.normal.x),static_cast<float>(p.normal.y),static_cast<float>(p.normal.z),0}});
        wallSamples_=immutable(samples.size()*sizeof(GpuWallSample),samples.data());
        wallAtlas_=Buffer(device,scene::LightingCount*16,false);
        direct_=Buffer(device,scene::PatchCount*16,false);bounce_=Buffer(device,scene::PatchCount*16,false);
        bounceWork_=Buffer(device,scene::PatchCount*16,false);
        lineIrradiance_=Buffer(device,scene::PatchCount*16,false);
        constants_=Buffer(device,2*256,true);
        D3D12_RANGE noRead{0,0};Check(constants_->Map(0,&noRead,reinterpret_cast<void**>(&mapped_)),"Map room constants");
        D3D12_ROOT_PARAMETER parameters[17]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[1].ParameterType=parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[2].Descriptor.ShaderRegister=1;
        parameters[3].ParameterType=parameters[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[4].Descriptor.ShaderRegister=1;
        parameters[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[5].Descriptor.ShaderRegister=2;
        parameters[6].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[6].Descriptor.ShaderRegister=2;
        parameters[7].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[7].Descriptor.ShaderRegister=3;
        parameters[8].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[8].Descriptor.ShaderRegister=3;
        parameters[9].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[9].Descriptor.ShaderRegister=4;
        parameters[10].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[10].Descriptor.ShaderRegister=4;
        parameters[11].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[11].Descriptor.ShaderRegister=5;
        parameters[12].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[12].Descriptor.ShaderRegister=6;
        parameters[13].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[13].Descriptor.ShaderRegister=6;
        for(std::uint32_t index=14;index<17;++index)
        {
            parameters[index].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
            parameters[index].Descriptor.ShaderRegister=index-7;
        }
        D3D12_ROOT_SIGNATURE_DESC root{};root.NumParameters=17;root.pParameters=parameters;
        ComPtr<ID3DBlob> serialized,error;
        Check(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error),"Serialize room root");
        Check(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root_)),"Create room root");
        D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};compute.pRootSignature=root_.Get();
        const auto budget=Compile("LineBudgetCS","cs_5_1"),direct=Compile("DirectCS","cs_5_1"),bounce=Compile("BounceCS","cs_5_1"),bounceStep=Compile("BounceStepCS","cs_5_1");
        compute.CS={budget->GetBufferPointer(),budget->GetBufferSize()};Check(device->CreateComputePipelineState(&compute,IID_PPV_ARGS(&lineBudgetPipeline_)),"Create emitter energy conservation pass");
        compute.CS={direct->GetBufferPointer(),direct->GetBufferSize()};Check(device->CreateComputePipelineState(&compute,IID_PPV_ARGS(&directPipeline_)),"Create direct lighting");
        compute.CS={bounce->GetBufferPointer(),bounce->GetBufferSize()};Check(device->CreateComputePipelineState(&compute,IID_PPV_ARGS(&bouncePipeline_)),"Create diffuse transport");
        compute.CS={bounceStep->GetBufferPointer(),bounceStep->GetBufferSize()};Check(device->CreateComputePipelineState(&compute,IID_PPV_ARGS(&bounceStepPipe_)),"Create changing diffuse transport");
        const auto gather=Compile("GatherCS","cs_5_1");
        compute.CS={gather->GetBufferPointer(),gather->GetBufferSize()};Check(device->CreateComputePipelineState(&compute,IID_PPV_ARGS(&gatherPipeline_)),"Create dense diffuse gather");
        const auto vs=Compile("FullscreenVS","vs_5_1"),scene=Compile("ScenePS","ps_5_1");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};graphics.pRootSignature=root_.Get();
        graphics.VS={vs->GetBufferPointer(),vs->GetBufferSize()};graphics.PS={scene->GetBufferPointer(),scene->GetBufferSize()};
        graphics.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        graphics.SampleMask=UINT_MAX;graphics.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;graphics.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;graphics.RasterizerState.DepthClipEnable=TRUE;
        graphics.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;graphics.NumRenderTargets=1;graphics.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;graphics.SampleDesc.Count=1;
        Check(device->CreateGraphicsPipelineState(&graphics,IID_PPV_ARGS(&scenePipeline_)),"Create refracted physical scene");

    }
    void BakedRoomScene::UpdateAfterGpu(std::uint32_t slot,const visualizer::TrailFrame* frame,std::uint32_t width,std::uint32_t height,scene::EyePose eye,
                          std::span<const visualizer::TrailPoint> trail,std::span<const visualizer::TrailPoint> sweep,const scene::WallField& wallField,const stroke::View* strokeView)
    {
        if(!width || !height) throw std::runtime_error("Physical room requires a nonempty viewport");
        if(!scene::ValidWallField(wallField))
            throw std::invalid_argument("Invalid wall field");
        slot_=slot;width_=width;height_=height;eye_=eye;
        wallMode_=wallField.mode;wallPasses_=wallField.passes;
        scene::Light light;light.position={0,0,scene::BaseLightZmm};light.radius=1;
        if(frame) light=scene::MakeLight(frame->dotXmm,frame->dotYmm,2*frame->dotRadiusMm,{frame->red,frame->green,frame->blue},frame->dotZmm);
        physicalLight_=light;
        const auto worldLines=scene::BuildLineLights(trail,sweep);
        const auto tree=scene::BuildTubeBvh(worldLines);treeCount_=static_cast<std::uint32_t>(tree.size());
        const auto treeCapacity=std::bit_ceil(
            std::max<std::size_t>(1,tree.size()));
        if(treeCapacity>treeCapacity_[slot]) {
            tubeTrees_[slot]=Buffer(device_.Get(),treeCapacity*sizeof(scene::TubeBvhNode),true);treeCapacity_[slot]=treeCapacity;
        }
        if(!tree.empty()) {
            void* nodes{};D3D12_RANGE noRead{0,0};Check(tubeTrees_[slot]->Map(0,&noRead,&nodes),"Map refracted-visibility BVH");
            std::memcpy(nodes,tree.data(),tree.size()*sizeof(scene::TubeBvhNode));tubeTrees_[slot]->Unmap(0,nullptr);
        }
        if(worldLines.size()>std::numeric_limits<UINT>::max()) throw std::runtime_error("Too many world trail segments for DX12");
        lineCount_=static_cast<std::uint32_t>(worldLines.size());
        const auto capacity=std::bit_ceil(
            std::max<std::size_t>(1,worldLines.size()));
        if(capacity>lineCapacity_[slot]) { lines_[slot]=Buffer(device_.Get(),capacity*sizeof(GpuLine),true);lineBudgets_[slot]=Buffer(device_.Get(),capacity*sizeof(float),false);lineCapacity_[slot]=capacity; }
        void* upload{};D3D12_RANGE noRead{0,0};Check(lines_[slot]->Map(0,&noRead,&upload),"Map tube upload");
        auto* destination=static_cast<GpuLine*>(upload);
        for(const auto& line:worldLines) *destination++={
            {static_cast<float>(line.startPoint.x),static_cast<float>(line.startPoint.y),static_cast<float>(line.startPoint.z),static_cast<float>(line.startRadius)},
            {static_cast<float>(line.endPoint.x),static_cast<float>(line.endPoint.y),static_cast<float>(line.endPoint.z),static_cast<float>(line.endRadius)},
            {static_cast<float>(line.startNitMm[0]),static_cast<float>(line.startNitMm[1]),static_cast<float>(line.startNitMm[2]),0},
            {static_cast<float>(line.endNitMm[0]),static_cast<float>(line.endNitMm[1]),static_cast<float>(line.endNitMm[2]),0},
            {static_cast<float>(line.startNits[0]*line.startEmitScale),static_cast<float>(line.startNits[1]*line.startEmitScale),static_cast<float>(line.startNits[2]*line.startEmitScale),static_cast<float>(line.startOpacity)},
            {static_cast<float>(line.endNits[0]*line.endEmitScale),static_cast<float>(line.endNits[1]*line.endEmitScale),static_cast<float>(line.endNits[2]*line.endEmitScale),static_cast<float>(line.endOpacity)}};
        lines_[slot]->Unmap(0,nullptr);
        const auto uploadStroke=[&](ComPtr<ID3D12Resource>& buffer,
            std::size_t& held,std::size_t count,std::size_t stride,const void* source)
        {
            const auto required=std::bit_ceil(std::max<std::size_t>(1,count));
            if(required>held)
            {buffer=Buffer(device_.Get(),required*stride,true);held=required;}
            if(count)
            {
                void* target{};D3D12_RANGE noRead{0,0};
                Check(buffer->Map(0,&noRead,&target),"Map wall stroke upload");
                std::memcpy(target,source,count*stride);buffer->Unmap(0,nullptr);
            }
        };
        const std::size_t shapeCount=strokeView?strokeView->shapes.size():0;
        const std::size_t rangeCount=strokeView?strokeView->ranges.size():0;
        const std::size_t indexCount=strokeView?strokeView->indices.size():0;
        const std::uint64_t strokeVersion=strokeView?strokeView->version:0;
        if(!strokeUploaded_[slot] || strokeVersion_[slot]!=strokeVersion)
        {
            uploadStroke(strokeShapes_[slot],strokeShapeCap_[slot],shapeCount,
                sizeof(stroke::Shape),strokeView?strokeView->shapes.data():nullptr);
            uploadStroke(strokeRanges_[slot],strokeRangeCap_[slot],rangeCount,
                sizeof(stroke::TileRange),strokeView?strokeView->ranges.data():nullptr);
            uploadStroke(strokeIndices_[slot],strokeIndexCap_[slot],indexCount,
                sizeof(std::uint32_t),strokeView?strokeView->indices.data():nullptr);
            strokeVersion_[slot]=strokeVersion;
            strokeUploaded_[slot]=true;
        }
        Constants constants{
            static_cast<float>(scene::ScreenArcWidthMm),
            static_cast<float>(scene::ScreenHeightMm),
            static_cast<float>(scene::ScreenCurveRadiusMm),
            static_cast<float>(scene::BackWallMm),
            static_cast<float>(width),static_cast<float>(height),
            static_cast<float>(scene::ScreenHalfChordMm()),
            static_cast<float>(scene::FrontEdgeZmm()),
            static_cast<float>(eye.position.x),
            static_cast<float>(eye.position.y),
            static_cast<float>(eye.position.z),0,
            static_cast<float>(light.position.x),
            static_cast<float>(light.position.y),
            static_cast<float>(light.position.z),
            static_cast<float>(light.radius),
            static_cast<float>(light.intensityRgb[0]),
            static_cast<float>(light.intensityRgb[1]),
            static_cast<float>(light.intensityRgb[2]),frame?1.0f:0.0f,
            scene::Roughness,scene::BaseReflectance,scene::NormalStrength,
            static_cast<float>(scene::PatchSide),
            static_cast<float>(scene::PaneThicknessMm),
            static_cast<float>(scene::PaneRefractiveIndex),
            static_cast<float>(scene::PaneAbsorptionPerMm),0,
            lineCount_,scene::LightingSide,treeCount_,debugView_,
            wallField.timeSec,
            {wallField.bands[0],wallField.bands[1],wallField.bands[2]},
            wallField.level,wallField.balance,wallField.entropy,
            static_cast<std::uint32_t>(wallField.mode),
            wallField.passes,{},
            strokeView?strokeView->scale:0,
            strokeView?strokeView->width:0,
            strokeView?strokeView->height:0,
            strokeView?strokeView->opacity:0,
            strokeView?strokeView->tilePx:1,
            strokeView?strokeView->tileCols:0,
            strokeView?strokeView->tileRows:0,
            static_cast<std::uint32_t>(shapeCount),
            strokeView && strokeView->paintEnabled &&
                shapeCount && rangeCount?1u:0u,{}};
        static_assert(sizeof(constants)<=256);std::memcpy(mapped_+slot*256,&constants,sizeof(constants));
    }
    void BakedRoomScene::Bind(ID3D12GraphicsCommandList* commands,bool compute)
    {
        const auto cb=constants_->GetGPUVirtualAddress()+slot_*256;
        if(compute) {
            commands->SetComputeRootSignature(root_.Get());commands->SetComputeRootConstantBufferView(0,cb);
            commands->SetComputeRootShaderResourceView(1,patches_->GetGPUVirtualAddress());commands->SetComputeRootShaderResourceView(2,response_->GetGPUVirtualAddress());
            commands->SetComputeRootUnorderedAccessView(3,direct_->GetGPUVirtualAddress());commands->SetComputeRootUnorderedAccessView(4,bounce_->GetGPUVirtualAddress());
            commands->SetComputeRootShaderResourceView(8,wallSamples_->GetGPUVirtualAddress());commands->SetComputeRootShaderResourceView(9,wallGather_->GetGPUVirtualAddress());commands->SetComputeRootUnorderedAccessView(10,wallAtlas_->GetGPUVirtualAddress());
            commands->SetComputeRootShaderResourceView(5,lines_[slot_]->GetGPUVirtualAddress());commands->SetComputeRootUnorderedAccessView(6,lineIrradiance_->GetGPUVirtualAddress());commands->SetComputeRootUnorderedAccessView(7,lineBudgets_[slot_]->GetGPUVirtualAddress());
            commands->SetComputeRootShaderResourceView(11,tubeTrees_[slot_]->GetGPUVirtualAddress());
            commands->SetComputeRootShaderResourceView(12,factors_->GetGPUVirtualAddress());
            commands->SetComputeRootUnorderedAccessView(13,bounceWork_->GetGPUVirtualAddress());
            commands->SetComputeRootShaderResourceView(14,strokeShapes_[slot_]->GetGPUVirtualAddress());
            commands->SetComputeRootShaderResourceView(15,strokeRanges_[slot_]->GetGPUVirtualAddress());
            commands->SetComputeRootShaderResourceView(16,strokeIndices_[slot_]->GetGPUVirtualAddress());
        } else {
            commands->SetGraphicsRootSignature(root_.Get());commands->SetGraphicsRootConstantBufferView(0,cb);
            commands->SetGraphicsRootShaderResourceView(1,patches_->GetGPUVirtualAddress());commands->SetGraphicsRootShaderResourceView(2,response_->GetGPUVirtualAddress());
            commands->SetGraphicsRootUnorderedAccessView(3,direct_->GetGPUVirtualAddress());commands->SetGraphicsRootUnorderedAccessView(4,bounce_->GetGPUVirtualAddress());
            commands->SetGraphicsRootShaderResourceView(8,wallSamples_->GetGPUVirtualAddress());commands->SetGraphicsRootShaderResourceView(9,wallGather_->GetGPUVirtualAddress());commands->SetGraphicsRootUnorderedAccessView(10,wallAtlas_->GetGPUVirtualAddress());
            commands->SetGraphicsRootShaderResourceView(5,lines_[slot_]->GetGPUVirtualAddress());commands->SetGraphicsRootUnorderedAccessView(6,lineIrradiance_->GetGPUVirtualAddress());commands->SetGraphicsRootUnorderedAccessView(7,lineBudgets_[slot_]->GetGPUVirtualAddress());
            commands->SetGraphicsRootShaderResourceView(11,tubeTrees_[slot_]->GetGPUVirtualAddress());
            commands->SetGraphicsRootShaderResourceView(12,factors_->GetGPUVirtualAddress());
            commands->SetGraphicsRootUnorderedAccessView(13,bounceWork_->GetGPUVirtualAddress());
            commands->SetGraphicsRootShaderResourceView(14,strokeShapes_[slot_]->GetGPUVirtualAddress());
            commands->SetGraphicsRootShaderResourceView(15,strokeRanges_[slot_]->GetGPUVirtualAddress());
            commands->SetGraphicsRootShaderResourceView(16,strokeIndices_[slot_]->GetGPUVirtualAddress());
        }
    }
    void BakedRoomScene::PrepareLighting(ID3D12GraphicsCommandList* commands)
    {
        if(!staticUploaded_) {
            for(const auto& pair:staticUploads_) {
                commands->CopyBufferRegion(pair[1].Get(),0,pair[0].Get(),0,pair[0]->GetDesc().Width);
                D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=pair[1].Get();
                b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;commands->ResourceBarrier(1,&b);
            }
            staticUploaded_=true;
        }
        Barrier(commands,direct_.Get());Barrier(commands,bounce_.Get());Barrier(commands,lineIrradiance_.Get());Barrier(commands,wallAtlas_.Get());Bind(commands,true);
        if(lineCount_) {
            Barrier(commands,lineBudgets_[slot_].Get());commands->SetPipelineState(lineBudgetPipeline_.Get());commands->Dispatch((lineCount_+63)/64,1,1);Barrier(commands,lineBudgets_[slot_].Get());
        }
        commands->SetPipelineState(directPipeline_.Get());commands->Dispatch((scene::PatchCount+63)/64,1,1);Barrier(commands,direct_.Get());Barrier(commands,lineIrradiance_.Get());
        if(wallMode_==scene::WallMode::Fixed) {
            commands->SetPipelineState(bouncePipeline_.Get());commands->Dispatch((scene::PatchCount+63)/64,1,1);Barrier(commands,bounce_.Get());
        } else {
            Barrier(commands,bounceWork_.Get());
            Barrier(commands,bounce_.Get());
            ID3D12Resource* source=bounce_.Get();
            ID3D12Resource* target=bounceWork_.Get();
            commands->SetPipelineState(bounceStepPipe_.Get());
            for(std::uint32_t pass=0;pass<wallPasses_;++pass) {
                commands->SetComputeRootUnorderedAccessView(4,source->GetGPUVirtualAddress());
                commands->SetComputeRootUnorderedAccessView(13,target->GetGPUVirtualAddress());
                commands->Dispatch((scene::PatchCount+63)/64,1,1);
                Barrier(commands,target);
                std::swap(source,target);
            }
            commands->SetComputeRootUnorderedAccessView(4,source->GetGPUVirtualAddress());
            commands->SetComputeRootUnorderedAccessView(13,target->GetGPUVirtualAddress());
        }
        commands->SetPipelineState(gatherPipeline_.Get());commands->Dispatch((scene::LightingCount+63)/64,1,1);Barrier(commands,wallAtlas_.Get());
    }
    void BakedRoomScene::DrawScene(ID3D12GraphicsCommandList* commands)
    {
        Bind(commands,false);commands->SetPipelineState(scenePipeline_.Get());commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);commands->DrawInstanced(3,1,0,0);
    }
}
