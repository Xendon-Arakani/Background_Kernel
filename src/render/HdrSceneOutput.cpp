#include "render/HdrSceneOutput.hpp"
#include "render/DisplayMath.hpp"
#include <d3dcompiler.h>
#include <cstring>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
namespace bk::render
{
    namespace
    {
        const std::string Shader=std::string(R"hlsl(
            cbuffer Display:register(b0) {
                float BlackNits;
                float PeakNits;
                float ScRgbWhiteNits;
                float Reserved;
            }
            Texture2D<float4> Scene:register(t0);
        )hlsl")+DisplayMath+R"hlsl(
            float4 VS(uint vertex:SV_VertexID):SV_POSITION {
                return vertex==0?float4(-1,-1,0,1):vertex==1?float4(-1,3,0,1):float4(3,-1,0,1);
            }
            float4 PS(float4 position:SV_POSITION):SV_TARGET {
                return float4(DisplayTransform(Scene.Load(int3((int2)position.xy,0)).rgb*80),1);
            }
        )hlsl";
        void Check(HRESULT value,const char* what) { if(FAILED(value)) throw std::runtime_error(std::string(what)+": "+std::to_string(value)); }
        ComPtr<ID3DBlob> Compile(const char* entry,const char* profile) {
            ComPtr<ID3DBlob> result,error;const auto hr=D3DCompile(Shader.data(),Shader.size(),"SharedSceneOutput",nullptr,nullptr,entry,profile,
                D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&result,&error);
            if(FAILED(hr)&&error) throw std::runtime_error(std::string(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()));
            Check(hr,"Compile shared display transform");return result;
        }
        void Transition(ID3D12GraphicsCommandList* commands,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};commands->ResourceBarrier(1,&barrier);
        }
    }
    HdrSceneOutput::~HdrSceneOutput() { if(constants_&&mapped_) constants_->Unmap(0,nullptr); }
    void HdrSceneOutput::ValidateShaders() { Compile("VS","vs_5_1");Compile("PS","ps_5_1"); }
    void HdrSceneOutput::Initialize(ID3D12Device* device)
    {
        device_=device;
        D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=1;
        Check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&rtv_)),"Create linear-scene RTV heap");
        heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&srv_)),"Create linear-scene SRV heap");
        D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;
        D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[1].DescriptorTable={1,&range};
        D3D12_ROOT_SIGNATURE_DESC root{};root.NumParameters=2;root.pParameters=params;
        ComPtr<ID3DBlob> serialized,error;Check(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error),"Serialize output root");
        Check(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root_)),"Create output root");
        const auto vs=Compile("VS","vs_5_1"),ps=Compile("PS","ps_5_1");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=root_.Get();pipeline.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pipeline.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pipeline.SampleMask=UINT_MAX;
        pipeline.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pipeline.RasterizerState.DepthClipEnable=TRUE;
        pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pipeline.NumRenderTargets=1;pipeline.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;pipeline.SampleDesc.Count=1;
        Check(device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&pipeline_)),"Create shared output pipeline");
        D3D12_HEAP_PROPERTIES upload{};upload.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=512;buffer.Height=1;buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Check(device->CreateCommittedResource(&upload,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&constants_)),"Create output constants");
        D3D12_RANGE noRead{0,0};Check(constants_->Map(0,&noRead,reinterpret_cast<void**>(&mapped_)),"Map output constants");
    }
    void HdrSceneOutput::ResizeAfterGpu(std::uint32_t width,std::uint32_t height)
    {
        if(width==width_&&height==height_) return;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=width;texture.Height=height;texture.DepthOrArraySize=texture.MipLevels=1;
        texture.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;texture.SampleDesc.Count=1;texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};clear.Format=texture.Format;clear.Color[3]=1;
        Check(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,&clear,IID_PPV_ARGS(&scene_)),"Create scene-linear target");
        device_->CreateRenderTargetView(scene_.Get(),nullptr,rtv_->GetCPUDescriptorHandleForHeapStart());
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=texture.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
        device_->CreateShaderResourceView(scene_.Get(),&view,srv_->GetCPUDescriptorHandleForHeapStart());width_=width;height_=height;
    }
    void HdrSceneOutput::Begin(ID3D12GraphicsCommandList* commands)
    {
        Transition(commands,scene_.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto target=rtv_->GetCPUDescriptorHandleForHeapStart();const float black[]{0,0,0,1};
        commands->ClearRenderTargetView(target,black,0,nullptr);commands->OMSetRenderTargets(1,&target,FALSE,nullptr);
    }
    void HdrSceneOutput::Resolve(ID3D12GraphicsCommandList* commands,D3D12_CPU_DESCRIPTOR_HANDLE destination,std::uint32_t slot,float blackNits,float peakNits)
    {
        struct Constants
        {
            float output[4]{};
        };
        static_assert(sizeof(Constants)<=256);
        Constants constants{};
        constants.output[0]=blackNits;constants.output[1]=peakNits;constants.output[2]=80.0f;
        std::memcpy(mapped_+slot*256,&constants,sizeof(constants));
        Transition(commands,scene_.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commands->OMSetRenderTargets(1,&destination,FALSE,nullptr);commands->SetGraphicsRootSignature(root_.Get());commands->SetPipelineState(pipeline_.Get());
        ID3D12DescriptorHeap* heaps[]{srv_.Get()};commands->SetDescriptorHeaps(1,heaps);
        commands->SetGraphicsRootConstantBufferView(0,constants_->GetGPUVirtualAddress()+slot*256);commands->SetGraphicsRootDescriptorTable(1,srv_->GetGPUDescriptorHandleForHeapStart());
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);commands->DrawInstanced(3,1,0,0);
    }
}
