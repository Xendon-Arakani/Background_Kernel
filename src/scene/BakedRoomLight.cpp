#include "scene/BakedRoomLight.hpp"
#include <execution>
#include <fstream>
#include <numeric>
#include <type_traits>
#include <windows.h>

namespace bk::scene
{
    namespace
    {
        constexpr std::uint64_t AssetMagic=0x54524f50534e4152;
        constexpr std::uint64_t AssetSchema=2;
        struct AssetHead
        {
            std::uint64_t magic{},schema{},patchCount{},formCount{};
            std::uint64_t wallCount{},pointCount{},hash{};
            double reciprocityError{},maxRowSum{},energyScale{};
        };
        std::uint64_t HashBytes(
            std::uint64_t hash,const void* data,std::size_t bytes)
        {
            const auto* octets=static_cast<const unsigned char*>(data);
            for(std::size_t offset=0;offset<bytes;++offset)
            {
                hash^=octets[offset];
                hash*=1099511628211ull;
            }
            return hash;
        }
        template<class Value>
        std::uint64_t HashList(
            std::uint64_t hash,const std::vector<Value>& values)
        {
            static_assert(std::is_trivially_copyable_v<Value>);
            return HashBytes(hash,values.data(),
                values.size()*sizeof(Value));
        }
        std::uint64_t TransportHash(const BakedRoomLight& transport)
        {
            auto hash=1469598103934665603ull;
            hash=HashList(hash,transport.patches);
            hash=HashList(hash,transport.formFactors);
            hash=HashList(hash,transport.wallSamples);
            return HashList(hash,transport.pointFactors);
        }
        template<class Value>
        void WriteList(
            std::ofstream& stream,const std::vector<Value>& values)
        {
            static_assert(std::is_trivially_copyable_v<Value>);
            stream.write(reinterpret_cast<const char*>(values.data()),
                static_cast<std::streamsize>(values.size()*sizeof(Value)));
            if(!stream) throw std::runtime_error(
                "Writing room transport failed");
        }
        template<class Value>
        void ReadList(
            std::ifstream& stream,std::vector<Value>& values,
            std::size_t count)
        {
            static_assert(std::is_trivially_copyable_v<Value>);
            values.resize(count);
            stream.read(reinterpret_cast<char*>(values.data()),
                static_cast<std::streamsize>(values.size()*sizeof(Value)));
            if(!stream) throw std::runtime_error(
                "Reading room transport failed");
        }
        std::filesystem::path AssetPath()
        {
            std::wstring path(32768,L'\0');
            const DWORD length=GetModuleFileNameW(
                nullptr,path.data(),static_cast<DWORD>(path.size()));
            if(!length || length>=path.size())
                throw std::runtime_error(
                    "Finding room transport failed");
            path.resize(length);
            auto result=std::filesystem::path(path);
            result.replace_filename(L"BakedRoomLight.bin");
            return result;
        }
        BakedRoomLight LoadTransport()
        {
            std::ifstream stream(AssetPath(),std::ios::binary);
            if(!stream) throw std::runtime_error(
                "Opening room transport failed");
            AssetHead head;
            stream.read(reinterpret_cast<char*>(&head),sizeof(head));
            const std::uint64_t patchCount=PatchCount;
            const std::uint64_t formCount=patchCount*patchCount;
            const std::uint64_t wallCount=LightingCount;
            const std::uint64_t pointCount=wallCount*patchCount;
            if(!stream || head.magic!=AssetMagic ||
                head.schema!=AssetSchema ||
                head.patchCount!=patchCount ||
                head.formCount!=formCount ||
                head.wallCount!=wallCount ||
                head.pointCount!=pointCount)
                throw std::runtime_error(
                    "Room transport does not match this build");
            BakedRoomLight result;
            result.reciprocityError=head.reciprocityError;
            result.maxRowSum=head.maxRowSum;
            result.energyScale=head.energyScale;
            ReadList(stream,result.patches,
                static_cast<std::size_t>(head.patchCount));
            ReadList(stream,result.formFactors,
                static_cast<std::size_t>(head.formCount));
            ReadList(stream,result.wallSamples,
                static_cast<std::size_t>(head.wallCount));
            ReadList(stream,result.pointFactors,
                static_cast<std::size_t>(head.pointCount));
            if(stream.peek()!=std::ifstream::traits_type::eof() ||
                TransportHash(result)!=head.hash)
                throw std::runtime_error(
                    "Room transport is damaged");
            return result;
        }
        double PolygonFactor(Vec origin, Vec normal, const Patch& source)
        {
            const std::array<Vec,4> corners{source.center-source.u-source.v,
                source.center+source.u-source.v,source.center+source.u+source.v,source.center-source.u+source.v};
            double integral=0;
            for (int corner=0;corner<4;++corner)
            {
                const auto firstRay=Unit(corners[corner]-origin);
                const auto nextRay=Unit(corners[(corner+1)%4]-origin);
                const auto edgeCross=Cross(firstRay,nextRay);
                const double crossLength=Length(edgeCross);
                if (crossLength>0)
                    integral+=Dot(normal,edgeCross)*
                        (std::atan2(crossLength,Dot(firstRay,nextRay))/
                            crossLength);
            }
            return std::abs(integral)/(2*Pi);
        }
        double AverageFactor(const Patch& receiver,const Patch& source)
        {
            constexpr double gauss=0.57735026918962576451;
            double result=0;
            for (double alongU:{-gauss,gauss})
                for(double alongV:{-gauss,gauss})
                    result+=PolygonFactor(
                        receiver.center+receiver.u*alongU+
                            receiver.v*alongV,
                        receiver.normal,source);
            return result/4;
        }
    }
    BakedRoomLight GenerateTransport()
    {
        BakedRoomLight result;
        const double halfWidth=ScreenHalfChordMm();
        const double halfHeight=ScreenHeightMm/2;
        const double front=FrontEdgeZmm();
        const double depth=BackWallMm-front;
        const std::array<Vec,5> centers{{
            {0,0,BackWallMm},{-halfWidth,0,front+depth/2},
            {halfWidth,0,front+depth/2},{0,halfHeight,front+depth/2},
            {0,-halfHeight,front+depth/2}}};
        const std::array<Vec,5> normals{{{0,0,-1},{1,0,0},{-1,0,0},{0,-1,0},{0,1,0}}};
        const std::array<Vec,5> faceU{{
            {halfWidth,0,0},{0,0,depth/2},{0,0,depth/2},
            {halfWidth,0,0},{halfWidth,0,0}}};
        const std::array<Vec,5> faceV{{
            {0,halfHeight,0},{0,halfHeight,0},{0,halfHeight,0},
            {0,0,depth/2},{0,0,depth/2}}};
        for(std::uint32_t face=0;face<FaceCount;++face)
            for(std::uint32_t row=0;row<PatchSide;++row)
            for(std::uint32_t column=0;column<PatchSide;++column)
            {
                Patch patch;
                patch.normal=normals[face];
                patch.u=faceU[face]*(1.0/PatchSide);
                patch.v=faceV[face]*(1.0/PatchSide);
                patch.center=centers[face]+
                    faceU[face]*(2*(column+0.5)/PatchSide-1)+
                    faceV[face]*(2*(row+0.5)/PatchSide-1);
                patch.area=4*Length(Cross(patch.u,patch.v));
                result.patches.push_back(patch);
            }
        const std::size_t patchCount=result.patches.size();
        result.formFactors.resize(patchCount*patchCount);
        for(std::size_t receiver=0;receiver<patchCount;++receiver)
        for(std::size_t source=receiver+1;source<patchCount;++source)
        {
            if(receiver/(PatchSide*PatchSide)==
                source/(PatchSide*PatchSide)) continue;
            const auto& receiverPatch=result.patches[receiver];
            const auto& sourcePatch=result.patches[source];
            const double exchange=(
                receiverPatch.area*AverageFactor(receiverPatch,sourcePatch)+
                sourcePatch.area*AverageFactor(sourcePatch,receiverPatch))/2;
            result.formFactors[receiver*patchCount+source]=
                exchange/receiverPatch.area;
            result.formFactors[source*patchCount+receiver]=
                exchange/sourcePatch.area;
        }
        std::vector<double> rowScale(patchCount,1);
        for(std::size_t receiver=0;receiver<patchCount;++receiver)
        {
            const double rowSum=std::accumulate(
                result.formFactors.begin()+receiver*patchCount,
                result.formFactors.begin()+(receiver+1)*patchCount,0.0);
            const Patch opening{{0,0,front},{0,0,1},
                {halfWidth,0,0},{0,halfHeight,0},
                4*halfWidth*halfHeight};
            const double available=
                1-AverageFactor(result.patches[receiver],opening);
            if(rowSum>available) rowScale[receiver]=available/rowSum;
        }
        for(std::size_t receiver=0;receiver<patchCount;++receiver)
        for(std::size_t source=0;source<patchCount;++source)
        {
            const double factor=std::min(
                rowScale[receiver],rowScale[source]);
            result.energyScale=std::min(result.energyScale,factor);
            result.formFactors[receiver*patchCount+source]*=factor;
        }
        for(std::size_t receiver=0;receiver<patchCount;++receiver)
        {
            result.maxRowSum=std::max(result.maxRowSum,std::accumulate(
                result.formFactors.begin()+receiver*patchCount,
                result.formFactors.begin()+(receiver+1)*patchCount,0.0));
            for(std::size_t source=0;source<patchCount;++source)
                result.reciprocityError=std::max(
                    result.reciprocityError,std::abs(
                        result.patches[receiver].area*
                            result.formFactors[receiver*patchCount+source]-
                        result.patches[source].area*
                            result.formFactors[source*patchCount+receiver]));
        }
        result.wallSamples.resize(LightingCount);
        result.pointFactors.resize(
            static_cast<std::size_t>(LightingCount)*patchCount);
        std::vector<std::uint32_t> receivers(LightingCount);
        std::iota(receivers.begin(),receivers.end(),0);
        std::for_each(std::execution::par,receivers.begin(),receivers.end(),
        [&](std::uint32_t sampleIndex) {
            const auto face=sampleIndex/(LightingSide*LightingSide);
            const auto pixel=sampleIndex%(LightingSide*LightingSide);
            const auto column=pixel%LightingSide;
            const auto row=pixel/LightingSide;
            auto& receiver=result.wallSamples[sampleIndex];
            receiver.normal=normals[face];
            receiver.center=centers[face]+
                faceU[face]*(2*(column+0.5)/LightingSide-1)+
                faceV[face]*(2*(row+0.5)/LightingSide-1);
            float* gatherRow=result.pointFactors.data()+
                static_cast<std::size_t>(sampleIndex)*patchCount;
            double sum=0;
            for(std::size_t source=0;source<patchCount;++source) {
                const double factor=source/(PatchSide*PatchSide)==face
                    ? 0:PolygonFactor(receiver.center,receiver.normal,
                        result.patches[source]);
                gatherRow[source]=static_cast<float>(factor);sum+=factor;
            }
            const Patch opening{{0,0,front},{0,0,1},
                {halfWidth,0,0},{0,halfHeight,0},
                4*halfWidth*halfHeight};
            const double available=std::max(0.0,
                1-PolygonFactor(receiver.center,receiver.normal,opening));
            if(sum>available)
                for(std::size_t source=0;source<patchCount;++source)
                    gatherRow[source]*=static_cast<float>(available/sum);
        });
        return result;
    }
    void SaveTransport(
        const BakedRoomLight& transport,
        const std::filesystem::path& path)
    {
        AssetHead head;
        head.magic=AssetMagic;
        head.schema=AssetSchema;
        head.patchCount=transport.patches.size();
        head.formCount=transport.formFactors.size();
        head.wallCount=transport.wallSamples.size();
        head.pointCount=transport.pointFactors.size();
        head.hash=TransportHash(transport);
        head.reciprocityError=transport.reciprocityError;
        head.maxRowSum=transport.maxRowSum;
        head.energyScale=transport.energyScale;
        auto pending=path;
        pending+=L".part";
        {
            std::ofstream stream(
                pending,std::ios::binary|std::ios::trunc);
            if(!stream) throw std::runtime_error(
                "Creating room transport failed");
            stream.write(reinterpret_cast<const char*>(&head),sizeof(head));
            WriteList(stream,transport.patches);
            WriteList(stream,transport.formFactors);
            WriteList(stream,transport.wallSamples);
            WriteList(stream,transport.pointFactors);
            stream.flush();
            if(!stream) throw std::runtime_error(
                "Flushing room transport failed");
        }
        if(!MoveFileExW(pending.c_str(),path.c_str(),
            MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error(
                "Installing room transport failed");
    }
    const BakedRoomLight& BuildTransport()
    {
        static const BakedRoomLight transport=LoadTransport();return transport;
    }
}
