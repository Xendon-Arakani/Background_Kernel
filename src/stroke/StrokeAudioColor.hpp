#pragma once

#include "stroke/WallStroke.hpp"
#include "audio/AudioCapture.hpp"
#include "visualizer/AudioSpectrum.hpp"
#include "visualizer/SceneSettings.hpp"
#include <string>

namespace bk::stroke
{
    class AudioColor
    {
    public:
        void Configure()
        {
            visualizer::settings::SpectrumConfig config{};
            std::size_t palette{};
            if(visualizer::settings::ReadSpectrum(config)!=ERROR_SUCCESS ||
                visualizer::settings::ReadPaletteChoice(palette)!=ERROR_SUCCESS)
                throw std::runtime_error("Reading orb palette failed");
            spectrum_.SetPalette(config.palette);
            spectrum_.SetPaletteIndex(palette);
            spectrum_.ApplyToFrame(frame_);
            Apply();
            const auto name=visualizer::PaletteNames[palette];
            palette_.assign(name.begin(),name.end());
        }

        void Start()
        {
            Configure();
            capture_.Start();
            started_=true;
        }

        void Shutdown() noexcept
        {
            capture_.Shutdown();
            started_=false;
        }

        void Update()
        {
            if(!started_) return;
            capture_.DrainEpochPcm(pcm_,times_,edges_);
            std::size_t offset{};
            std::size_t edgeCursor{};
            auto frames=cadence_.NextFrameCount();
            while(pcm_.size()-offset>=frames)
            {
                bool crossed=false;
                while(edgeCursor<edges_.size() && edges_[edgeCursor]<offset+frames)
                {
                    ++edgeCursor;
                    crossed=true;
                }
                if(crossed) spectrum_.Reset();
                spectrum_.AnalyzeTick(std::span<const audio::RawStereoPcm>(
                    pcm_.data()+offset,frames));
                spectrum_.ApplyToFrame(frame_);
                Apply();
                offset+=frames;
                cadence_.Advance();
                frames=cadence_.NextFrameCount();
            }
            edges_.erase(edges_.begin(),edges_.begin()+
                static_cast<std::ptrdiff_t>(edgeCursor));
            if(offset)
            {
                for(auto& edge:edges_) edge-=offset;
                pcm_.erase(pcm_.begin(),pcm_.begin()+offset);
                times_.erase(times_.begin(),times_.begin()+offset);
            }
        }

        Color Current() const noexcept
        {
            return color_;
        }

        const std::wstring& Palette() const noexcept
        {
            return palette_;
        }

    private:
        void Apply() noexcept
        {
            color_={frame_.red/100,frame_.green/100,frame_.blue/100};
        }

        audio::AudioCapture capture_;
        visualizer::AudioSpectrum spectrum_;
        visualizer::TickCadence cadence_;
        visualizer::TrailFrame frame_{};
        std::vector<audio::RawStereoPcm> pcm_;
        std::vector<std::uint64_t> times_;
        std::vector<std::size_t> edges_;
        Color color_{};
        std::wstring palette_;
        bool started_{};
    };
}
