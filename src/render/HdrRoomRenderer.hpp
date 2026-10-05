#pragma once

#include "visualizer/AudioTrailMotion.hpp"
#include "render/HdrToneMapper.hpp"
#include "render/PresentationSync.hpp"
#include "render/BakedRoomScene.hpp"
#include "render/HdrSceneOutput.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <memory>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace bk::visualizer { struct TrailPoint; }
namespace bk::stroke { struct View; }

namespace bk::render
{
    class HdrRoomRenderer
    {
    public:
        HdrRoomRenderer() = default;
        ~HdrRoomRenderer();

        HdrRoomRenderer(const HdrRoomRenderer&) = delete;
        HdrRoomRenderer& operator=(const HdrRoomRenderer&) = delete;

        void Initialize(HWND window, std::uint32_t width, std::uint32_t height);
        void Resize(std::uint32_t width, std::uint32_t height);
        void SetPaperWhiteNits(float nits);
        void SetPresentationSync(PresentationSync mode);
        void Render(
            const visualizer::TrailFrame* visualizerFrame = nullptr,
            std::span<const visualizer::TrailPoint> trail = {},
            std::span<const visualizer::TrailPoint> sweep = {},
            const scene::EyePose* roomEye = nullptr,
            const scene::WallField& wallField = {},
            const stroke::View* strokeView = nullptr);
        void WaitForGpu();

    private:
        static constexpr std::uint32_t FrameCount = 2;

        struct FrameData;
        struct DisplayLimits;
        struct TrailVertex;

        void CreateDevice();
        void CreateSwapChain();
        void CreatePipelines();
        void CreateViewTargets();
        void DetectDisplay();
        void RefreshDisplay();
        void UpdateFrameData(const visualizer::TrailFrame* visualizerFrame);
        void UpdateTrailGeometry(
            std::span<const visualizer::TrailPoint> trail,
            std::span<const visualizer::TrailPoint> sweep,
            float opacity);
        void EnsureTrailCapacity(std::size_t required);
        void AcquireFrameSlot();

        HWND window_{};
        std::uint32_t width_{};
        std::uint32_t height_{};
        std::uint32_t frameIndex_{};
        std::uint32_t rtvStride_{};
        std::uint32_t swapFlags_{};
        bool canTear_{};
        std::uint64_t fenceValue_{};
        HANDLE fenceEvent_{};
        DisplayLimits* displayLimits_{};

        Microsoft::WRL::ComPtr<IDXGIFactory6> factory_;
        Microsoft::WRL::ComPtr<IDXGIAdapter4> adapter_;
        Microsoft::WRL::ComPtr<ID3D12Device> device_;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> drawQueue_;
        Microsoft::WRL::ComPtr<IDXGISwapChain4> swapChain_;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> dotPipeline_;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> trailPipeline_;
        std::unique_ptr<BakedRoomScene> room_;
        std::unique_ptr<HdrSceneOutput> output_;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> drawList_;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
        Microsoft::WRL::ComPtr<ID3D12Resource> frameData_;
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, FrameCount> trailBuffers_;

        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, FrameCount> backBuffers_;
        std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, FrameCount> frameAllocators_;
        std::array<std::uint64_t, FrameCount> frameFences_{};

        std::byte* mappedFrameData_{};
        std::array<TrailVertex*, FrameCount> mappedTrail_{};
        std::array<std::size_t, FrameCount> trailCaps_{};
        D3D12_VERTEX_BUFFER_VIEW trailView_{};
        std::uint32_t trailVertexCount_{};
        D3D12_VIEWPORT viewport_{};
        D3D12_RECT scissorRect_{};
        float paperWhiteNits_{static_cast<float>(hdr::BasePaperWhite)};
        UINT syncInterval_{};
        UINT presentFlags_{};
    };
}
