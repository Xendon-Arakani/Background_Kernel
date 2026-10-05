#include "dwm_hook/HookRenderer.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <new>

using Microsoft::WRL::ComPtr;

namespace bk::dwm_hook
{
    namespace
    {
        constexpr DXGI_FORMAT SceneFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

        constexpr char BlitShaderSource[] = R"hlsl(
            struct VOut { float4 position:SV_POSITION; float2 uv:TEXCOORD0; };
            VOut BlitVS(uint id:SV_VertexID)
            {
                VOut o;
                o.uv=float2((id<<1)&2,id&2);
                o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);
                return o;
            }
            Texture2D<float4> sourceTexture:register(t0);
            SamplerState sourceSampler:register(s0);
            float4 BlitPS(VOut i):SV_TARGET { return sourceTexture.Sample(sourceSampler,i.uv); }
        )hlsl";

        ComPtr<ID3DBlob> Compile(const char* source, const char* entryPoint, const char* profile)
        {
            ComPtr<ID3DBlob> bytecode;
            ComPtr<ID3DBlob> errors;
            UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
            if (FAILED(D3DCompile(source, std::strlen(source), "DwmHookModule", nullptr, nullptr, entryPoint, profile, flags, 0, &bytecode, &errors)))
            {
                if (errors)
                {
                    OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
                }
                return nullptr;
            }
            return bytecode;
        }

        D3D12_HEAP_PROPERTIES HeapProperties(const D3D12_HEAP_TYPE type)
        {
            D3D12_HEAP_PROPERTIES properties{};
            properties.Type = type;
            properties.CreationNodeMask = 1;
            properties.VisibleNodeMask = 1;
            return properties;
        }

        D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            return barrier;
        }
    }

    struct HookRenderer::SharedScene
    {
        ComPtr<ID3D12Resource> sceneTexture;
        ComPtr<ID3D11Texture2D> renderTexture;
        ComPtr<ID3D11ShaderResourceView> shaderResource;
        ComPtr<ID3D12CommandAllocator> sceneAllocator;
        std::atomic_uint64_t sceneReady{0};
        std::atomic_uint64_t renderDone{0};
    };

    HookRenderer::HookRenderer(const bool wakeDesktop) noexcept
        : wakeDesktop_(wakeDesktop)
    {
    }

    HookRenderer::~HookRenderer()
    {
        Shutdown();
    }

    bool HookRenderer::EnsureInitialized(ID3D11RenderTargetView* renderTarget)
    {
        if (initialized_.load(std::memory_order_acquire))
        {
            return true;
        }
        std::scoped_lock lock(initMutex_);
        if (initialized_.load(std::memory_order_relaxed))
        {
            return true;
        }

        stage_.store(1, std::memory_order_release);

        ComPtr<ID3D11Resource> resource;
        renderTarget->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (!resource || FAILED(resource.As(&texture)))
        {
            return false;
        }
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        ComPtr<ID3D11Device> device;
        texture->GetDevice(&device);
        if (!device || !Initialize(device.Get(), description.Width, description.Height))
        {
            Shutdown();
            return false;
        }
        initialized_.store(true, std::memory_order_release);
        stop_.store(false, std::memory_order_release);
        renderThread_ = std::thread(&HookRenderer::RenderLoop, this);
        stage_.store(9, std::memory_order_release);
        return true;
    }

    bool HookRenderer::Initialize(ID3D11Device* renderDevice, const uint32_t width, const uint32_t height)
    {
        width_ = width;
        height_ = height;
        surfaces_ = new (std::nothrow) std::array<SharedScene, SurfaceCount>();
        if (!surfaces_)
        {
            stage_.store(0x80000101, std::memory_order_release);
            return false;
        }

        renderDevice_ = renderDevice;
        if (FAILED(renderDevice_.As(&fenceDevice_)))
        {
            stage_.store(0x80000102, std::memory_order_release);
            return false;
        }
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(renderDevice_.As(&dxgiDevice)))
        {
            stage_.store(0x80000103, std::memory_order_release);
            return false;
        }
        if (FAILED(dxgiDevice->GetAdapter(&adapter)))
        {
            stage_.store(0x80000104, std::memory_order_release);
            return false;
        }
        if (!CreateBlitPipeline(renderDevice)) return false;
        stage_.store(2, std::memory_order_release);
        if (!CreateSceneQueue(adapter.Get())) return false;
        stage_.store(3, std::memory_order_release);
        if (!CreateFenceBridge(renderDevice)) return false;
        stage_.store(4, std::memory_order_release);
        if (!CreateSurfaces(renderDevice)) return false;
        stage_.store(5, std::memory_order_release);
        if (!CreateClosedList()) return false;
        stage_.store(6, std::memory_order_release);
        stage_.store(8, std::memory_order_release);
        return true;
    }

    bool HookRenderer::CreateBlitPipeline(ID3D11Device* renderDevice)
    {
        const auto vertexShader = Compile(BlitShaderSource, "BlitVS", "vs_5_0");
        const auto pixelShader = Compile(BlitShaderSource, "BlitPS", "ps_5_0");
        if (!vertexShader || !pixelShader)
        {
            stage_.store(0x80000201, std::memory_order_release);
            return false;
        }
        if (FAILED(renderDevice->CreateVertexShader(
                vertexShader->GetBufferPointer(), vertexShader->GetBufferSize(), nullptr, &blitVertexShader_)))
        {
            stage_.store(0x80000202, std::memory_order_release);
            return false;
        }
        if (FAILED(renderDevice->CreatePixelShader(
                pixelShader->GetBufferPointer(), pixelShader->GetBufferSize(), nullptr, &blitPixelShader_)))
        {
            stage_.store(0x80000203, std::memory_order_release);
            return false;
        }
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(renderDevice->CreateSamplerState(&sampler, &blitSampler_)))
        {
            stage_.store(0x80000204, std::memory_order_release);
            return false;
        }

        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_DEPTH_STENCIL_DESC depthStencil{};
        depthStencil.DepthEnable = FALSE;
        depthStencil.StencilEnable = FALSE;
        if (FAILED(renderDevice->CreateRasterizerState(&rasterizer, &blitRasterizer_)) ||
            FAILED(renderDevice->CreateBlendState(&blend, &blitBlendState_)) ||
            FAILED(renderDevice->CreateDepthStencilState(&depthStencil, &blitDepthState_)))
        {
            stage_.store(0x80000205, std::memory_order_release);
            return false;
        }
        return true;
    }

    bool HookRenderer::CreateSceneQueue(IDXGIAdapter* adapter)
    {
        if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&sceneDevice_))))
        {
            return false;
        }
        D3D12_COMMAND_QUEUE_DESC queue{};
        queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        return SUCCEEDED(sceneDevice_->CreateCommandQueue(&queue, IID_PPV_ARGS(&drawQueue_)));
    }

    bool HookRenderer::CreateFenceBridge(ID3D11Device*)
    {
        if (FAILED(sceneDevice_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&sceneReadyFence_))) ||
            FAILED(sceneDevice_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&sceneDoneFence_))))
        {
            return false;
        }
        HANDLE readyHandle{};
        HANDLE retiredHandle{};
        if (FAILED(sceneDevice_->CreateSharedHandle(sceneReadyFence_.Get(), nullptr, GENERIC_ALL, nullptr, &readyHandle)) ||
            FAILED(sceneDevice_->CreateSharedHandle(sceneDoneFence_.Get(), nullptr, GENERIC_ALL, nullptr, &retiredHandle)))
        {
            if (readyHandle) CloseHandle(readyHandle);
            if (retiredHandle) CloseHandle(retiredHandle);
            return false;
        }
        const HRESULT readyResult = fenceDevice_->OpenSharedFence(readyHandle, IID_PPV_ARGS(&renderReadyFence_));
        const HRESULT retiredResult = fenceDevice_->OpenSharedFence(retiredHandle, IID_PPV_ARGS(&renderDoneFence_));
        CloseHandle(readyHandle);
        CloseHandle(retiredHandle);
        return SUCCEEDED(readyResult) && SUCCEEDED(retiredResult);
    }

    bool HookRenderer::CreateSurfaces(ID3D11Device* renderDevice)
    {
        ComPtr<ID3D11Device1> sharedDevice;
        if (FAILED(renderDevice->QueryInterface(IID_PPV_ARGS(&sharedDevice))))
        {
            return false;
        }
        D3D12_CLEAR_VALUE clear{};
        clear.Format = SceneFormat;
        clear.Color[0] = 0.0f;
        clear.Color[1] = 0.0f;
        clear.Color[2] = 0.0f;
        clear.Color[3] = 1.0f;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = width_;
        description.Height = height_;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = SceneFormat;
        description.SampleDesc = {1, 0};
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);

        D3D12_DESCRIPTOR_HEAP_DESC targetHeapDesc{};
        targetHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        targetHeapDesc.NumDescriptors = SurfaceCount;
        if (FAILED(sceneDevice_->CreateDescriptorHeap(
            &targetHeapDesc,
            IID_PPV_ARGS(&targetHeap_))))
        {
            return false;
        }
        const UINT targetStride =
            sceneDevice_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        for (uint32_t index = 0; index < SurfaceCount; ++index)
        {
            SharedScene& surface = (*surfaces_)[index];
            if (FAILED(sceneDevice_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&surface.sceneAllocator))) ||
                FAILED(sceneDevice_->CreateCommittedResource(
                    &defaultHeap,
                    D3D12_HEAP_FLAG_SHARED,
                    &description,
                    D3D12_RESOURCE_STATE_COMMON,
                    &clear,
                    IID_PPV_ARGS(&surface.sceneTexture))))
            {
                return false;
            }
            HANDLE textureHandle{};
            if (FAILED(sceneDevice_->CreateSharedHandle(surface.sceneTexture.Get(), nullptr, GENERIC_ALL, nullptr, &textureHandle)))
            {
                return false;
            }
            const HRESULT openResult = sharedDevice->OpenSharedResource1(textureHandle, IID_PPV_ARGS(&surface.renderTexture));
            CloseHandle(textureHandle);
            if (FAILED(openResult) || FAILED(renderDevice->CreateShaderResourceView(surface.renderTexture.Get(), nullptr, &surface.shaderResource)))
            {
                return false;
            }

            D3D12_RENDER_TARGET_VIEW_DESC targetViewDesc{};
            targetViewDesc.Format = SceneFormat;
            targetViewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE renderTarget =
                targetHeap_->GetCPUDescriptorHandleForHeapStart();
            renderTarget.ptr += static_cast<SIZE_T>(index) * targetStride;
            sceneDevice_->CreateRenderTargetView(
                surface.sceneTexture.Get(),
                &targetViewDesc,
                renderTarget);
        }
        return true;
    }

    bool HookRenderer::CreateClosedList()
    {
        if (FAILED(sceneDevice_->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            (*surfaces_)[0].sceneAllocator.Get(),
            nullptr,
            IID_PPV_ARGS(&drawList_))))
        {
            return false;
        }
        return SUCCEEDED(drawList_->Close());
    }

    uint32_t HookRenderer::SelectWriteSurface() const
    {
        const uint32_t displayed=shownSurface_.load(std::memory_order_acquire);
        const uint32_t latest=latestSurface_.load(std::memory_order_acquire);
        for(uint32_t index=0;index<SurfaceCount;++index)
        {
            const uint64_t submittedFence =
                (*surfaces_)[index].sceneReady.load(std::memory_order_acquire);
            const bool allocatorReady = submittedFence == 0 ||
                sceneReadyFence_->GetCompletedValue() >= submittedFence;
            if(index!=displayed && index!=latest && allocatorReady) return index;
        }
        return InvalidSurface;
    }

    void HookRenderer::RenderLoop()
    {
        using clock=std::chrono::steady_clock;
        auto nextFrame=clock::now();
        while(!stop_.load(std::memory_order_acquire))
        {
            const uint32_t surface=SelectWriteSurface();
            if(surface!=InvalidSurface)
            {
                if (RenderSurface(surface))
                {
                    stage_.store(10, std::memory_order_release);
                }
                else
                {
                    stage_.store(0x8000000a, std::memory_order_release);
                }
            }
            WakeDesktop();
            nextFrame+=std::chrono::microseconds(6944);
            std::this_thread::sleep_until(nextFrame);
            if(clock::now()-nextFrame>std::chrono::milliseconds(100)) nextFrame=clock::now();
        }
    }

    bool HookRenderer::RenderSurface(const uint32_t surfaceIndex)
    {
        SharedScene& surface=(*surfaces_)[surfaceIndex];
        const uint64_t retired=surface.renderDone.load(std::memory_order_acquire);
        if(retired && FAILED(drawQueue_->Wait(sceneDoneFence_.Get(),retired))) return false;
        if(FAILED(surface.sceneAllocator->Reset()) || FAILED(drawList_->Reset(surface.sceneAllocator.Get(),nullptr))) return false;

        const auto toRender=Transition(surface.sceneTexture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
        drawList_->ResourceBarrier(1,&toRender);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = targetHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(surfaceIndex) *
            sceneDevice_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        constexpr float clear[4]{0,0,0,1};
        drawList_->ClearRenderTargetView(rtv,clear,0,nullptr);
        const auto toCommon=Transition(surface.sceneTexture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);
        drawList_->ResourceBarrier(1,&toCommon);
        if(FAILED(drawList_->Close())) return false;
        ID3D12CommandList* lists[]{drawList_.Get()};
        drawQueue_->ExecuteCommandLists(1,lists);
        const uint64_t ready=nextReadyFence_.fetch_add(1,std::memory_order_relaxed)+1;
        if(FAILED(drawQueue_->Signal(sceneReadyFence_.Get(),ready))) return false;
        surface.sceneReady.store(ready,std::memory_order_release);
        latestSurface_.store(surfaceIndex,std::memory_order_release);
        return true;
    }

    bool HookRenderer::DrawSurface(ID3D11DeviceContext* immediateContext,ID3D11RenderTargetView* renderTarget,const uint32_t surfaceIndex)
    {
        ComPtr<ID3D11DeviceContext4> fenceContext;
        if(FAILED(immediateContext->QueryInterface(IID_PPV_ARGS(&fenceContext)))) return false;
        SharedScene& surface=(*surfaces_)[surfaceIndex];
        const uint64_t ready=surface.sceneReady.load(std::memory_order_acquire);
        if(!ready) return false;
        if(FAILED(fenceContext->Wait(renderReadyFence_.Get(),ready))) return false;

        struct PipeStateRestore final
        {
            ID3D11DeviceContext* context;
            std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> renderTargets{};
            ID3D11DepthStencilView* depthView{};
            ID3D11RasterizerState* rasterizer{};
            std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
            UINT viewportCount{static_cast<UINT>(viewports.size())};
            std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors{};
            UINT scissorCount{static_cast<UINT>(scissors.size())};
            ID3D11BlendState* blend{};
            FLOAT blendFactor[4]{};
            UINT sampleMask{};
            ID3D11DepthStencilState* depthStencil{};
            UINT stencilReference{};
            ID3D11VertexShader* vertexShader{};
            ID3D11HullShader* hullShader{};
            ID3D11DomainShader* domainShader{};
            ID3D11GeometryShader* geometryShader{};
            ID3D11PixelShader* pixelShader{};
            ID3D11ShaderResourceView* pixelResource{};
            ID3D11SamplerState* pixelSampler{};
            ID3D11InputLayout* inputLayout{};
            D3D11_PRIMITIVE_TOPOLOGY topology{};
            ID3D11Predicate* predicate{};
            BOOL predicateValue{};

            explicit PipeStateRestore(ID3D11DeviceContext* source) : context(source)
            {
                context->OMGetRenderTargets(
                    static_cast<UINT>(renderTargets.size()), renderTargets.data(), &depthView);
                context->RSGetState(&rasterizer);
                context->RSGetViewports(&viewportCount, viewports.data());
                context->RSGetScissorRects(&scissorCount, scissors.data());
                context->OMGetBlendState(&blend, blendFactor, &sampleMask);
                context->OMGetDepthStencilState(&depthStencil, &stencilReference);
                context->VSGetShader(&vertexShader, nullptr, nullptr);
                context->HSGetShader(&hullShader, nullptr, nullptr);
                context->DSGetShader(&domainShader, nullptr, nullptr);
                context->GSGetShader(&geometryShader, nullptr, nullptr);
                context->PSGetShader(&pixelShader, nullptr, nullptr);
                context->PSGetShaderResources(0, 1, &pixelResource);
                context->PSGetSamplers(0, 1, &pixelSampler);
                context->IAGetInputLayout(&inputLayout);
                context->IAGetPrimitiveTopology(&topology);
                context->GetPredication(&predicate, &predicateValue);
            }

            ~PipeStateRestore()
            {
                context->SetPredication(predicate, predicateValue);
                context->IASetInputLayout(inputLayout);
                context->IASetPrimitiveTopology(topology);
                context->VSSetShader(vertexShader, nullptr, 0);
                context->HSSetShader(hullShader, nullptr, 0);
                context->DSSetShader(domainShader, nullptr, 0);
                context->GSSetShader(geometryShader, nullptr, 0);
                context->PSSetShader(pixelShader, nullptr, 0);
                context->PSSetShaderResources(0, 1, &pixelResource);
                context->PSSetSamplers(0, 1, &pixelSampler);
                context->RSSetState(rasterizer);
                context->RSSetViewports(viewportCount, viewports.data());
                context->RSSetScissorRects(scissorCount, scissors.data());
                context->OMSetBlendState(blend, blendFactor, sampleMask);
                context->OMSetDepthStencilState(depthStencil, stencilReference);
                context->OMSetRenderTargets(
                    static_cast<UINT>(renderTargets.size()), renderTargets.data(), depthView);

                for (ID3D11RenderTargetView* value : renderTargets) if(value) value->Release();
                if(depthView) depthView->Release();
                if(rasterizer) rasterizer->Release();
                if(blend) blend->Release();
                if(depthStencil) depthStencil->Release();
                if(vertexShader) vertexShader->Release();
                if(hullShader) hullShader->Release();
                if(domainShader) domainShader->Release();
                if(geometryShader) geometryShader->Release();
                if(pixelShader) pixelShader->Release();
                if(pixelResource) pixelResource->Release();
                if(pixelSampler) pixelSampler->Release();
                if(inputLayout) inputLayout->Release();
                if(predicate) predicate->Release();
            }
        } pipelineGuard{immediateContext};

        const D3D11_VIEWPORT viewport{0,0,static_cast<float>(width_),static_cast<float>(height_),0,1};
        immediateContext->RSSetViewports(1,&viewport);
        immediateContext->RSSetState(blitRasterizer_.Get());
        immediateContext->OMSetRenderTargets(1,&renderTarget,nullptr);
        constexpr FLOAT opaqueBlendFactor[4]{};
        immediateContext->OMSetBlendState(blitBlendState_.Get(),opaqueBlendFactor,UINT_MAX);
        immediateContext->OMSetDepthStencilState(blitDepthState_.Get(),0);
        immediateContext->SetPredication(nullptr,FALSE);
        immediateContext->VSSetShader(blitVertexShader_.Get(),nullptr,0);
        immediateContext->HSSetShader(nullptr,nullptr,0);
        immediateContext->DSSetShader(nullptr,nullptr,0);
        immediateContext->GSSetShader(nullptr,nullptr,0);
        immediateContext->PSSetShader(blitPixelShader_.Get(),nullptr,0);
        ID3D11ShaderResourceView* srv=surface.shaderResource.Get();
        ID3D11SamplerState* sampler=blitSampler_.Get();
        immediateContext->PSSetShaderResources(0,1,&srv);
        immediateContext->PSSetSamplers(0,1,&sampler);
        immediateContext->IASetInputLayout(nullptr);
        immediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        immediateContext->Draw(3,0);
        return true;
    }

    bool HookRenderer::Draw(ID3D11DeviceContext* immediateContext,ID3D11RenderTargetView* renderTarget)
    {
        if (drawActive_.test_and_set(std::memory_order_acquire))
        {
            return false;
        }
        struct DrawFlagReset final
        {
            std::atomic_flag& flag;
            ~DrawFlagReset() { flag.clear(std::memory_order_release); }
        } drawGuard{drawActive_};

        ComPtr<ID3D11Device> contextDevice;
        immediateContext->GetDevice(&contextDevice);
        if (initialized_.load(std::memory_order_acquire) &&
            contextDevice.Get() != renderDevice_.Get())
        {
            return false;
        }
        if(!EnsureInitialized(renderTarget)) return false;
        const uint32_t latest=latestSurface_.load(std::memory_order_acquire);
        if(latest==InvalidSurface) return false;
        const uint32_t previous=shownSurface_.load(std::memory_order_acquire);
        if(!DrawSurface(immediateContext,renderTarget,latest)) return false;
        stage_.store(11, std::memory_order_release);
        if(previous!=InvalidSurface && previous!=latest)
        {
            ComPtr<ID3D11DeviceContext4> fenceContext;
            if(SUCCEEDED(immediateContext->QueryInterface(IID_PPV_ARGS(&fenceContext))))
            {
                const uint64_t retired=nextDoneFence_.fetch_add(1,std::memory_order_relaxed)+1;
                if(SUCCEEDED(fenceContext->Signal(renderDoneFence_.Get(),retired)))
                {
                    (*surfaces_)[previous].renderDone.store(retired,std::memory_order_release);
                }
            }
        }
        shownSurface_.store(latest,std::memory_order_release);
        return true;
    }

    void HookRenderer::WakeDesktop() const
    {
        if (!wakeDesktop_)
        {
            return;
        }
        const HWND progman=FindWindowW(L"Progman",nullptr);
        if(progman) RedrawWindow(progman,nullptr,nullptr,RDW_INVALIDATE|RDW_INTERNALPAINT|RDW_ALLCHILDREN);
    }

    void HookRenderer::Shutdown()
    {
        stop_.store(true,std::memory_order_release);
        if (sceneDoneFence_)
        {
            constexpr uint64_t stopDoneFence = UINT64_MAX;
            sceneDoneFence_->Signal(stopDoneFence);
        }
        if(renderThread_.joinable()) renderThread_.join();
        initialized_.store(false,std::memory_order_release);
        latestSurface_=InvalidSurface;
        shownSurface_=InvalidSurface;
        if(drawQueue_ && sceneReadyFence_)
        {
            const uint64_t value=nextReadyFence_.fetch_add(1)+1;
            drawQueue_->Signal(sceneReadyFence_.Get(),value);
            HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            if(event)
            {
                sceneReadyFence_->SetEventOnCompletion(value,event);
                WaitForSingleObject(event,5000);
                CloseHandle(event);
            }
        }
        delete surfaces_;
        surfaces_=nullptr;
        renderDevice_.Reset(); fenceDevice_.Reset(); blitVertexShader_.Reset(); blitPixelShader_.Reset(); blitSampler_.Reset();
        blitRasterizer_.Reset(); blitBlendState_.Reset(); blitDepthState_.Reset();
        targetHeap_.Reset();
        sceneDevice_.Reset(); drawQueue_.Reset(); drawList_.Reset();
        sceneReadyFence_.Reset(); sceneDoneFence_.Reset(); renderReadyFence_.Reset(); renderDoneFence_.Reset();
        nextReadyFence_ = 0;
        nextDoneFence_ = 0;
    }
}
