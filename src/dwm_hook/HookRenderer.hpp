#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

namespace bk::dwm_hook
{
    class HookRenderer
    {
    public:
        explicit HookRenderer(bool wakeDesktop = true) noexcept;
        ~HookRenderer();

        HookRenderer(const HookRenderer&) = delete;
        HookRenderer& operator=(const HookRenderer&) = delete;

        bool EnsureInitialized(ID3D11RenderTargetView* renderTarget);
        bool Draw(ID3D11DeviceContext* immediateContext, ID3D11RenderTargetView* renderTarget);
        [[nodiscard]] uint32_t Diagnostics() const noexcept
        {
            return stage_.load(std::memory_order_acquire);
        }
        void Shutdown();

    private:
        static constexpr uint32_t SurfaceCount = 3;
        static constexpr uint32_t InvalidSurface = UINT32_MAX;
        struct SharedScene;

        bool Initialize(ID3D11Device* renderDevice, uint32_t width, uint32_t height);
        bool CreateBlitPipeline(ID3D11Device* renderDevice);
        bool CreateSceneQueue(IDXGIAdapter* adapter);
        bool CreateSurfaces(ID3D11Device* renderDevice);
        bool CreateFenceBridge(ID3D11Device* renderDevice);
        bool CreateClosedList();

        void RenderLoop();
        uint32_t SelectWriteSurface() const;
        bool RenderSurface(uint32_t surfaceIndex);
        bool DrawSurface(
            ID3D11DeviceContext* immediateContext,
            ID3D11RenderTargetView* renderTarget,
            uint32_t surfaceIndex);
        void WakeDesktop() const;

        std::mutex initMutex_;
        std::atomic_flag drawActive_ = ATOMIC_FLAG_INIT;
        std::atomic_bool initialized_{false};
        std::atomic_uint32_t stage_{0};
        std::atomic_bool stop_{false};
        std::thread renderThread_;

        uint32_t width_{};
        uint32_t height_{};
        Microsoft::WRL::ComPtr<ID3D11Device> renderDevice_;
        Microsoft::WRL::ComPtr<ID3D11Device5> fenceDevice_;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> blitVertexShader_;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> blitPixelShader_;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> blitSampler_;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> blitRasterizer_;
        Microsoft::WRL::ComPtr<ID3D11BlendState> blitBlendState_;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> blitDepthState_;

        Microsoft::WRL::ComPtr<ID3D12Device> sceneDevice_;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> drawQueue_;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> drawList_;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> targetHeap_;
        Microsoft::WRL::ComPtr<ID3D12Fence> sceneReadyFence_;
        Microsoft::WRL::ComPtr<ID3D12Fence> sceneDoneFence_;
        Microsoft::WRL::ComPtr<ID3D11Fence> renderReadyFence_;
        Microsoft::WRL::ComPtr<ID3D11Fence> renderDoneFence_;

        std::array<SharedScene, SurfaceCount>* surfaces_{};
        std::atomic_uint32_t latestSurface_{InvalidSurface};
        std::atomic_uint32_t shownSurface_{InvalidSurface};
        std::atomic_uint64_t nextReadyFence_{0};
        std::atomic_uint64_t nextDoneFence_{0};
        bool wakeDesktop_{true};
    };
}
