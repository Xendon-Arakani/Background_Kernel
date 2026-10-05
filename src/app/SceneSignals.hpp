#pragma once

namespace bk::app::surface
{
    inline constexpr wchar_t KernelOwner[] =
        L"Local\\BkKernelOwner";
    inline constexpr wchar_t KernelStop[] =
        L"Local\\BkKernelStop";
    inline constexpr wchar_t KernelReady[] =
        L"Local\\BkKernelReady";
    inline constexpr wchar_t SceneToggle[] =
        L"Local\\BkSceneToggle";
    inline constexpr wchar_t WallStrokeToggle[] =
        L"Local\\BkStrokeToggle";
    inline constexpr wchar_t WallStrokeStop[] =
        L"Local\\BkStrokeStop";
    inline constexpr wchar_t WallStrokeReady[] =
        L"Local\\BkStrokeReady";
    inline constexpr wchar_t OwnerName[] =
        L"Local\\BkSurfaceOwn";
    inline constexpr wchar_t RenderStop[] =
        L"Local\\BkRenderStop";
    inline constexpr wchar_t RenderReady[] =
        L"Local\\BkRenderReady";
    inline constexpr wchar_t VisualStop[] =
        L"Local\\BkAudioStop";
    inline constexpr wchar_t VisualReady[] =
        L"Local\\BkAudioReady";
}
