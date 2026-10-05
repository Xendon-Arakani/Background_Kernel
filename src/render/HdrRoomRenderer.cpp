#include "render/HdrRoomRenderer.hpp"
#include "render/HdrToneMapper.hpp"
#include "stroke/WallStroke.hpp"
#include "visualizer/AdaptiveTrail.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

namespace bk::render
{
    namespace
    {
        constexpr DXGI_FORMAT BackBufferFormat =
            DXGI_FORMAT_R16G16B16A16_FLOAT;
        constexpr std::uint32_t ConstantAlign = 256;
        constexpr std::size_t CapsuleVertexCount = 6;

        void ThrowIfFailed(const HRESULT result, const char* operation)
        {
            if (FAILED(result))
            {
                char message[192]{};
                sprintf_s(
                    message,
                    "%s failed with HRESULT 0x%08X",
                    operation,
                    static_cast<unsigned int>(result));
                throw std::runtime_error(message);
            }
        }

        D3D12_HEAP_PROPERTIES UploadHeapProperties()
        {
            D3D12_HEAP_PROPERTIES properties{};
            properties.Type = D3D12_HEAP_TYPE_UPLOAD;
            properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            properties.CreationNodeMask = 1;
            properties.VisibleNodeMask = 1;
            return properties;
        }

        D3D12_RESOURCE_DESC BufferDescription(
            const std::uint64_t byteCount)
        {
            D3D12_RESOURCE_DESC description{};
            description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            description.Width = byteCount;
            description.Height = 1;
            description.DepthOrArraySize = 1;
            description.MipLevels = 1;
            description.Format = DXGI_FORMAT_UNKNOWN;
            description.SampleDesc = {1, 0};
            description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            return description;
        }

        D3D12_RESOURCE_BARRIER Transition(
            ID3D12Resource* resource,
            const D3D12_RESOURCE_STATES before,
            const D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            return barrier;
        }

        ComPtr<ID3DBlob> CompileShader(
            const char* source,
            const char* entryPoint,
            const char* target)
        {
            UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
            flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
            flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
            ComPtr<ID3DBlob> bytecode;
            ComPtr<ID3DBlob> errors;
            const HRESULT result = D3DCompile(
                source,
                std::strlen(source),
                "BkHdrResolve",
                nullptr,
                nullptr,
                entryPoint,
                target,
                flags,
                0,
                &bytecode,
                &errors);
            if (FAILED(result))
            {
                const std::string errorText = errors
                    ? std::string(
                        static_cast<const char*>(errors->GetBufferPointer()),
                        errors->GetBufferSize())
                    : std::string("Unknown shader compilation error");
                throw std::runtime_error(errorText);
            }
            return bytecode;
        }

        constexpr char HdrResolveShader[] = R"hlsl(
            cbuffer Resolve : register(b0)
            {
                float ScenePeakNits;
                float TargetBlackNits;
                float TargetPeakNits;
                float ScRgbWhiteNits;
                float2 DotCenterPx;
                float DotRadiusPx;
                float DotEnabled;
                float3 DotSceneNits;
                float DotPad;
                float2 SurfaceSizePx;
                float2 SurfacePad;
            };

            float3 SceneLinear(float3 nits) { return max(nits,0)/80; }

            struct PixelInput
            {
                float4 position : SV_POSITION;
            };

            PixelInput FullscreenVS(uint vertexId : SV_VertexID)
            {
                PixelInput output;
                if (vertexId == 0)
                {
                    output.position = float4(-1.0f, -1.0f, 0.0f, 1.0f);
                }
                else if (vertexId == 1)
                {
                    output.position = float4(-1.0f, 3.0f, 0.0f, 1.0f);
                }
                else
                {
                    output.position = float4(3.0f, -1.0f, 0.0f, 1.0f);
                }
                return output;
            }

            float4 HdrResolvePS(PixelInput input) : SV_TARGET
            {
                if (DotEnabled < 0.5f)
                {
                    discard;
                }

                const float2 coverageOffsets[4] =
                {
                    float2(-0.25f, -0.25f),
                    float2(+0.25f, -0.25f),
                    float2(-0.25f, +0.25f),
                    float2(+0.25f, +0.25f)
                };
                float coverage = 0.0f;
                [unroll]
                for (uint sampleIndex = 0; sampleIndex < 4; ++sampleIndex)
                {
                    const float2 delta =
                        input.position.xy + coverageOffsets[sampleIndex] -
                        DotCenterPx;
                    coverage += dot(delta, delta) <=
                        DotRadiusPx * DotRadiusPx
                        ? 1.0f
                        : 0.0f;
                }
                coverage *= 0.25f;
                if (coverage <= 0.0f)
                {
                    discard;
                }
                return float4(
                    SceneLinear(DotSceneNits) * coverage,
                    coverage);
            }

            struct TrailVertexInput
            {
                float2 corner : POSITION;
                float2 segmentStart : TEXCOORD0;
                float2 segmentEnd : TEXCOORD1;
                float2 radii : TEXCOORD2;
                float3 startColorNits : COLOR0;
                float startOpacity : COLOR1;
                float3 endColorNits : COLOR2;
                float endOpacity : COLOR3;
            };

            struct TrailPixelInput
            {
                float4 position : SV_POSITION;
                nointerpolation float2 segmentStart : TEXCOORD0;
                nointerpolation float2 segmentEnd : TEXCOORD1;
                nointerpolation float2 radii : TEXCOORD2;
                nointerpolation float4 sweepParameters : TEXCOORD3;
                nointerpolation float3 startColorNits : COLOR0;
                nointerpolation float startOpacity : COLOR1;
                nointerpolation float3 endColorNits : COLOR2;
                nointerpolation float endOpacity : COLOR3;
            };

            TrailPixelInput TrailVS(TrailVertexInput input)
            {
                TrailPixelInput output;
                const float2 normalized =
                    input.corner / SurfaceSizePx;
                output.position = float4(
                    normalized.x * 2.0f - 1.0f,
                    1.0f - normalized.y * 2.0f,
                    0.0f,
                    1.0f);
                output.segmentStart = input.segmentStart;
                output.segmentEnd = input.segmentEnd;
                output.radii = input.radii;
                const float2 delta = input.segmentEnd - input.segmentStart;
                const float radiusDelta = input.radii.y - input.radii.x;
                const float quadratic = dot(delta, delta) - radiusDelta * radiusDelta;
                output.sweepParameters = float4(delta, radiusDelta,
                    quadratic > 0.0f ? rcp(quadratic) : 0.0f);
                output.startColorNits = input.startColorNits;
                output.startOpacity = input.startOpacity;
                output.endColorNits = input.endColorNits;
                output.endOpacity = input.endOpacity;
                return output;
            }

            float SweepParameter(float2 position, TrailPixelInput input)
            {
                if (input.sweepParameters.w == 0.0f)
                    return input.radii.y >= input.radii.x ? 1.0f : 0.0f;
                return saturate((dot(position - input.segmentStart, input.sweepParameters.xy) +
                    input.radii.x * input.sweepParameters.z) * input.sweepParameters.w);
            }

            float4 TrailPS(TrailPixelInput input) : SV_TARGET
            {
                const float centerT = SweepParameter(input.position.xy, input);
                const float2 coverageOffsets[4] =
                {
                    float2(-0.25f, -0.25f),
                    float2(+0.25f, -0.25f),
                    float2(-0.25f, +0.25f),
                    float2(+0.25f, +0.25f)
                };
                float coverage = 0.0f;
                [unroll]
                for (uint sampleIndex = 0; sampleIndex < 4; ++sampleIndex)
                {
                    const float2 samplePosition =
                        input.position.xy + coverageOffsets[sampleIndex];
                    const float sampleT = SweepParameter(samplePosition, input);
                    const float2 center = lerp(
                        input.segmentStart, input.segmentEnd, sampleT);
                    const float radius = lerp(
                        input.radii.x, input.radii.y, sampleT);
                    const float2 delta = samplePosition - center;
                    coverage += dot(delta, delta) <= radius * radius
                        ? 1.0f
                        : 0.0f;
                }
                coverage *= 0.25f;
                if (coverage <= 0.0f)
                {
                    discard;
                }
                const float3 colorNits = lerp(
                    input.startColorNits,
                    input.endColorNits,
                    centerT);
                const float pathOpacity = lerp(
                    input.startOpacity,
                    input.endOpacity,
                    centerT);
                return float4(
                    SceneLinear(colorNits) * coverage * pathOpacity,
                    coverage * pathOpacity);
            }

        )hlsl";
    }

    struct HdrRoomRenderer::FrameData
    {
        float scenePeakNits{};
        float targetBlackNits{};
        float targetPeakNits{};
        float scRgbWhiteNits{};
        float dotCenterPx[2]{};
        float dotRadiusPx{};
        float dotEnabled{};
        float dotSceneNits[3]{};
        float dotPad{};
        float surfaceSizePx[2]{};
        float surfacePad[2]{};
    };

    struct HdrRoomRenderer::TrailVertex
    {
        float corner[2]{};
        float segmentStart[2]{};
        float segmentEnd[2]{};
        float radii[2]{};
        float startColorNits[3]{};
        float startOpacity{};
        float endColorNits[3]{};
        float endOpacity{};
    };

    struct HdrRoomRenderer::DisplayLimits
    {
        bool hdrActive{};
        std::uint32_t bitsPerColor{8};
        float minNits{};
        float peakNits{80.0f};
        float fullFrameNits{80.0f};
        float redPrimary[2]{};
        float greenPrimary[2]{};
        float bluePrimary[2]{};
        float whitePoint[2]{};
    };

    HdrRoomRenderer::~HdrRoomRenderer()
    {
        try
        {
            WaitForGpu();
        }
        catch (...)
        {
        }
        if (frameData_ && mappedFrameData_)
        {
            frameData_->Unmap(0, nullptr);
            mappedFrameData_ = nullptr;
        }
        for (std::size_t frame = 0; frame < FrameCount; ++frame)
        {
            if (trailBuffers_[frame] && mappedTrail_[frame])
                trailBuffers_[frame]->Unmap(0, nullptr);
            mappedTrail_[frame] = nullptr;
        }
        if (fenceEvent_)
        {
            CloseHandle(fenceEvent_);
            fenceEvent_ = nullptr;
        }
        delete displayLimits_;
        displayLimits_ = nullptr;
    }

    void HdrRoomRenderer::Initialize(
        const HWND window,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        window_ = window;
        width_ = std::max(width, 1u);
        height_ = std::max(height, 1u);

        CreateDevice();
        displayLimits_ = new DisplayLimits{};
        DetectDisplay();
        CreateSwapChain();
        CreatePipelines();
        CreateViewTargets();
    }

    void HdrRoomRenderer::SetPaperWhiteNits(const float nits)
    {
        if (!hdr::ValidPaperWhiteNits(nits))
        {
            throw std::invalid_argument(
                "Paper white must be finite, positive, and no greater than 10000 nits");
        }
        paperWhiteNits_ = nits;
    }

    void HdrRoomRenderer::SetPresentationSync(const PresentationSync mode)
    {
        if(!ValidSyncMode(mode))
            throw std::invalid_argument("Invalid presentation synchronization mode");
        syncInterval_=mode==PresentationSync::VSync?1u:0u;
        presentFlags_=syncInterval_==0 && canTear_
            ? DXGI_PRESENT_ALLOW_TEARING:0;
    }

    void HdrRoomRenderer::CreateDevice()
    {
        UINT factoryFlags = 0;
#if defined(_DEBUG)
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(
                IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
#endif
        ThrowIfFailed(
            CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory_)),
            "CreateDXGIFactory2");

        ComPtr<IDXGIAdapter1> candidate;
        for (std::uint32_t index = 0;
             factory_->EnumAdapterByGpuPreference(
                 index,
                 DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                 IID_PPV_ARGS(&candidate)) != DXGI_ERROR_NOT_FOUND;
             ++index)
        {
            DXGI_ADAPTER_DESC1 description{};
            candidate->GetDesc1(&description);
            if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            {
                candidate.Reset();
                continue;
            }
            if (SUCCEEDED(D3D12CreateDevice(
                    candidate.Get(),
                    D3D_FEATURE_LEVEL_12_0,
                    __uuidof(ID3D12Device),
                    nullptr)))
            {
                ThrowIfFailed(
                    candidate.As(&adapter_),
                    "Query IDXGIAdapter4");
                break;
            }
            candidate.Reset();
        }
        if (!adapter_)
        {
            ThrowIfFailed(
                factory_->EnumWarpAdapter(IID_PPV_ARGS(&adapter_)),
                "EnumWarpAdapter");
        }
        ThrowIfFailed(
            D3D12CreateDevice(
                adapter_.Get(),
                D3D_FEATURE_LEVEL_12_0,
                IID_PPV_ARGS(&device_)),
            "D3D12CreateDevice");

        D3D12_COMMAND_QUEUE_DESC queueDescription{};
        queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDescription.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        ThrowIfFailed(
            device_->CreateCommandQueue(
                &queueDescription,
                IID_PPV_ARGS(&drawQueue_)),
            "CreateCommandQueue");
        for (auto& allocator : frameAllocators_)
        {
            ThrowIfFailed(
                device_->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(&allocator)),
                "CreateCommandAllocator");
        }
        ThrowIfFailed(
            device_->CreateFence(
                0,
                D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(&fence_)),
            "CreateFence");
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent_)
        {
            ThrowIfFailed(
                HRESULT_FROM_WIN32(GetLastError()),
                "CreateEventW(GPU fence)");
        }
    }

    void HdrRoomRenderer::DetectDisplay()
    {
        if (!displayLimits_ || !adapter_ || !window_)
        {
            return;
        }

        RECT windowBounds{};
        GetWindowRect(window_, &windowBounds);
        std::int64_t bestIntersectionArea = -1;
        DXGI_OUTPUT_DESC1 bestDescription{};
        bool found = false;

        for (std::uint32_t index = 0;; ++index)
        {
            ComPtr<IDXGIOutput> output;
            if (adapter_->EnumOutputs(index, &output) ==
                DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            DXGI_OUTPUT_DESC description{};
            if (FAILED(output->GetDesc(&description)))
            {
                continue;
            }
            const LONG left = std::max(
                windowBounds.left,
                description.DesktopCoordinates.left);
            const LONG top = std::max(
                windowBounds.top,
                description.DesktopCoordinates.top);
            const LONG right = std::min(
                windowBounds.right,
                description.DesktopCoordinates.right);
            const LONG bottom = std::min(
                windowBounds.bottom,
                description.DesktopCoordinates.bottom);
            const std::int64_t area =
                static_cast<std::int64_t>(std::max(right - left, 0L)) *
                static_cast<std::int64_t>(std::max(bottom - top, 0L));
            if (area <= bestIntersectionArea)
            {
                continue;
            }
            ComPtr<IDXGIOutput6> colorOutput;
            DXGI_OUTPUT_DESC1 candidateDescription{};
            if (FAILED(output.As(&colorOutput)) ||
                FAILED(colorOutput->GetDesc1(&candidateDescription)))
            {
                continue;
            }
            bestIntersectionArea = area;
            bestDescription = candidateDescription;
            found = true;
        }

        DisplayLimits detected{};
        if (found)
        {
            detected.hdrActive = bestDescription.ColorSpace ==
                DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            detected.bitsPerColor = bestDescription.BitsPerColor;
            detected.minNits = std::max(
                bestDescription.MinLuminance, 0.0f);
            detected.peakNits =
                bestDescription.MaxLuminance > 1.0f
                ? bestDescription.MaxLuminance
                : 80.0f;
            detected.fullFrameNits =
                bestDescription.MaxFullFrameLuminance > 1.0f
                ? bestDescription.MaxFullFrameLuminance
                : detected.peakNits;
            std::copy_n(
                bestDescription.RedPrimary, 2, detected.redPrimary);
            std::copy_n(
                bestDescription.GreenPrimary, 2, detected.greenPrimary);
            std::copy_n(
                bestDescription.BluePrimary, 2, detected.bluePrimary);
            std::copy_n(
                bestDescription.WhitePoint, 2, detected.whitePoint);
        }
        *displayLimits_ = detected;

        char diagnostic[384]{};
        sprintf_s(
            diagnostic,
            "BackgroundKernel HDR: active=%u bits=%u min=%.6f max=%.3f "
            "fullFrame=%.3f paperWhite=%.3f\n",
            detected.hdrActive ? 1u : 0u,
            detected.bitsPerColor,
            detected.minNits,
            detected.peakNits,
            detected.fullFrameNits,
            paperWhiteNits_);
        OutputDebugStringA(diagnostic);
    }

    void HdrRoomRenderer::RefreshDisplay()
    {
        if (!factory_ || factory_->IsCurrent())
        {
            return;
        }
        DXGI_ADAPTER_DESC3 activeAdapterDesc{};
        if (!adapter_ || FAILED(
                adapter_->GetDesc3(&activeAdapterDesc)))
        {
            return;
        }
        ComPtr<IDXGIFactory6> refreshedFactory;
        if (FAILED(CreateDXGIFactory2(
                0, IID_PPV_ARGS(&refreshedFactory))))
        {
            return;
        }
        ComPtr<IDXGIAdapter1> candidate;
        for (std::uint32_t index = 0;
             refreshedFactory->EnumAdapters1(index, &candidate) !=
                DXGI_ERROR_NOT_FOUND;
             ++index)
        {
            DXGI_ADAPTER_DESC1 description{};
            candidate->GetDesc1(&description);
            if (description.AdapterLuid.HighPart ==
                    activeAdapterDesc.AdapterLuid.HighPart &&
                description.AdapterLuid.LowPart ==
                    activeAdapterDesc.AdapterLuid.LowPart)
            {
                ComPtr<IDXGIAdapter4> refreshedAdapter;
                if (SUCCEEDED(candidate.As(&refreshedAdapter)))
                {
                    adapter_ = refreshedAdapter;
                }
                break;
            }
            candidate.Reset();
        }
        factory_ = refreshedFactory;
        DetectDisplay();
    }

    void HdrRoomRenderer::CreateSwapChain()
    {
        BOOL tearingSupported=FALSE;
        ComPtr<IDXGIFactory5> tearingFactory;
        if(SUCCEEDED(factory_.As(&tearingFactory)))
            tearingFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                          &tearingSupported,sizeof(tearingSupported));
        canTear_=tearingSupported!=FALSE;
        swapFlags_=canTear_?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;

        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = width_;
        description.Height = height_;
        description.Format = BackBufferFormat;
        description.SampleDesc = {1, 0};
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = FrameCount;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        description.Flags=swapFlags_;

        ComPtr<IDXGISwapChain1> swapChain;
        ThrowIfFailed(
            factory_->CreateSwapChainForHwnd(
                drawQueue_.Get(),
                window_,
                &description,
                nullptr,
                nullptr,
                &swapChain),
            "CreateSwapChainForHwnd");
        ThrowIfFailed(
            factory_->MakeWindowAssociation(
                window_, DXGI_MWA_NO_ALT_ENTER),
            "MakeWindowAssociation");
        ThrowIfFailed(
            swapChain.As(&swapChain_),
            "Query IDXGISwapChain4");
        UINT colorSpaceSupport = 0;
        ThrowIfFailed(
            swapChain_->CheckColorSpaceSupport(
                DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
                &colorSpaceSupport),
            "Check scRGB color-space support");
        if ((colorSpaceSupport &
            DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0)
        {
            throw std::runtime_error(
                "The output cannot present an FP16 scRGB swap chain");
        }
        ThrowIfFailed(
            swapChain_->SetColorSpace1(
                DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709),
            "Set FP16 scRGB swap-chain color space");
        frameIndex_ = swapChain_->GetCurrentBackBufferIndex();

        D3D12_DESCRIPTOR_HEAP_DESC rtvDescription{};
        rtvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDescription.NumDescriptors = FrameCount;
        ThrowIfFailed(
            device_->CreateDescriptorHeap(
                &rtvDescription, IID_PPV_ARGS(&rtvHeap_)),
            "Create RTV heap");
        rtvStride_ =
            device_->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    void HdrRoomRenderer::CreatePipelines()
    {
        D3D12_ROOT_PARAMETER rootParameter{};
        rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameter.Descriptor.ShaderRegister = 0;
        rootParameter.Descriptor.RegisterSpace = 0;
        rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootDescription{};
        rootDescription.NumParameters = 1;
        rootDescription.pParameters = &rootParameter;
        rootDescription.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> serializedRoot;
        ComPtr<ID3DBlob> rootErrors;
        ThrowIfFailed(
            D3D12SerializeRootSignature(
                &rootDescription,
                D3D_ROOT_SIGNATURE_VERSION_1,
                &serializedRoot,
                &rootErrors),
            "SerializeRootSignature");
        ThrowIfFailed(
            device_->CreateRootSignature(
                0,
                serializedRoot->GetBufferPointer(),
                serializedRoot->GetBufferSize(),
                IID_PPV_ARGS(&root_)),
            "CreateRootSignature");

        const auto vertexShader = CompileShader(
            HdrResolveShader, "FullscreenVS", "vs_5_1");
        const auto pixelShader = CompileShader(
            HdrResolveShader, "HdrResolvePS", "ps_5_1");
        const auto trailVertexShader = CompileShader(
            HdrResolveShader, "TrailVS", "vs_5_1");
        const auto trailPixelShader = CompileShader(
            HdrResolveShader, "TrailPS", "ps_5_1");

        D3D12_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
        rasterizer.CullMode = D3D12_CULL_MODE_NONE;
        rasterizer.DepthClipEnable = TRUE;
        D3D12_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        D3D12_DEPTH_STENCIL_DESC depthStencil{};
        depthStencil.DepthEnable = FALSE;
        depthStencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = root_.Get();
        pipeline.VS = {
            vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
        pipeline.PS = {
            pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
        D3D12_BLEND_DESC coverageBlend = blend;
        auto& dotRenderTarget = coverageBlend.RenderTarget[0];
        dotRenderTarget.BlendEnable = TRUE;
        dotRenderTarget.SrcBlend = D3D12_BLEND_ONE;
        dotRenderTarget.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        dotRenderTarget.BlendOp = D3D12_BLEND_OP_ADD;
        dotRenderTarget.SrcBlendAlpha = D3D12_BLEND_ONE;
        dotRenderTarget.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        dotRenderTarget.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        pipeline.BlendState = coverageBlend;
        pipeline.SampleMask = UINT_MAX;
        pipeline.RasterizerState = rasterizer;
        pipeline.DepthStencilState = depthStencil;
        pipeline.InputLayout = {nullptr, 0};
        pipeline.PrimitiveTopologyType =
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets = 1;
        pipeline.RTVFormats[0] = BackBufferFormat;
        pipeline.SampleDesc = {1, 0};
        ThrowIfFailed(
            device_->CreateGraphicsPipelineState(
                &pipeline, IID_PPV_ARGS(&dotPipeline_)),
            "Create HDR resolve pipeline");

        constexpr D3D12_INPUT_ELEMENT_DESC trailInput[]
        {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
                static_cast<UINT>(offsetof(TrailVertex, corner)),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
                static_cast<UINT>(offsetof(TrailVertex, segmentStart)),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0,
                static_cast<UINT>(offsetof(TrailVertex, segmentEnd)),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0,
                static_cast<UINT>(offsetof(TrailVertex, radii)),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
                static_cast<UINT>(offsetof(TrailVertex, startColorNits)),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 1, DXGI_FORMAT_R32_FLOAT, 0,
                static_cast<UINT>(offsetof(TrailVertex, startOpacity)),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0,
                static_cast<UINT>(offsetof(TrailVertex, endColorNits)),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 3, DXGI_FORMAT_R32_FLOAT, 0,
                static_cast<UINT>(offsetof(TrailVertex, endOpacity)),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC trailPipelineDesc = pipeline;
        trailPipelineDesc.VS = {
            trailVertexShader->GetBufferPointer(),
            trailVertexShader->GetBufferSize()};
        trailPipelineDesc.PS = {
            trailPixelShader->GetBufferPointer(),
            trailPixelShader->GetBufferSize()};
        trailPipelineDesc.BlendState = coverageBlend;
        trailPipelineDesc.InputLayout = {
            trailInput,
            static_cast<UINT>(std::size(trailInput))};
        ThrowIfFailed(
            device_->CreateGraphicsPipelineState(
                &trailPipelineDesc, IID_PPV_ARGS(&trailPipeline_)),
            "Create swept-trail pipeline");

        ThrowIfFailed(
            device_->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                frameAllocators_[0].Get(),
                dotPipeline_.Get(),
                IID_PPV_ARGS(&drawList_)),
            "CreateCommandList");
        ThrowIfFailed(
            drawList_->Close(),
            "Close initial command list");

        const auto uploadProperties = UploadHeapProperties();
        const auto description = BufferDescription(
            ConstantAlign * FrameCount);
        ThrowIfFailed(
            device_->CreateCommittedResource(
                &uploadProperties,
                D3D12_HEAP_FLAG_NONE,
                &description,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&frameData_)),
            "Create tone-map constant buffer");
        D3D12_RANGE noRead{0, 0};
        ThrowIfFailed(
            frameData_->Map(
                0,
                &noRead,
                reinterpret_cast<void**>(&mappedFrameData_)),
            "Map tone-map constant buffer");
    }

    void HdrRoomRenderer::CreateViewTargets()
    {
        auto handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        for (std::uint32_t frame = 0; frame < FrameCount; ++frame)
        {
            ThrowIfFailed(
                swapChain_->GetBuffer(
                    frame, IID_PPV_ARGS(&backBuffers_[frame])),
                "Get swap-chain buffer");
            device_->CreateRenderTargetView(
                backBuffers_[frame].Get(), nullptr, handle);
            handle.ptr += rtvStride_;
        }
        viewport_ = {
            0.0f,
            0.0f,
            static_cast<float>(width_),
            static_cast<float>(height_),
            0.0f,
            1.0f};
        scissorRect_ = {
            0,
            0,
            static_cast<LONG>(width_),
            static_cast<LONG>(height_)};
    }

    void HdrRoomRenderer::Resize(
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (!swapChain_ || width == 0 || height == 0 ||
            (width == width_ && height == height_))
        {
            return;
        }
        WaitForGpu();
        for (auto& target : backBuffers_)
        {
            target.Reset();
        }
        width_ = width;
        height_ = height;
        ThrowIfFailed(
            swapChain_->ResizeBuffers(
                FrameCount, width_, height_, BackBufferFormat, swapFlags_),
            "ResizeBuffers");
        frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
        CreateViewTargets();
    }

    void HdrRoomRenderer::UpdateFrameData(
        const visualizer::TrailFrame* visualizerFrame)
    {
        const bool hdrActive =
            displayLimits_ && displayLimits_->hdrActive;
        const float targetMinNits = hdrActive
            ? displayLimits_->minNits
            : 0.0f;
        const float detectedPeak = hdrActive
            ? displayLimits_->peakNits
            : 80.0f;
        const float targetMaxNits = hdrActive
            ? std::clamp(
                detectedPeak,
                80.0f,
                static_cast<float>(hdr::SourcePeakNits))
            : 80.0f;

        FrameData constants{};
        static_assert(sizeof(FrameData) <= ConstantAlign);
        constants.scenePeakNits=static_cast<float>(hdr::SourcePeakNits);
        constants.targetBlackNits=targetMinNits;
        constants.targetPeakNits=targetMaxNits;
        constants.scRgbWhiteNits=static_cast<float>(hdr::ScRgbWhiteNits);
        constants.surfaceSizePx[0]=static_cast<float>(width_);
        constants.surfaceSizePx[1]=static_cast<float>(height_);
        if (visualizerFrame)
        {
            constants.dotCenterPx[0] = static_cast<float>(visualizerFrame->dotXmm*width_/scene::ScreenChordMm);
            constants.dotCenterPx[1] = static_cast<float>(visualizerFrame->dotYmm*height_/scene::ScreenHeightMm);
            constants.dotRadiusPx = static_cast<float>(visualizerFrame->dotRadiusMm*height_/scene::ScreenHeightMm);
            constants.dotEnabled = 1.0f;
            constants.dotSceneNits[0] = static_cast<float>(
                hdr::SceneValueToNits(visualizerFrame->red));
            constants.dotSceneNits[1] = static_cast<float>(
                hdr::SceneValueToNits(visualizerFrame->green));
            constants.dotSceneNits[2] = static_cast<float>(
                hdr::SceneValueToNits(visualizerFrame->blue));
        }
        std::memcpy(
            mappedFrameData_ +
                static_cast<std::size_t>(frameIndex_) *
                    ConstantAlign,
            &constants,
            sizeof(constants));
    }

    void HdrRoomRenderer::EnsureTrailCapacity(const std::size_t required)
    {
        if (required <= trailCaps_[frameIndex_]) return;
        const std::size_t capacity = std::bit_ceil(required);
        if (capacity > std::numeric_limits<UINT>::max() / sizeof(TrailVertex))
            throw std::runtime_error("The presentation sweep exceeds the DX12 vertex-view size");
        const auto properties = UploadHeapProperties();
        const auto description = BufferDescription(capacity * sizeof(TrailVertex));
        ComPtr<ID3D12Resource> buffer;
        ThrowIfFailed(device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
            &description, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer)),
            "Grow presentation-sweep vertex buffer");
        D3D12_RANGE noRead{0, 0};
        TrailVertex* mapped{};
        ThrowIfFailed(buffer->Map(0, &noRead, reinterpret_cast<void**>(&mapped)),
                      "Map presentation-sweep vertex buffer");
        if (trailBuffers_[frameIndex_] && mappedTrail_[frameIndex_])
            trailBuffers_[frameIndex_]->Unmap(0, nullptr);
        trailBuffers_[frameIndex_] = std::move(buffer);
        mappedTrail_[frameIndex_] = mapped;
        trailCaps_[frameIndex_] = capacity;
    }

    void HdrRoomRenderer::UpdateTrailGeometry(
        const std::span<const visualizer::TrailPoint> trail,
        const std::span<const visualizer::TrailPoint> sweep,
        const float opacity)
    {
        trailVertexCount_ = 0;
        const auto segmentCount = [](const auto path) {
            return path.size() > 1 ? path.size() - 1 : 0;
        };
        const std::size_t required = (segmentCount(trail) + segmentCount(sweep)) * CapsuleVertexCount;
        if (required == 0) return;
        EnsureTrailCapacity(required);
        auto* destination = mappedTrail_[frameIndex_];
        trailView_.BufferLocation = trailBuffers_[frameIndex_]->GetGPUVirtualAddress();
        trailView_.SizeInBytes = static_cast<UINT>(required * sizeof(TrailVertex));
        trailView_.StrideInBytes = sizeof(TrailVertex);

        struct CurveSample
        {
            float x{}, y{}, radius{};
            float colorNits[3]{};
            float opacity{};
        };
        const auto convert = [opacity](const visualizer::TrailPoint& point) {
            return CurveSample{point.x, point.y, point.radius,
                {static_cast<float>(hdr::SceneValueToNits(point.red))*point.opacity,
                 static_cast<float>(hdr::SceneValueToNits(point.green))*point.opacity,
                 static_cast<float>(hdr::SceneValueToNits(point.blue))*point.opacity}, point.opacity*opacity};
        };
        const auto setVertex = [](
            TrailVertex& vertex, const float cornerX, const float cornerY,
            const CurveSample& start, const CurveSample& end)
        {
            vertex.corner[0] = cornerX;
            vertex.corner[1] = cornerY;
            vertex.segmentStart[0] = start.x;
            vertex.segmentStart[1] = start.y;
            vertex.segmentEnd[0] = end.x;
            vertex.segmentEnd[1] = end.y;
            vertex.radii[0] = start.radius;
            vertex.radii[1] = end.radius;
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                vertex.startColorNits[channel] = start.colorNits[channel];
                vertex.endColorNits[channel] = end.colorNits[channel];
            }
            vertex.startOpacity = start.opacity;
            vertex.endOpacity = end.opacity;
        };
        const std::array paths{trail, sweep};
        for (const auto path : paths)
        {
            for (std::size_t index = 0; index + 1 < path.size(); ++index)
            {
                const auto start = convert(path[index]);
                const auto end = convert(path[index + 1]);
                const float deltaX = end.x - start.x;
                const float deltaY = end.y - start.y;
                const float length = std::hypot(deltaX, deltaY);
                const float directionX = length > 0.0f ? deltaX / length : 1.0f;
                const float directionY = length > 0.0f ? deltaY / length : 0.0f;
                const float normalX = -directionY;
                const float normalY = directionX;
                const float extent = std::max(start.radius, end.radius) + 1.0f;
                const float startX = start.x - directionX * extent;
                const float startY = start.y - directionY * extent;
                const float endX = end.x + directionX * extent;
                const float endY = end.y + directionY * extent;
                const float corners[4][2]{
                    {startX + normalX * extent, startY + normalY * extent},
                    {startX - normalX * extent, startY - normalY * extent},
                    {endX + normalX * extent, endY + normalY * extent},
                    {endX - normalX * extent, endY - normalY * extent}};
                constexpr std::size_t order[CapsuleVertexCount]{0, 1, 2, 2, 1, 3};
                for (std::size_t vertex = 0; vertex < CapsuleVertexCount; ++vertex)
                {
                    const auto corner = order[vertex];
                    setVertex(destination[trailVertexCount_++],
                              corners[corner][0], corners[corner][1], start, end);
                }
            }
        }
    }

    void HdrRoomRenderer::Render(
        const visualizer::TrailFrame* visualizerFrame,
        const std::span<const visualizer::TrailPoint> trail,
        const std::span<const visualizer::TrailPoint> sweep,
        const scene::EyePose* roomEye,
        const scene::WallField& wallField,
        const stroke::View* strokeView)
    {
        RefreshDisplay();
        UpdateFrameData(visualizerFrame);
        if(!output_) { output_=std::make_unique<HdrSceneOutput>();output_->Initialize(device_.Get()); }
        output_->ResizeAfterGpu(width_,height_);

        if (roomEye)
        {
            if (!room_) { room_=std::make_unique<BakedRoomScene>(); room_->Initialize(device_.Get()); }
            room_->UpdateAfterGpu(frameIndex_,visualizerFrame,width_,height_,*roomEye,
                trail,sweep,wallField,strokeView);
            trailVertexCount_=0;
        }
        else {
            std::vector<visualizer::TrailPoint> flatTrail(trail.begin(),trail.end()),flatSweep(sweep.begin(),sweep.end());
            for(auto* path:{&flatTrail,&flatSweep}) for(auto& p:*path) {
                p.x*=static_cast<float>(width_/scene::ScreenChordMm);p.y*=static_cast<float>(height_/scene::ScreenHeightMm);
                p.radius*=static_cast<float>(height_/scene::ScreenHeightMm);
            }
            UpdateTrailGeometry(flatTrail,flatSweep,1);
        }
        ThrowIfFailed(
            frameAllocators_[frameIndex_]->Reset(),
            "Reset command allocator");
        ThrowIfFailed(
            drawList_->Reset(
                frameAllocators_[frameIndex_].Get(),
                dotPipeline_.Get()),
            "Reset command list");

        const D3D12_RESOURCE_BARRIER toRenderTarget = Transition(
            backBuffers_[frameIndex_].Get(),
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        drawList_->ResourceBarrier(1, &toRenderTarget);
        drawList_->RSSetViewports(1, &viewport_);
        drawList_->RSSetScissorRects(1, &scissorRect_);

        auto rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<std::size_t>(frameIndex_) *
            rtvStride_;
        constexpr float black[4]{0.0f, 0.0f, 0.0f, 1.0f};
        drawList_->ClearRenderTargetView(rtv, black, 0, nullptr);
        output_->Begin(drawList_.Get());
        if (roomEye) room_->PrepareLighting(drawList_.Get());
        drawList_->SetGraphicsRootSignature(root_.Get());
        drawList_->SetGraphicsRootConstantBufferView(
            0,
            frameData_->GetGPUVirtualAddress() +
                static_cast<std::uint64_t>(frameIndex_) *
                    ConstantAlign);
        drawList_->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        if (trailVertexCount_ != 0)
        {
            drawList_->SetPipelineState(trailPipeline_.Get());
            drawList_->IASetVertexBuffers(0, 1, &trailView_);
            drawList_->DrawInstanced(
                trailVertexCount_, 1, 0, 0);
        }
        if (roomEye) { room_->DrawScene(drawList_.Get()); }
        else
        {
            drawList_->SetPipelineState(dotPipeline_.Get());
            drawList_->DrawInstanced(3, 1, 0, 0);
        }

        const auto* outputConstants=reinterpret_cast<const FrameData*>(mappedFrameData_+frameIndex_*ConstantAlign);
        output_->Resolve(drawList_.Get(),rtv,frameIndex_,
            outputConstants->targetBlackNits,
            outputConstants->targetPeakNits);
        const D3D12_RESOURCE_BARRIER toPresent = Transition(
            backBuffers_[frameIndex_].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT);
        drawList_->ResourceBarrier(1, &toPresent);
        ThrowIfFailed(drawList_->Close(), "Close command list");
        ID3D12CommandList* lists[]{drawList_.Get()};
        drawQueue_->ExecuteCommandLists(1, lists);
        ThrowIfFailed(swapChain_->Present(syncInterval_, presentFlags_), "Present");
        AcquireFrameSlot();
    }

    void HdrRoomRenderer::AcquireFrameSlot()
    {
        const std::uint64_t signalValue = ++fenceValue_;
        ThrowIfFailed(
            drawQueue_->Signal(fence_.Get(), signalValue),
            "Signal frame fence");
        frameFences_[frameIndex_] = signalValue;
        frameIndex_ = swapChain_->GetCurrentBackBufferIndex();

        const std::uint64_t requiredValue =
            frameFences_[frameIndex_];
        if (requiredValue != 0 &&
            fence_->GetCompletedValue() < requiredValue)
        {
            ThrowIfFailed(
                fence_->SetEventOnCompletion(
                    requiredValue, fenceEvent_),
                "Set frame fence event");
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
    }

    void HdrRoomRenderer::WaitForGpu()
    {
        if (!drawQueue_ || !fence_ || !fenceEvent_)
        {
            return;
        }
        const std::uint64_t signalValue = ++fenceValue_;
        ThrowIfFailed(
            drawQueue_->Signal(fence_.Get(), signalValue),
            "Signal GPU fence");
        ThrowIfFailed(
            fence_->SetEventOnCompletion(signalValue, fenceEvent_),
            "Set GPU fence event");
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
}
