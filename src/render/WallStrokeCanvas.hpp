#pragma once

#include "stroke/WallStroke.hpp"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace bk::render
{
    class WallStrokeCanvas
    {
    public:
        ~WallStrokeCanvas();
        void Initialize(HWND window,int width,int height);
        void Resize(int width,int height);
        void RefreshDisplay();
        void Draw(std::span<const stroke::Shape> shapes,
            stroke::Point cursor,stroke::Color color,float radius,
            float opacity,bool hovering,bool drawing);
        HANDLE FrameReady() const noexcept {return frameReady_;}

    private:
        Microsoft::WRL::ComPtr<ID3D11Device> device_;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
        Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_,shapeVertex_;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_,shapePixel_;
        Microsoft::WRL::ComPtr<ID3D11Buffer> constants_,shapeData_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> scene_,depth_;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> sceneTarget_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sceneView_,shapeView_;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthView_;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> paintDepth_;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
        HWND window_{};
        HANDLE frameReady_{};
        std::size_t shapeCapacity_{};
        int width_{},height_{};
        float black_{},peak_{80};
    };
}
