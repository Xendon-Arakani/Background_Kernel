#include "dwm_hook/OutputHook.hpp"
#include "dwm_hook/HookRenderer.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <thread>

namespace bk::dwm_hook
{
    namespace
    {
        constexpr size_t ClearTargetSlot = 50;
        constexpr size_t BindTargetsSlot = 33;
        using ClearTargetFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
        using BindTargetsFn = void(STDMETHODCALLTYPE*)(
            ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);

        std::atomic_bool running{false};
        std::atomic_bool hookInstalled{false};
        std::atomic_uint32_t activeHookCalls{0};
        std::atomic_bool wallpaperHidden{false};
        std::atomic_bool firstFrameDrawn{false};
        std::atomic_uint64_t monitorTargetMatches{0};
        std::atomic_uint64_t rendererAttempts{0};
        std::atomic_uint64_t totalClearCalls{0};
        std::atomic_uint64_t largestClearTarget{0};
        std::atomic_uint64_t totalTargetBindings{0};
        std::atomic_uint64_t largestBoundTarget{0};
        std::atomic_uint64_t lastDrawTick{0};
        std::atomic<HWND> hiddenWallpaper{nullptr};
        std::mutex hookMutex;
        ClearTargetFn clearOriginal{};
        BindTargetsFn bindOriginal{};
        void* clearHookTarget{};
        void* bindHookTarget{};
        thread_local bool insideHook{};
        HookRenderer renderer;

        class ActiveHookCall final
        {
        public:
            ActiveHookCall() noexcept
            {
                activeHookCalls.fetch_add(1, std::memory_order_acq_rel);
            }

            ~ActiveHookCall()
            {
                activeHookCalls.fetch_sub(1, std::memory_order_acq_rel);
            }
        };

        class HookDepthGuard final
        {
        public:
            HookDepthGuard() noexcept { insideHook = true; }
            ~HookDepthGuard() { insideHook = false; }
        };

        HWND FindWallpaperWorker()
        {
            const HWND progman = FindWindowW(L"Progman", nullptr);
            if (!progman)
            {
                return nullptr;
            }

            HWND worker{};
            HWND best{};
            long long bestArea{};
            while((worker=FindWindowExW(
                progman,worker,L"WorkerW",nullptr)))
            {
                RECT bounds{};
                if(!GetWindowRect(worker,&bounds)) continue;
                const long long area=static_cast<long long>(
                    bounds.right-bounds.left)*(bounds.bottom-bounds.top);
                if(area>bestArea)
                {
                    best=worker;
                    bestArea=area;
                }
            }
            return best;
        }

        bool HideWallpaperLayer()
        {
            if (wallpaperHidden.load(std::memory_order_acquire))
            {
                const HWND existing = hiddenWallpaper.load(std::memory_order_acquire);
                if (existing && IsWindow(existing) && !IsWindowVisible(existing))
                {
                    return true;
                }
                hiddenWallpaper.store(nullptr, std::memory_order_release);
                wallpaperHidden.store(false, std::memory_order_release);
            }
            bool expected = false;
            if (!wallpaperHidden.compare_exchange_strong(expected, true))
            {
                return true;
            }
            const HWND worker = FindWallpaperWorker();
            if (!worker)
            {
                wallpaperHidden.store(false, std::memory_order_release);
                return false;
            }
            if (!IsWindowVisible(worker))
            {
                wallpaperHidden.store(false, std::memory_order_release);
                return true;
            }
            hiddenWallpaper.store(worker, std::memory_order_release);
            if (!ShowWindowAsync(worker, SW_HIDE))
            {
                hiddenWallpaper.store(nullptr, std::memory_order_release);
                wallpaperHidden.store(false, std::memory_order_release);
                return false;
            }
            return true;
        }

        void RestoreWallpaper()
        {
            const HWND worker = hiddenWallpaper.exchange(nullptr, std::memory_order_acq_rel);
            wallpaperHidden.store(false, std::memory_order_release);
            if (worker && IsWindow(worker))
            {
                ShowWindowAsync(worker, SW_SHOW);
            }
            const HWND progman = FindWindowW(L"Progman", nullptr);
            if (progman)
            {
                RedrawWindow(
                    progman,
                    nullptr,
                    nullptr,
                    RDW_INVALIDATE | RDW_INTERNALPAINT | RDW_ALLCHILDREN);
            }
        }

        bool IsMonitorSizedTarget(
            ID3D11RenderTargetView* renderTarget,
            std::atomic_uint64_t& largestTarget)
        {
            if (!renderTarget)
            {
                return false;
            }
            ID3D11Resource* rawResource{};
            renderTarget->GetResource(&rawResource);
            if (!rawResource)
            {
                return false;
            }
            ID3D11Texture2D* rawTexture{};
            const HRESULT queryResult = rawResource->QueryInterface(IID_PPV_ARGS(&rawTexture));
            rawResource->Release();
            if (FAILED(queryResult))
            {
                return false;
            }
            D3D11_TEXTURE2D_DESC description{};
            rawTexture->GetDesc(&description);
            rawTexture->Release();
            const uint64_t packedDimensions =
                (static_cast<uint64_t>(description.Width) << 32) | description.Height;
            uint64_t previous = largestTarget.load(std::memory_order_relaxed);
            while (static_cast<uint32_t>(packedDimensions >> 32) *
                       static_cast<uint64_t>(static_cast<uint32_t>(packedDimensions)) >
                       static_cast<uint32_t>(previous >> 32) *
                       static_cast<uint64_t>(static_cast<uint32_t>(previous)) &&
                   !largestTarget.compare_exchange_weak(
                       previous, packedDimensions, std::memory_order_relaxed))
            {
            }
            if (description.Width < 800 || description.Height < 600 || description.SampleDesc.Count != 1)
            {
                return false;
            }

            POINT origin{};
            const HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO info{sizeof(info)};
            if (!GetMonitorInfoW(monitor, &info))
            {
                return false;
            }
            const UINT monitorWidth = static_cast<UINT>(info.rcMonitor.right - info.rcMonitor.left);
            const UINT monitorHeight = static_cast<UINT>(info.rcMonitor.bottom - info.rcMonitor.top);
            return description.Width == monitorWidth && description.Height == monitorHeight;
        }

        void DrawHookSafe(
            ID3D11DeviceContext* context,
            ID3D11RenderTargetView* renderTarget)
        {
            if (!running.load(std::memory_order_acquire) || insideHook ||
                !IsMonitorSizedTarget(renderTarget, largestBoundTarget))
            {
                return;
            }
            monitorTargetMatches.fetch_add(1, std::memory_order_relaxed);
            const uint64_t now = GetTickCount64();
            uint64_t previous = lastDrawTick.load(std::memory_order_relaxed);
            if (now - previous < 4 ||
                !lastDrawTick.compare_exchange_strong(
                    previous, now, std::memory_order_acq_rel))
            {
                return;
            }

            HookDepthGuard recursionGuard;
            try
            {
                rendererAttempts.fetch_add(1, std::memory_order_relaxed);
                if (renderer.Draw(context, renderTarget) && HideWallpaperLayer())
                {
                    firstFrameDrawn.store(true, std::memory_order_release);
                }
            }
            catch (...)
            {
                OutputDebugStringW(L"render_error=suppressed\n");
            }
        }

        void STDMETHODCALLTYPE HookClearTarget(
            ID3D11DeviceContext* context,
            ID3D11RenderTargetView* renderTarget,
            const FLOAT color[4])
        {
            ActiveHookCall activeCall;
            clearOriginal(context, renderTarget, color);
            totalClearCalls.fetch_add(1, std::memory_order_relaxed);
            if (!running.load(std::memory_order_relaxed) || insideHook ||
                !IsMonitorSizedTarget(renderTarget, largestClearTarget))
            {
                return;
            }
            DrawHookSafe(context, renderTarget);
        }

        void STDMETHODCALLTYPE HookBindTargets(
            ID3D11DeviceContext* context,
            const UINT viewCount,
            ID3D11RenderTargetView* const* renderTargets,
            ID3D11DepthStencilView* depthStencil)
        {
            ActiveHookCall activeCall;
            bindOriginal(context, viewCount, renderTargets, depthStencil);
            totalTargetBindings.fetch_add(1, std::memory_order_relaxed);
            if (!renderTargets || insideHook)
            {
                return;
            }
            for (UINT index = 0; index < viewCount; ++index)
            {
                if (renderTargets[index])
                {
                    DrawHookSafe(context, renderTargets[index]);
                }
            }
        }

        bool FindContextHooks(void*& clearTarget, void*& bindTarget)
        {
            D3D_FEATURE_LEVEL featureLevel{};
            ID3D11Device* device{};
            ID3D11DeviceContext* context{};
            const D3D_FEATURE_LEVEL requested[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
            HRESULT result = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                requested,
                static_cast<UINT>(std::size(requested)),
                D3D11_SDK_VERSION,
                &device,
                &featureLevel,
                &context);
            if (FAILED(result))
            {
                return false;
            }
            void** vtable = *reinterpret_cast<void***>(context);
            clearTarget = vtable[ClearTargetSlot];
            bindTarget = vtable[BindTargetsSlot];
            context->Release();
            device->Release();
            return clearTarget && bindTarget;
        }
    }

    DWORD StartOutputHook()
    {
        std::scoped_lock lock(hookMutex);
        if (hookInstalled.load(std::memory_order_acquire))
        {
            return ERROR_BUSY;
        }
        if (running.exchange(true))
        {
            return ERROR_ALREADY_EXISTS;
        }
        firstFrameDrawn.store(false, std::memory_order_release);
        monitorTargetMatches.store(0, std::memory_order_release);
        rendererAttempts.store(0, std::memory_order_release);
        totalClearCalls.store(0, std::memory_order_release);
        largestClearTarget.store(0, std::memory_order_release);
        totalTargetBindings.store(0, std::memory_order_release);
        largestBoundTarget.store(0, std::memory_order_release);
        lastDrawTick.store(0, std::memory_order_release);

        void* clearTarget{};
        void* bindTarget{};
        if (!FindContextHooks(clearTarget, bindTarget))
        {
            running = false;
            return ERROR_NOT_SUPPORTED;
        }
        if (MH_Initialize() != MH_OK)
        {
            running = false;
            return ERROR_INVALID_FUNCTION;
        }
        if (MH_CreateHook(clearTarget, &HookClearTarget, reinterpret_cast<void**>(&clearOriginal)) != MH_OK)
        {
            MH_Uninitialize();
            running = false;
            return ERROR_INVALID_FUNCTION;
        }
        if (MH_CreateHook(bindTarget, &HookBindTargets, reinterpret_cast<void**>(&bindOriginal)) != MH_OK)
        {
            MH_RemoveHook(clearTarget);
            MH_Uninitialize();
            running = false;
            return ERROR_INVALID_FUNCTION;
        }
        clearHookTarget = clearTarget;
        bindHookTarget = bindTarget;
        hookInstalled.store(true, std::memory_order_release);
        if (MH_EnableHook(clearTarget) != MH_OK)
        {
            running = false;
            return ERROR_INVALID_FUNCTION;
        }
        if (MH_EnableHook(bindTarget) != MH_OK)
        {
            running = false;
            return ERROR_INVALID_FUNCTION;
        }
        const HWND progman = FindWindowW(L"Progman", nullptr);
        if (progman)
        {
            RedrawWindow(
                progman,
                nullptr,
                nullptr,
                RDW_INVALIDATE | RDW_INTERNALPAINT | RDW_ALLCHILDREN);
        }
        OutputDebugStringW(L"dwm_hook=enabled\n");
        return ERROR_SUCCESS;
    }

    DWORD StopOutputHook()
    {
        std::scoped_lock lock(hookMutex);
        running.store(false, std::memory_order_release);
        if (!hookInstalled.load(std::memory_order_acquire))
        {
            return ERROR_SUCCESS;
        }
        if (clearHookTarget)
        {
            const auto result=MH_DisableHook(clearHookTarget);
            if(result!=MH_OK && result!=MH_ERROR_DISABLED)
            {
                RestoreWallpaper();
                return ERROR_BUSY;
            }
        }
        if (bindHookTarget)
        {
            const auto result=MH_DisableHook(bindHookTarget);
            if(result!=MH_OK && result!=MH_ERROR_DISABLED)
            {
                RestoreWallpaper();
                return ERROR_BUSY;
            }
        }
        const auto waitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (activeHookCalls.load(std::memory_order_acquire) != 0 &&
               std::chrono::steady_clock::now() < waitDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (activeHookCalls.load(std::memory_order_acquire) != 0)
        {
            OutputDebugStringW(L"dwm_hook=timeout module=loaded\n");
            RestoreWallpaper();
            return ERROR_BUSY;
        }
        try
        {
            renderer.Shutdown();
        }
        catch(...)
        {
            RestoreWallpaper();
            return ERROR_GEN_FAILURE;
        }
        RestoreWallpaper();
        firstFrameDrawn.store(false, std::memory_order_release);
        if (clearHookTarget)
        {
            const auto result=MH_RemoveHook(clearHookTarget);
            if(result!=MH_OK && result!=MH_ERROR_NOT_CREATED)
                return ERROR_BUSY;
        }
        if (bindHookTarget)
        {
            const auto result=MH_RemoveHook(bindHookTarget);
            if(result!=MH_OK && result!=MH_ERROR_NOT_CREATED)
                return ERROR_BUSY;
        }
        const auto uninitResult=MH_Uninitialize();
        if(uninitResult!=MH_OK && uninitResult!=MH_ERROR_NOT_INITIALIZED)
            return ERROR_BUSY;
        hookInstalled.store(false, std::memory_order_release);
        clearHookTarget = nullptr;
        bindHookTarget = nullptr;
        clearOriginal = nullptr;
        bindOriginal = nullptr;
        OutputDebugStringW(L"dwm_hook=disabled\n");
        return ERROR_SUCCESS;
    }

    DWORD AwaitHookReady(const DWORD timeoutMs)
    {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(std::min(timeoutMs, 10000ul));
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (!hookInstalled.load(std::memory_order_acquire))
            {
                return ERROR_INVALID_STATE;
            }
            if (firstFrameDrawn.load(std::memory_order_acquire))
            {
                return ERROR_SUCCESS;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (monitorTargetMatches.load(std::memory_order_acquire) == 0)
        {
            return ERROR_NOT_FOUND;
        }
        if (rendererAttempts.load(std::memory_order_acquire) == 0)
        {
            return ERROR_RETRY;
        }
        return WAIT_TIMEOUT;
    }

    DWORD GetHookDiag()
    {
        uint64_t dimensions = largestBoundTarget.load(std::memory_order_acquire);
        if (!dimensions)
        {
            dimensions = largestClearTarget.load(std::memory_order_acquire);
        }
        const uint32_t width = static_cast<uint32_t>(dimensions >> 32);
        const uint32_t height = static_cast<uint32_t>(dimensions);
        if (totalClearCalls.load(std::memory_order_acquire) == 0 &&
            totalTargetBindings.load(std::memory_order_acquire) == 0)
        {
            return 0;
        }
        return ((std::min(width, 65535u) & 0xffffu) << 16) |
            (std::min(height, 65535u) & 0xffffu);
    }

    DWORD GetRenderDiag()
    {
        DWORD result = renderer.Diagnostics();
        if (wallpaperHidden.load(std::memory_order_acquire)) result |= 0x00010000;
        if (firstFrameDrawn.load(std::memory_order_acquire)) result |= 0x00020000;
        return result;
    }
}
