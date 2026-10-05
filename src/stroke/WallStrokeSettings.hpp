#pragma once

#include "stroke/WallStroke.hpp"
#include <windows.h>
#include <charconv>

namespace bk::stroke
{
    inline constexpr wchar_t SettingsPath[]=
        L"Software\\BackgroundKernel\\WallStroke";

    struct SavedSettings
    {
        std::array<double,5> value{};
        std::uint32_t paintEnabled{};
        std::uint32_t reserved{};
    };

    struct PriorSettings
    {
        float restWidthCm{};
        float fullWidthCm{};
        float quietTrailCm{};
        float loudTrailCm{};
        float opacityPct{};
        bool paintEnabled{};
    };

    static_assert(sizeof(SavedSettings)==48);
    static_assert(sizeof(PriorSettings)==24);

    struct SavedCast
    {
        std::array<double,3> value{};
    };

    static_assert(sizeof(SavedCast)==24);

    inline double WidenFloat(const float value) noexcept
    {
        char text[32]{};
        const auto made=std::to_chars(text,text+sizeof(text),value);
        double result=static_cast<double>(value);
        if(made.ec==std::errc{})
        {
            const auto read=std::from_chars(text,made.ptr,result);
            if(read.ec!=std::errc{}) result=static_cast<double>(value);
        }
        return result;
    }

    struct CastPoint
    {
        double across{0.5},down{0.5},depth{0.5};
    };

    inline bool Valid(const CastPoint value) noexcept
    {
        return std::isfinite(value.across) && std::isfinite(value.down) &&
            std::isfinite(value.depth) && value.across>=0 && value.across<=1 &&
            value.down>=0 && value.down<=1 && value.depth>=0 && value.depth<=1;
    }

    inline Settings Load()
    {
        Settings value{};
        SavedSettings saved{};
        DWORD bytes=sizeof(saved);
        if(RegGetValueW(HKEY_CURRENT_USER,SettingsPath,L"PaintAuthoring",
            RRF_RT_REG_BINARY,nullptr,&saved,&bytes)==ERROR_SUCCESS &&
            bytes==sizeof(saved) && saved.paintEnabled<=1 && !saved.reserved)
        {
            const Settings stored{saved.value[0],saved.value[1],saved.value[2],
                saved.value[3],saved.value[4],saved.paintEnabled!=0};
            if(Valid(stored)) return stored;
        }
        PriorSettings priorValue{};
        bytes=sizeof(priorValue);
        if(RegGetValueW(HKEY_CURRENT_USER,SettingsPath,L"PaintStroke",
            RRF_RT_REG_BINARY,nullptr,&priorValue,&bytes)==ERROR_SUCCESS &&
            bytes==sizeof(priorValue))
        {
            const Settings stored{WidenFloat(priorValue.restWidthCm),
                WidenFloat(priorValue.fullWidthCm),
                WidenFloat(priorValue.quietTrailCm),
                WidenFloat(priorValue.loudTrailCm),
                WidenFloat(priorValue.opacityPct),priorValue.paintEnabled};
            if(Valid(stored)) return stored;
        }
        std::array<float,5> prior{};
        bytes=sizeof(prior);
        if(RegGetValueW(HKEY_CURRENT_USER,SettingsPath,L"OpacityStroke",
            RRF_RT_REG_BINARY,nullptr,prior.data(),&bytes)==ERROR_SUCCESS &&
            bytes==sizeof(prior))
        {
            const Settings stored{
                WidenFloat(prior[0]),WidenFloat(prior[1]),
                WidenFloat(prior[2]),WidenFloat(prior[3]),
                WidenFloat(prior[4]),true};
            if(Valid(stored)) return stored;
        }
        std::array<float,5> legacy{};
        bytes=sizeof(legacy);
        if(RegGetValueW(HKEY_CURRENT_USER,SettingsPath,L"Stroke",
            RRF_RT_REG_BINARY,nullptr,legacy.data(),&bytes)==ERROR_SUCCESS &&
            bytes==sizeof(legacy))
        {
            const Settings stored{WidenFloat(legacy[0]),WidenFloat(legacy[1]),
                WidenFloat(legacy[2]),WidenFloat(legacy[3]),WidenFloat(
                    legacy[4]*static_cast<float>(
                        100/render::hdr::SourcePeakNits)),true};
            if(Valid(stored)) value=stored;
        }
        return value;
    }

    inline LSTATUS Store(const Settings& value)
    {
        if(!Valid(value)) return ERROR_INVALID_DATA;
        HKEY key{};
        const auto opened=RegCreateKeyExW(HKEY_CURRENT_USER,SettingsPath,0,nullptr,
            REG_OPTION_NON_VOLATILE,KEY_SET_VALUE,nullptr,&key,nullptr);
        if(opened!=ERROR_SUCCESS) return opened;
        const SavedSettings saved{{value.restWidthCm,value.fullWidthCm,
            value.quietTrailCm,value.loudTrailCm,value.opacityPct},
            value.paintEnabled?1u:0u,0u};
        auto result=RegSetValueExW(key,L"PaintAuthoring",0,REG_BINARY,
            reinterpret_cast<const BYTE*>(&saved),sizeof(saved));
        if(result==ERROR_SUCCESS)
        {
            const PriorSettings priorValue{
                static_cast<float>(value.restWidthCm),
                static_cast<float>(value.fullWidthCm),
                static_cast<float>(value.quietTrailCm),
                static_cast<float>(value.loudTrailCm),
                static_cast<float>(value.opacityPct),value.paintEnabled};
            result=RegSetValueExW(key,L"PaintStroke",0,REG_BINARY,
                reinterpret_cast<const BYTE*>(&priorValue),sizeof(priorValue));
        }
        RegCloseKey(key);
        return result;
    }

    inline float LoadLabDbfs(const float baseDbfs)
    {
        float value{};
        DWORD bytes=sizeof(value);
        if(RegGetValueW(HKEY_CURRENT_USER,SettingsPath,L"LabDbfs",
            RRF_RT_REG_BINARY,nullptr,&value,&bytes)==ERROR_SUCCESS &&
            bytes==sizeof(value) && std::isfinite(value)) return value;
        return baseDbfs;
    }

    inline LSTATUS StoreLabDbfs(const float value)
    {
        if(!std::isfinite(value)) return ERROR_INVALID_DATA;
        HKEY key{};
        const auto opened=RegCreateKeyExW(HKEY_CURRENT_USER,SettingsPath,0,nullptr,
            REG_OPTION_NON_VOLATILE,KEY_SET_VALUE,nullptr,&key,nullptr);
        if(opened!=ERROR_SUCCESS) return opened;
        const auto result=RegSetValueExW(key,L"LabDbfs",0,REG_BINARY,
            reinterpret_cast<const BYTE*>(&value),sizeof(value));
        RegCloseKey(key);
        return result;
    }

    inline CastPoint LoadCast()
    {
        CastPoint value{};
        SavedCast saved{};
        DWORD bytes=sizeof(saved);
        if(RegGetValueW(HKEY_CURRENT_USER,SettingsPath,L"CastAuthoring",
            RRF_RT_REG_BINARY,nullptr,&saved,&bytes)==ERROR_SUCCESS &&
            bytes==sizeof(saved))
        {
            const CastPoint stored{
                saved.value[0],saved.value[1],saved.value[2]};
            if(Valid(stored)) return stored;
        }
        std::array<float,3> prior{};
        bytes=sizeof(prior);
        if(RegGetValueW(HKEY_CURRENT_USER,SettingsPath,L"Cast",
            RRF_RT_REG_BINARY,nullptr,prior.data(),&bytes)==ERROR_SUCCESS &&
            bytes==sizeof(prior))
        {
            const CastPoint stored{WidenFloat(prior[0]),WidenFloat(prior[1]),
                WidenFloat(prior[2])};
            if(Valid(stored)) value=stored;
        }
        return value;
    }

    inline LSTATUS StoreCast(const CastPoint& value)
    {
        if(!Valid(value)) return ERROR_INVALID_DATA;
        HKEY key{};
        const auto opened=RegCreateKeyExW(HKEY_CURRENT_USER,SettingsPath,0,nullptr,
            REG_OPTION_NON_VOLATILE,KEY_SET_VALUE,nullptr,&key,nullptr);
        if(opened!=ERROR_SUCCESS) return opened;
        const SavedCast saved{{value.across,value.down,value.depth}};
        auto result=RegSetValueExW(key,L"CastAuthoring",0,REG_BINARY,
            reinterpret_cast<const BYTE*>(&saved),sizeof(saved));
        if(result==ERROR_SUCCESS)
        {
            const std::array<float,3> prior{
                static_cast<float>(value.across),
                static_cast<float>(value.down),
                static_cast<float>(value.depth)};
            result=RegSetValueExW(key,L"Cast",0,REG_BINARY,
                reinterpret_cast<const BYTE*>(prior.data()),sizeof(prior));
        }
        RegCloseKey(key);
        return result;
    }
}
