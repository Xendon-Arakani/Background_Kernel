#pragma once

#include "visualizer/AudioTrailMotion.hpp"
#include "visualizer/RainbowPalette.hpp"
#include "visualizer/SpectralMapping.hpp"
#include "visualizer/PathPursuit.hpp"
#include "visualizer/MoveSizeMap.hpp"
#include "scene/RoomGeometry.hpp"
#include "render/HdrToneMapper.hpp"
#include "render/PresentationSync.hpp"
#include <windows.h>

namespace bk::visualizer::settings
{
    inline constexpr wchar_t SettingsPath[] = L"Software\\BackgroundKernel\\Visualizer";
    inline constexpr wchar_t EyeDistanceKey[] = L"EyeDistanceMm";
    inline constexpr wchar_t CompatEyeInchesKey[] = L"EyeDistanceInches";
    inline constexpr wchar_t SettingsChanged[] = L"Local\\BkSetChanged";
    inline constexpr wchar_t SettingsApplied[] = L"Local\\BkSetApplied";
    inline constexpr wchar_t PanelOpenedEvent[] = L"Local\\BkPanelOpen";
    inline constexpr wchar_t PaperWhiteKey[] = L"PaperWhiteNits";
    inline constexpr wchar_t VerticalRangeKey[] = L"VerticalDbfsRangeV1";
    inline constexpr wchar_t TopDecayKey[] = L"TopDecay";
    inline constexpr wchar_t SyncModeKey[] = L"PresentationSyncV1";
    inline constexpr wchar_t SyncChangedEvent[] = L"Local\\BkSyncChanged";
    inline constexpr wchar_t PaletteChoiceKey[] = L"ColorPaletteSelectionV1";

    inline constexpr wchar_t SpectrumConfigKey[] = L"SpectralControlsV1";
    inline constexpr wchar_t EntropyPursuitKey[]=L"EntropyXySmoothingV1";
    inline constexpr wchar_t VolumeCurveKey[]=L"PositionHalfLifeExponent";
    inline constexpr wchar_t VolumeZeroKey[]=L"PositionHalfLifeZeroLevel";
    inline constexpr wchar_t CompatSizeCurveKey[]=L"DisplacementSizeCurveExponentV1";
    inline constexpr wchar_t CompatSizeMapKey[]=L"DisplacementSizeControlsV2";
    inline constexpr wchar_t MoveSizeMapKey[]=L"DisplacementSizeControlsV3";
    inline LSTATUS ReadPaletteChoice(std::size_t& paletteIndex,
        const wchar_t* key=SettingsPath) {
        DWORD value{},bytes=sizeof(value);
        const auto status=RegGetValueW(HKEY_CURRENT_USER,key,PaletteChoiceKey,
            RRF_RT_REG_DWORD,nullptr,&value,&bytes);
        if(status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND) {
            paletteIndex=0;return ERROR_SUCCESS;
        }
        if(status!=ERROR_SUCCESS) return status;
        if(bytes!=sizeof(value) || value>=PaletteCount) return ERROR_INVALID_DATA;
        paletteIndex=static_cast<std::size_t>(value);return ERROR_SUCCESS;
    }
    inline LSTATUS WritePaletteChoice(const std::size_t paletteIndex,
        const wchar_t* key=SettingsPath) {
        if(paletteIndex>=PaletteCount) return ERROR_INVALID_DATA;
        HKEY opened{};
        const auto status=RegCreateKeyExW(HKEY_CURRENT_USER,key,0,nullptr,
            REG_OPTION_NON_VOLATILE,KEY_SET_VALUE,nullptr,&opened,nullptr);
        if(status!=ERROR_SUCCESS) return status;
        const DWORD value=static_cast<DWORD>(paletteIndex);
        const auto result=RegSetValueExW(opened,PaletteChoiceKey,0,
            REG_DWORD,reinterpret_cast<const BYTE*>(&value),sizeof(value));
        RegCloseKey(opened);return result;
    }
    inline LSTATUS ReadSyncMode(render::PresentationSync& mode,const wchar_t* key=SettingsPath) {
        DWORD value{},bytes=sizeof(value);
        const auto status=RegGetValueW(HKEY_CURRENT_USER,key,SyncModeKey,RRF_RT_REG_DWORD,nullptr,&value,&bytes);
        if(status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND) { mode=render::PresentationSync::Immediate;return ERROR_SUCCESS; }
        if(status!=ERROR_SUCCESS) return status;
        const auto candidate=static_cast<render::PresentationSync>(value);
        if(bytes!=sizeof(value) || !render::ValidSyncMode(candidate)) return ERROR_INVALID_DATA;
        mode=candidate;return ERROR_SUCCESS;
    }
    inline LSTATUS WriteSyncMode(const render::PresentationSync mode,const wchar_t* key=SettingsPath) {
        if(!render::ValidSyncMode(mode)) return ERROR_INVALID_DATA;
        HKEY opened{};
        const auto status=RegCreateKeyExW(HKEY_CURRENT_USER,key,0,nullptr,REG_OPTION_NON_VOLATILE,KEY_SET_VALUE,nullptr,&opened,nullptr);
        if(status!=ERROR_SUCCESS) return status;
        const DWORD value=static_cast<DWORD>(mode);
        const auto result=RegSetValueExW(opened,SyncModeKey,0,REG_DWORD,reinterpret_cast<const BYTE*>(&value),sizeof(value));
        RegCloseKey(opened);return result;
    }
    static_assert(sizeof(VerticalDbfsRange)==2*sizeof(double));
    inline LSTATUS ReadVerticalRange(VerticalDbfsRange& range,const wchar_t* key=SettingsPath) {
        VerticalDbfsRange value{};DWORD bytes=sizeof(value);
        const auto status=RegGetValueW(HKEY_CURRENT_USER,key,VerticalRangeKey,RRF_RT_REG_BINARY,nullptr,&value,&bytes);
        if(status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND) { range={};return ERROR_SUCCESS; }
        if(status!=ERROR_SUCCESS) return status;
        if(bytes!=sizeof(value) || !ValidVerticalRange(value)) return ERROR_INVALID_DATA;
        range=value;return ERROR_SUCCESS;
    }
    inline LSTATUS WriteVerticalRange(const VerticalDbfsRange& range,const wchar_t* key=SettingsPath) {
        if(!ValidVerticalRange(range)) return ERROR_INVALID_DATA;
        HKEY opened{};
        const auto status=RegCreateKeyExW(HKEY_CURRENT_USER,key,0,nullptr,REG_OPTION_NON_VOLATILE,KEY_SET_VALUE,nullptr,&opened,nullptr);
        if(status!=ERROR_SUCCESS) return status;
        const auto result=RegSetValueExW(opened,VerticalRangeKey,0,REG_BINARY,reinterpret_cast<const BYTE*>(&range),sizeof(range));
        RegCloseKey(opened);return result;
    }
    static_assert(sizeof(TopDecay)==2*sizeof(double));
    inline LSTATUS ReadTopDecay(TopDecay& decay,
        const wchar_t* key=SettingsPath) {
        TopDecay value{};DWORD bytes=sizeof(value);
        const auto status=RegGetValueW(HKEY_CURRENT_USER,key,TopDecayKey,
            RRF_RT_REG_BINARY,nullptr,&value,&bytes);
        if(status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND) {
            decay={};return ERROR_SUCCESS;
        }
        if(status!=ERROR_SUCCESS) return status;
        if(bytes!=sizeof(value) || !ValidTopDecay(value))
            return ERROR_INVALID_DATA;
        decay=value;return ERROR_SUCCESS;
    }
    inline LSTATUS WriteTopDecay(const TopDecay& decay,
        const wchar_t* key=SettingsPath) {
        if(!ValidTopDecay(decay)) return ERROR_INVALID_DATA;
        HKEY opened{};
        const auto status=RegCreateKeyExW(HKEY_CURRENT_USER,key,0,nullptr,
            REG_OPTION_NON_VOLATILE,KEY_SET_VALUE,nullptr,&opened,nullptr);
        if(status!=ERROR_SUCCESS) return status;
        const auto result=RegSetValueExW(opened,TopDecayKey,0,REG_BINARY,
            reinterpret_cast<const BYTE*>(&decay),sizeof(decay));
        RegCloseKey(opened);return result;
    }
    inline LSTATUS ReadPaperWhite(double& nits,const wchar_t* key=SettingsPath) {
        double value{};DWORD bytes=sizeof(value);
        const auto status=RegGetValueW(HKEY_CURRENT_USER,key,PaperWhiteKey,RRF_RT_REG_BINARY,nullptr,&value,&bytes);
        if(status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND) { nits=render::hdr::BasePaperWhite;return ERROR_SUCCESS; }
        if(status!=ERROR_SUCCESS) return status;
        if(bytes!=sizeof(value) || !render::hdr::ValidPaperWhiteNits(value)) return ERROR_INVALID_DATA;
        nits=value;return ERROR_SUCCESS;
    }
    inline LSTATUS WritePaperWhite(double nits,const wchar_t* key=SettingsPath) {
        if(!render::hdr::ValidPaperWhiteNits(nits)) return ERROR_INVALID_DATA;
        HKEY opened{};
        const auto status=RegCreateKeyExW(HKEY_CURRENT_USER,key,0,nullptr,REG_OPTION_NON_VOLATILE,KEY_SET_VALUE,nullptr,&opened,nullptr);
        if(status!=ERROR_SUCCESS) return status;
        const auto result=RegSetValueExW(opened,PaperWhiteKey,0,REG_BINARY,reinterpret_cast<const BYTE*>(&nits),sizeof(nits));
        RegCloseKey(opened);return result;
    }
    static_assert(sizeof(EntropyPursuit)==3*sizeof(double));
    inline LSTATUS ReadEntropyPursuit(EntropyPursuit& pursuit,const wchar_t* key=SettingsPath) {
        EntropyPursuit value{};DWORD bytes=sizeof(value);
        const auto status=RegGetValueW(HKEY_CURRENT_USER,key,EntropyPursuitKey,RRF_RT_REG_BINARY,nullptr,&value,&bytes);
        if(status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND) { pursuit={};return ERROR_SUCCESS; }
        if(status!=ERROR_SUCCESS) return status;
        if(bytes!=sizeof(value) || !ValidEntropyPursuit(value)) return ERROR_INVALID_DATA;
        pursuit=value;return ERROR_SUCCESS;
    }
    inline LSTATUS WriteEntropyPursuit(const EntropyPursuit& pursuit,const wchar_t* key=SettingsPath) {
        if(!ValidEntropyPursuit(pursuit)) return ERROR_INVALID_DATA;
        HKEY opened{};
        const auto status=RegCreateKeyExW(HKEY_CURRENT_USER,key,0,nullptr,REG_OPTION_NON_VOLATILE,KEY_SET_VALUE,nullptr,&opened,nullptr);
        if(status!=ERROR_SUCCESS) return status;
        const auto result=RegSetValueExW(opened,EntropyPursuitKey,0,REG_BINARY,reinterpret_cast<const BYTE*>(&pursuit),sizeof(pursuit));
        RegCloseKey(opened);return result;
    }
    inline LSTATUS ReadVolumePursuit(VolumePursuit& pursuit,
        const wchar_t* key=SettingsPath) {
        VolumePursuit value{};
        DWORD bytes=sizeof(value.curveExponent);
        auto status=RegGetValueW(HKEY_CURRENT_USER,key,VolumeCurveKey,
            RRF_RT_REG_BINARY,nullptr,&value.curveExponent,&bytes);
        if(status!=ERROR_SUCCESS && status!=ERROR_FILE_NOT_FOUND &&
           status!=ERROR_PATH_NOT_FOUND) return status;
        if(status==ERROR_SUCCESS &&
           (bytes!=sizeof(value.curveExponent) ||
            !std::isfinite(value.curveExponent) || value.curveExponent<=0.0))
            return ERROR_INVALID_DATA;
        bytes=sizeof(value.zeroHalfLifeLevel);
        status=RegGetValueW(HKEY_CURRENT_USER,key,VolumeZeroKey,
            RRF_RT_REG_BINARY,nullptr,&value.zeroHalfLifeLevel,&bytes);
        if(status!=ERROR_SUCCESS && status!=ERROR_FILE_NOT_FOUND &&
           status!=ERROR_PATH_NOT_FOUND) return status;
        if(status==ERROR_SUCCESS &&
           (bytes!=sizeof(value.zeroHalfLifeLevel) ||
            !std::isfinite(value.zeroHalfLifeLevel) || value.zeroHalfLifeLevel<=0.0))
            return ERROR_INVALID_DATA;
        pursuit=value;return ERROR_SUCCESS;
    }
    inline LSTATUS WriteVolumePursuit(const VolumePursuit& pursuit,
        const wchar_t* key=SettingsPath) {
        if(!ValidVolumePursuit(pursuit)) return ERROR_INVALID_DATA;
        HKEY opened{};
        const auto status=RegCreateKeyExW(HKEY_CURRENT_USER,key,0,nullptr,
            REG_OPTION_NON_VOLATILE,KEY_SET_VALUE,nullptr,&opened,nullptr);
        if(status!=ERROR_SUCCESS) return status;
        auto result=RegSetValueExW(opened,VolumeCurveKey,0,REG_BINARY,
            reinterpret_cast<const BYTE*>(&pursuit.curveExponent),
            sizeof(pursuit.curveExponent));
        if(result==ERROR_SUCCESS)
            result=RegSetValueExW(opened,VolumeZeroKey,0,REG_BINARY,
                reinterpret_cast<const BYTE*>(&pursuit.zeroHalfLifeLevel),
                sizeof(pursuit.zeroHalfLifeLevel));
        RegCloseKey(opened);return result;
    }
    struct CompatSizeRecord {
        double maxDiameterPx;
        double curveExponent;
    };
    struct SavedSizeRecord {
        double maxDiameterPx;
        double curveExponent;
        std::uint32_t inverted;
        std::uint32_t reserved;
    };
    static_assert(sizeof(CompatSizeRecord)==2*sizeof(double));
    static_assert(sizeof(SavedSizeRecord)==24);
    inline LSTATUS ReadMoveSizeMap(MoveSizeMap& controls,
        const wchar_t* key=SettingsPath) {
        SavedSizeRecord record{};DWORD bytes=sizeof(record);
        auto status=RegGetValueW(HKEY_CURRENT_USER,key,MoveSizeMapKey,
            RRF_RT_REG_BINARY,nullptr,&record,&bytes);
        if(status==ERROR_SUCCESS) {
            const MoveSizeMap value{
                record.maxDiameterPx,record.curveExponent,record.inverted!=0};
            if(bytes!=sizeof(record) || record.inverted>1 || record.reserved!=0 ||
               !ValidMoveSizeMap(value)) return ERROR_INVALID_DATA;
            controls=value;return ERROR_SUCCESS;
        }
        if(status!=ERROR_FILE_NOT_FOUND && status!=ERROR_PATH_NOT_FOUND) return status;

        CompatSizeRecord compat{};bytes=sizeof(compat);
        status=RegGetValueW(HKEY_CURRENT_USER,key,CompatSizeMapKey,
            RRF_RT_REG_BINARY,nullptr,&compat,&bytes);
        if(status==ERROR_SUCCESS) {
            const MoveSizeMap value{
                compat.maxDiameterPx,compat.curveExponent,false};
            if(bytes!=sizeof(compat) || !ValidMoveSizeMap(value))
                return ERROR_INVALID_DATA;
            controls=value;return ERROR_SUCCESS;
        }
        if(status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND) {
            double compatCurve{};bytes=sizeof(compatCurve);
            status=RegGetValueW(HKEY_CURRENT_USER,key,CompatSizeCurveKey,
                RRF_RT_REG_BINARY,nullptr,&compatCurve,&bytes);
            if(status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND) {
                controls={};return ERROR_SUCCESS;
            }
            if(status!=ERROR_SUCCESS) return status;
            if(bytes!=sizeof(compatCurve) ||
               !ValidSizeCurve(compatCurve)) return ERROR_INVALID_DATA;
            controls={scene::BaseMaxRefPx,compatCurve};
            return ERROR_SUCCESS;
        }
        if(status!=ERROR_SUCCESS) return status;
        return ERROR_INVALID_DATA;
    }
    inline LSTATUS WriteMoveSizeMap(const MoveSizeMap& controls,
        const wchar_t* key=SettingsPath) {
        if(!ValidMoveSizeMap(controls)) return ERROR_INVALID_DATA;
        HKEY opened{};
        const auto status=RegCreateKeyExW(HKEY_CURRENT_USER,key,0,nullptr,REG_OPTION_NON_VOLATILE,KEY_SET_VALUE,nullptr,&opened,nullptr);
        if(status!=ERROR_SUCCESS) return status;
        const SavedSizeRecord record{
            controls.maxDiameterPx,controls.curveExponent,
            controls.inverted?1u:0u,0u};
        const auto result=RegSetValueExW(opened,MoveSizeMapKey,0,
            REG_BINARY,reinterpret_cast<const BYTE*>(&record),sizeof(record));
        RegCloseKey(opened);return result;
    }
    struct SpectrumConfig {
        SpectralPalette palette{};
        std::uint32_t reverseTrackZ{1};
        bool operator==(const SpectrumConfig&) const = default;
    };
    struct SpectrumRecord {
        std::array<double,3> hz;
        std::array<std::uint32_t,3> channels;
        std::uint32_t reverseTrackZ;
    };
    static_assert(sizeof(SpectrumRecord)==40);
    inline LSTATUS ReadSpectrum(SpectrumConfig& value,const wchar_t* key=SettingsPath)
    {
        SpectrumRecord record{};DWORD bytes=sizeof(record);
        const auto result=RegGetValueW(HKEY_CURRENT_USER,key,SpectrumConfigKey,RRF_RT_REG_BINARY,nullptr,&record,&bytes);
        if(result==ERROR_FILE_NOT_FOUND || result==ERROR_PATH_NOT_FOUND) { value={};return ERROR_SUCCESS; }
        if(result!=ERROR_SUCCESS) return result;
        const SpectrumConfig candidate{{record.hz,record.channels},record.reverseTrackZ};
        if(bytes!=sizeof(record) || !ValidPalette(candidate.palette) || candidate.reverseTrackZ>1) return ERROR_INVALID_DATA;
        value=candidate;return ERROR_SUCCESS;
    }
    inline LSTATUS WriteSpectrum(const SpectrumConfig& value,const wchar_t* key=SettingsPath)
    {
        if(!ValidPalette(value.palette) || value.reverseTrackZ>1) return ERROR_INVALID_DATA;
        HKEY opened{};
        const auto result=RegCreateKeyExW(HKEY_CURRENT_USER,key,0,nullptr,REG_OPTION_NON_VOLATILE,KEY_SET_VALUE,nullptr,&opened,nullptr);
        if(result!=ERROR_SUCCESS) return result;
        const SpectrumRecord record{value.palette.anchorHz,value.palette.compatChannel,value.reverseTrackZ};
        const auto status=RegSetValueExW(opened,SpectrumConfigKey,0,REG_BINARY,reinterpret_cast<const BYTE*>(&record),sizeof(record));
        RegCloseKey(opened);return status;
    }

    inline LSTATUS ReadEyeDistance(double& millimeters, const wchar_t* key = SettingsPath)
    {
        double value{}; DWORD bytes=sizeof(value);
        auto status=RegGetValueW(HKEY_CURRENT_USER,key,EyeDistanceKey,RRF_RT_REG_BINARY,nullptr,&value,&bytes);
        if(status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND)
        {
            bytes=sizeof(value);
            status=RegGetValueW(HKEY_CURRENT_USER,key,CompatEyeInchesKey,RRF_RT_REG_BINARY,nullptr,&value,&bytes);
            if(status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND) { millimeters=scene::BaseEyeMm; return ERROR_SUCCESS; }
            if(status!=ERROR_SUCCESS) return status;
            value*=scene::MillimetersPerInch;
        }
        if(status!=ERROR_SUCCESS) return status;
        if(bytes!=sizeof(value) || !scene::ValidEyeDistanceMm(value)) return ERROR_INVALID_DATA;
        millimeters=value;return ERROR_SUCCESS;
    }
    inline LSTATUS WriteEyeDistance(double millimeters,const wchar_t* key=SettingsPath)
    {
        if(!scene::ValidEyeDistanceMm(millimeters)) return ERROR_INVALID_DATA;
        HKEY opened{};
        const auto created=RegCreateKeyExW(HKEY_CURRENT_USER,key,0,nullptr,REG_OPTION_NON_VOLATILE,KEY_SET_VALUE,nullptr,&opened,nullptr);
        if(created!=ERROR_SUCCESS) return created;
        const auto status=RegSetValueExW(opened,EyeDistanceKey,0,REG_BINARY,reinterpret_cast<const BYTE*>(&millimeters),sizeof(millimeters));
        RegCloseKey(opened);return status;
    }

}
