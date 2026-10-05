#include "scene/BakedRoomLight.hpp"

#include <iostream>

int wmain(int count,wchar_t** values)
{
    if(count!=2) return 2;
    try
    {
        const auto transport=bk::scene::GenerateTransport();
        bk::scene::SaveTransport(transport,values[1]);
        return 0;
    }
    catch(const std::exception& error)
    {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
