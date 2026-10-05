#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstddef>

namespace bk::render
{
    class HdrSceneOutput
    {
    public:
        ~HdrSceneOutput();
        void Initialize(ID3D12Device* device);
        void ResizeAfterGpu(std::uint32_t width,std::uint32_t height);
        void Begin(ID3D12GraphicsCommandList* commands);
        void Resolve(ID3D12GraphicsCommandList* commands,D3D12_CPU_DESCRIPTOR_HANDLE destination,
                     std::uint32_t slot,float blackNits,float peakNits);
        static void ValidateShaders();
    private:
        Microsoft::WRL::ComPtr<ID3D12Device> device_;
        Microsoft::WRL::ComPtr<ID3D12Resource> scene_,constants_;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_,srv_;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
        std::byte* mapped_{};
        std::uint32_t width_{},height_{};
    };
}
