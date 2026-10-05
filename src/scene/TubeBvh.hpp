#pragma once
#include "scene/TubeGeometry.hpp"

namespace bk::scene
{
    inline constexpr std::size_t EscapeLink=0;
    inline constexpr std::size_t EmitterLink=1;
    struct TubeBvhNode
    {
        std::array<float,4> lower{},upper{};
        std::array<std::uint32_t,4> links{};
    };
    inline std::vector<TubeBvhNode> BuildTubeBvh(std::span<const LineLight> lines)
    {
        std::vector<TubeBvhNode> nodes;
        if(lines.empty()) return nodes;
        if(lines.size()>(std::numeric_limits<std::uint32_t>::max()-1)/2) throw std::runtime_error("Too many tube BVH nodes");
        std::vector<std::uint32_t> emitters(lines.size());
        std::iota(emitters.begin(),emitters.end(),0u);
        nodes.reserve(lines.size()*2-1);
        const auto coordinate=[](Vec p,int axis) { return axis==0?p.x:axis==1?p.y:p.z; };
        const auto build=[&](auto&& self,std::size_t begin,std::size_t end)->void {
            const auto index=static_cast<std::uint32_t>(nodes.size());nodes.emplace_back();
            std::array<double,3> lower,upper;
            lower.fill(std::numeric_limits<double>::infinity());
            upper.fill(-std::numeric_limits<double>::infinity());
            for(auto item=begin;item<end;++item) {
                const auto& line=lines[emitters[item]];
                for(int axis=0;axis<3;++axis) {
                    lower[axis]=std::min({
                        lower[axis],coordinate(line.startPoint,axis)-line.startRadius,
                        coordinate(line.endPoint,axis)-line.endRadius});
                    upper[axis]=std::max({
                        upper[axis],coordinate(line.startPoint,axis)+line.startRadius,
                        coordinate(line.endPoint,axis)+line.endRadius});
                }
            }
            for(int axis=0;axis<3;++axis) {
                nodes[index].lower[axis]=std::nextafter(
                    static_cast<float>(lower[axis]),
                    -std::numeric_limits<float>::infinity());
                nodes[index].upper[axis]=std::nextafter(
                    static_cast<float>(upper[axis]),
                    std::numeric_limits<float>::infinity());
            }
            if(end-begin==1)
                nodes[index].links[EmitterLink]=emitters[begin]+1;
            else {
                int axis=0;
                for(int candidate=1;candidate<3;++candidate)
                    if(upper[candidate]-lower[candidate]>
                        upper[axis]-lower[axis]) axis=candidate;
                const auto middle=begin+(end-begin)/2;
                std::nth_element(
                    emitters.begin()+begin,emitters.begin()+middle,
                    emitters.begin()+end,[&](auto left,auto right) {
                    const auto leftCenter=coordinate(
                        lines[left].startPoint+lines[left].endPoint,axis);
                    const auto rightCenter=coordinate(
                        lines[right].startPoint+lines[right].endPoint,axis);
                    return leftCenter==rightCenter
                        ? left<right:leftCenter<rightCenter;
                });
                self(self,begin,middle);self(self,middle,end);
            }
            nodes[index].links[EscapeLink]=
                static_cast<std::uint32_t>(nodes.size());
        };
        build(build,0,emitters.size());return nodes;
    }
}
