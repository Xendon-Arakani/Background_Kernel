#include "render/WallStrokeCanvas.hpp"
#include "render/DisplayMath.hpp"
#include <d3dcompiler.h>
#include <array>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

namespace bk::render
{
    namespace
    {
        void Check(const HRESULT result,const char* operation)
        {
            if(FAILED(result)) throw std::runtime_error(
                std::string(operation)+": "+std::to_string(result));
        }

        const std::string Shader=std::string(R"hlsl(
            cbuffer Controls:register(b0) {
                float PaintOpacity;float BlackNits;float PeakNits;float ScRgbWhiteNits;
                float2 Cursor;float BrushRadius;float Hovering;
                float Drawing;float3 Tint;
                float2 SurfacePx;float OrbNits;float SurfacePad;
            }
            struct Shape {float4 start;float4 end;float4 startColor;float4 endColor;};
            StructuredBuffer<Shape> Shapes:register(t0);
            Texture2D<float4> Scene:register(t1);
        )hlsl")+DisplayMath+R"hlsl(
            float4 VS(uint vertex:SV_VertexID):SV_POSITION {
                return vertex==0?float4(-1,-1,0,1):
                    vertex==1?float4(-1,3,0,1):float4(3,-1,0,1);
            }
            struct ShapePixel {
                float4 position:SV_POSITION;
                float2 pixel:TEXCOORD0;
                nointerpolation uint index:TEXCOORD1;
            };
            ShapePixel ShapeVS(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
                Shape shape=Shapes[instance];
                float padding=max(shape.start.z,shape.end.z)+1;
                float2 minimum=min(shape.start.xy,shape.end.xy)-padding;
                float2 maximum=max(shape.start.xy,shape.end.xy)+padding;
                float2 corners[6]={float2(0,0),float2(0,1),float2(1,0),
                    float2(1,0),float2(0,1),float2(1,1)};
                ShapePixel output;
                output.pixel=lerp(minimum,maximum,corners[vertex]);
                output.position=float4(output.pixel.x/SurfacePx.x*2-1,
                    1-output.pixel.y/SurfacePx.y*2,0,1);
                output.index=instance;
                return output;
            }
            struct PaintPixel {
                float4 light:SV_TARGET;
                float opacity:SV_DEPTH;
            };
            PaintPixel ShapePS(ShapePixel input) {
                Shape shape=Shapes[input.index];
                float2 delta=shape.end.xy-shape.start.xy;
                float extent=max(dot(delta,delta),1e-8);
                float t=saturate(dot(input.pixel-shape.start.xy,delta)/extent);
                float2 nearest=lerp(shape.start.xy,shape.end.xy,t);
                float radius=lerp(shape.start.z,shape.end.z,t);
                if(radius<=0) discard;
                float coverage=1-smoothstep(radius-0.7,radius+0.7,
                    length(input.pixel-nearest));
                float fade=lerp(shape.start.w,shape.end.w,t);
                float strength=fade*coverage;
                float alpha=PaintOpacity*strength;
                if(strength<=0) discard;
                float3 color=lerp(shape.startColor.rgb,shape.endColor.rgb,t);
                PaintPixel output;
                output.light=float4(color*OrbNits*fade*alpha,alpha);
                output.opacity=strength;
                return output;
            }
            float4 PS(float4 position:SV_POSITION):SV_TARGET {
                float3 output=DisplayTransform(clamp(
                    Scene.Load(int3((int2)position.xy,0)).rgb,0,10000));
                if(Hovering>0) {
                    float distance=length(position.xy-Cursor);
                    float ring=1-smoothstep(0.7,1.7,abs(distance-BrushRadius));
                    output=lerp(output,float3(0.55,0.55,0.55),
                        ring*(Drawing>0?0.7:0.35));
                    float orb=1-smoothstep(2.5,3.5,distance);
                    output=lerp(output,DisplayTransform(Tint*OrbNits),orb);
                }
                return float4(output,1);
            }
        )hlsl";

        ComPtr<ID3DBlob> Compile(const char* entry,const char* profile)
        {
            ComPtr<ID3DBlob> blob,error;
            const auto result=D3DCompile(Shader.data(),Shader.size(),
                "WallStrokeCanvas",nullptr,nullptr,entry,profile,
                D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0,&blob,&error);
            if(FAILED(result) && error) throw std::runtime_error(std::string(
                static_cast<const char*>(error->GetBufferPointer()),
                error->GetBufferSize()));
            Check(result,"Compile wall stroke shader");
            return blob;
        }
    }

    WallStrokeCanvas::~WallStrokeCanvas()
    {
        if(context_)
        {
            context_->ClearState();
            context_->Flush();
        }
        if(frameReady_) CloseHandle(frameReady_);
    }

    void WallStrokeCanvas::Initialize(HWND window,int width,int height)
    {
        window_=window;
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0};
        Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,2,D3D11_SDK_VERSION,
            &device_,nullptr,&context_),"Create wall stroke device");
        ComPtr<IDXGIDevice> dxgi;
        Check(device_.As(&dxgi),"Get wall stroke DXGI device");
        ComPtr<IDXGIAdapter> adapter;
        Check(dxgi->GetAdapter(&adapter),"Get wall stroke adapter");
        ComPtr<IDXGIFactory2> factory;
        Check(adapter->GetParent(IID_PPV_ARGS(&factory)),
            "Get wall stroke factory");
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width=static_cast<UINT>(width);
        desc.Height=static_cast<UINT>(height);
        desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count=1;
        desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount=2;
        desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> swap;
        Check(factory->CreateSwapChainForHwnd(device_.Get(),window,&desc,
            nullptr,nullptr,&swap),"Create wall stroke swap chain");
        Check(swap.As(&swap_),"Get wall stroke color interface");
        Check(factory->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER),
            "Set wall stroke window association");
        Check(swap_->SetMaximumFrameLatency(1),"Set wall stroke frame latency");
        frameReady_=swap_->GetFrameLatencyWaitableObject();
        if(!frameReady_) throw std::runtime_error(
            "Wall stroke frame signal unavailable");
        UINT support{};
        Check(swap_->CheckColorSpaceSupport(
            DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,&support),
            "Check wall stroke scRGB support");
        if(!(support&DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
            throw std::runtime_error("Wall stroke scRGB presentation unavailable");
        Check(swap_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709),
            "Set wall stroke scRGB output");
        const auto vs=Compile("VS","vs_5_0");
        const auto ps=Compile("PS","ps_5_0");
        const auto shapeVs=Compile("ShapeVS","vs_5_0");
        const auto shapePs=Compile("ShapePS","ps_5_0");
        Check(device_->CreateVertexShader(vs->GetBufferPointer(),
            vs->GetBufferSize(),nullptr,&vertex_),"Create wall stroke vertex shader");
        Check(device_->CreatePixelShader(ps->GetBufferPointer(),
            ps->GetBufferSize(),nullptr,&pixel_),"Create wall stroke pixel shader");
        Check(device_->CreateVertexShader(shapeVs->GetBufferPointer(),
            shapeVs->GetBufferSize(),nullptr,&shapeVertex_),
            "Create wall stroke shape vertex shader");
        Check(device_->CreatePixelShader(shapePs->GetBufferPointer(),
            shapePs->GetBufferSize(),nullptr,&shapePixel_),
            "Create wall stroke shape pixel shader");
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable=TRUE;
        depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
        depth.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;
        Check(device_->CreateDepthStencilState(&depth,&paintDepth_),
            "Create paint union state");
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth=64;
        cb.Usage=D3D11_USAGE_DYNAMIC;
        cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        Check(device_->CreateBuffer(&cb,nullptr,&constants_),
            "Create wall stroke constants");
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode=D3D11_FILL_SOLID;
        raster.CullMode=D3D11_CULL_NONE;
        raster.DepthClipEnable=TRUE;
        Check(device_->CreateRasterizerState(&raster,&raster_),
            "Create wall stroke raster state");
        Resize(width,height);
        RefreshDisplay();
    }

    void WallStrokeCanvas::Resize(int width,int height)
    {
        if(width<=0 || height<=0 || !swap_ ||
            (width==width_ && height==height_)) return;
        context_->OMSetRenderTargets(0,nullptr,nullptr);
        target_.Reset();
        Check(swap_->ResizeBuffers(0,static_cast<UINT>(width),
            static_cast<UINT>(height),DXGI_FORMAT_UNKNOWN,
            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT),
            "Resize wall stroke canvas");
        ComPtr<ID3D11Texture2D> buffer;
        Check(swap_->GetBuffer(0,IID_PPV_ARGS(&buffer)),
            "Get wall stroke back buffer");
        Check(device_->CreateRenderTargetView(buffer.Get(),nullptr,&target_),
            "Create wall stroke render target");
        ID3D11ShaderResourceView* empty[2]{};
        context_->PSSetShaderResources(0,2,empty);
        sceneView_.Reset();
        sceneTarget_.Reset();
        scene_.Reset();
        depthView_.Reset();
        depth_.Reset();
        D3D11_TEXTURE2D_DESC scene{};
        scene.Width=static_cast<UINT>(width);
        scene.Height=static_cast<UINT>(height);
        scene.MipLevels=1;
        scene.ArraySize=1;
        scene.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        scene.SampleDesc.Count=1;
        scene.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        Check(device_->CreateTexture2D(&scene,nullptr,&scene_),
            "Create wall stroke scene");
        Check(device_->CreateRenderTargetView(scene_.Get(),nullptr,&sceneTarget_),
            "Create wall stroke scene target");
        Check(device_->CreateShaderResourceView(scene_.Get(),nullptr,&sceneView_),
            "Create wall stroke scene view");
        scene.Format=DXGI_FORMAT_D32_FLOAT;
        scene.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        Check(device_->CreateTexture2D(&scene,nullptr,&depth_),
            "Create paint union buffer");
        Check(device_->CreateDepthStencilView(depth_.Get(),nullptr,&depthView_),
            "Create paint union view");
        width_=width;
        height_=height;
    }

    void WallStrokeCanvas::RefreshDisplay()
    {
        if(!swap_) return;
        ComPtr<IDXGIOutput> output;
        ComPtr<IDXGIOutput6> hdr;
        DXGI_OUTPUT_DESC1 info{};
        black_=0;
        peak_=80;
        if(SUCCEEDED(swap_->GetContainingOutput(&output)) &&
            SUCCEEDED(output.As(&hdr)) && SUCCEEDED(hdr->GetDesc1(&info)) &&
            info.ColorSpace==DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
        {
            black_=std::clamp(info.MinLuminance,0.0f,10000.0f);
            peak_=info.MaxLuminance>1?std::min(info.MaxLuminance,10000.0f):80;
        }
    }

    void WallStrokeCanvas::Draw(std::span<const stroke::Shape> shapes,
        const stroke::Point cursor,const stroke::Color color,const float radius,
        const float opacity,const bool hovering,const bool drawing)
    {
        if(!target_) return;
        ID3D11ShaderResourceView* empty[2]{};
        context_->PSSetShaderResources(0,2,empty);
        context_->VSSetShaderResources(0,2,empty);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(!shapes.empty())
        {
            if(shapeCapacity_<shapes.size())
            {
                shapeCapacity_=std::bit_ceil(shapes.size());
                shapeView_.Reset();
                shapeData_.Reset();
                D3D11_BUFFER_DESC desc{};
                desc.ByteWidth=static_cast<UINT>(shapeCapacity_*
                    sizeof(stroke::Shape));
                desc.Usage=D3D11_USAGE_DYNAMIC;
                desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
                desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
                desc.StructureByteStride=sizeof(stroke::Shape);
                Check(device_->CreateBuffer(&desc,nullptr,&shapeData_),
                    "Create wall stroke shape buffer");
                D3D11_SHADER_RESOURCE_VIEW_DESC view{};
                view.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;
                view.Buffer.NumElements=static_cast<UINT>(shapeCapacity_);
                Check(device_->CreateShaderResourceView(shapeData_.Get(),
                    &view,&shapeView_),"Create wall stroke shape view");
            }
            Check(context_->Map(shapeData_.Get(),0,D3D11_MAP_WRITE_DISCARD,
                0,&mapped),"Upload wall stroke shapes");
            std::memcpy(mapped.pData,shapes.data(),
                shapes.size_bytes());
            context_->Unmap(shapeData_.Get(),0);
        }
        struct Constants
        {
            float opacity{},black{},peak{},white{};
            float cursorX{},cursorY{},radius{},hovering{};
            float drawing{};
            std::array<float,3> tint{};
            float width{},height{},orbNits{},pad{};
        };
        static_assert(sizeof(Constants)==64);
        const Constants values{opacity,black_,peak_,80,cursor.x,cursor.y,
            radius,hovering?1.0f:0.0f,drawing?1.0f:0.0f,
            {color.red,color.green,color.blue},static_cast<float>(width_),
            static_cast<float>(height_),
            static_cast<float>(hdr::SourcePeakNits),0};
        Check(context_->Map(constants_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),
            "Upload wall stroke draw constants");
        std::memcpy(mapped.pData,&values,sizeof(values));
        context_->Unmap(constants_.Get(),0);
        const D3D11_VIEWPORT viewport{0,0,static_cast<float>(width_),
            static_cast<float>(height_),0,1};
        context_->RSSetViewports(1,&viewport);
        context_->RSSetState(raster_.Get());
        ID3D11Buffer* constants=constants_.Get();
        context_->PSSetConstantBuffers(0,1,&constants);
        context_->VSSetConstantBuffers(0,1,&constants);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11RenderTargetView* scene=sceneTarget_.Get();
        context_->OMSetRenderTargets(1,&scene,depthView_.Get());
        context_->OMSetDepthStencilState(paintDepth_.Get(),0);
        context_->OMSetBlendState(nullptr,nullptr,~0u);
        constexpr float clear[4]{};
        context_->ClearRenderTargetView(sceneTarget_.Get(),clear);
        context_->ClearDepthStencilView(depthView_.Get(),D3D11_CLEAR_DEPTH,0,0);
        if(!shapes.empty())
        {
            ID3D11ShaderResourceView* shapeView=shapeView_.Get();
            context_->VSSetShaderResources(0,1,&shapeView);
            context_->PSSetShaderResources(0,1,&shapeView);
            context_->VSSetShader(shapeVertex_.Get(),nullptr,0);
            context_->PSSetShader(shapePixel_.Get(),nullptr,0);
            context_->DrawInstanced(6,static_cast<UINT>(shapes.size()),0,0);
        }
        ID3D11RenderTargetView* target=target_.Get();
        context_->OMSetRenderTargets(1,&target,nullptr);
        context_->OMSetDepthStencilState(nullptr,0);
        context_->OMSetBlendState(nullptr,nullptr,~0u);
        context_->VSSetShader(vertex_.Get(),nullptr,0);
        context_->PSSetShader(pixel_.Get(),nullptr,0);
        ID3D11ShaderResourceView* sceneView=sceneView_.Get();
        context_->PSSetShaderResources(1,1,&sceneView);
        context_->Draw(3,0);
        Check(swap_->Present(1,0),"Present wall stroke canvas");
    }
}
