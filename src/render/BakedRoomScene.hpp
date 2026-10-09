#pragma once
#include "scene/RoomGeometry.hpp"
#include "scene/WallField.hpp"
#include "visualizer/AdaptiveTrail.hpp"
#include <d3d12.h>
#include <wrl/client.h>

namespace bk::stroke { struct View; }

namespace bk::render
{
    class BakedRoomScene
    {
    public:
        ~BakedRoomScene() { if(constants_ && mapped_) constants_->Unmap(0,nullptr); }
        void Initialize(ID3D12Device* device);
        void UpdateAfterGpu(std::uint32_t slot, const visualizer::TrailFrame* frame,
                    std::uint32_t width, std::uint32_t height, scene::EyePose eye,
                    std::span<const visualizer::TrailPoint> trail={},std::span<const visualizer::TrailPoint> sweep={},
                    const scene::WallField& wallField={},
                    const stroke::View* strokeView=nullptr);
        void PrepareLighting(ID3D12GraphicsCommandList* commands);
        void DrawScene(ID3D12GraphicsCommandList* commands);
        const scene::Light& Source() const { return physicalLight_; }
        static void ValidateShaders();
    private:
        void Bind(ID3D12GraphicsCommandList* commands,bool compute);
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> lineBudgetPipeline_,directPipeline_,bounceStepPipe_,gatherPipeline_,scenePipeline_;
        Microsoft::WRL::ComPtr<ID3D12Resource> patches_,wallReflect_,factors_,direct_,bounce_,bounceWork_,lineIrradiance_,constants_,wallSamples_,wallGather_,wallAtlas_;
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2> strokeShapes_,strokeRanges_,strokeIndices_;
        std::vector<std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2>> staticUploads_;
        bool staticUploaded_{};
        Microsoft::WRL::ComPtr<ID3D12Device> device_;
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2> lines_,lineBudgets_;
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2> tubeTrees_;
        std::array<std::size_t,2> treeCapacity_{};
        std::array<std::size_t,2> lineCapacity_{};
        std::array<std::size_t,2> strokeShapeCap_{},strokeRangeCap_{},strokeIndexCap_{};
        std::array<std::uint64_t,2> strokeVersion_{};
        std::array<bool,2> strokeUploaded_{};
        std::uint32_t lineCount_{};
        std::uint32_t treeCount_{},debugView_{};
        std::uint32_t wallPasses_{};
        std::array<float,3> wallAlbedo_{};
        double maxRowSum_{};
        std::byte* mapped_{};
        std::uint32_t slot_{},width_{},height_{};
        scene::EyePose eye_{};
        scene::Light physicalLight_{};
    };
}
