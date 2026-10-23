#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace Trainer
{
    inline void CopyUtf8(char* Output, std::size_t Capacity, std::string_view Text)
    {
        if (!Output || Capacity == 0)
            return;
        std::size_t Count = std::min(Capacity - 1, Text.size());
        while (Count < Text.size() && Count > 0 &&
            (static_cast<unsigned char>(Text[Count]) & 0xc0) == 0x80)
            --Count;
        if (Count != 0)
            std::memcpy(Output, Text.data(), Count);
        Output[Count] = '\0';
    }
}
