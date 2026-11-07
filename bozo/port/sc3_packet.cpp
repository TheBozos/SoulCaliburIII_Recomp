#include "sc3_port.h"

#include <cstring>
#include <stdexcept>

namespace sc3::packet
{
    void copyScratchpad(PS2Runtime *runtime, uint32_t ram, uint32_t scratchpad, uint32_t quadwords, bool toScratchpad)
    {
        for (uint32_t i = 0; i < quadwords; ++i, ram += 16u, scratchpad += 16u)
        {
            uint8_t *r = runtime->memory.hostPointer(ram, 16);
            uint8_t *s = runtime->memory.hostPointer(0x70000000u | (scratchpad & 0x3FF0u), 16);
            if (!r || !s)
                throw std::runtime_error("sc3: scratchpad copy outside memory");
            std::memcpy(toScratchpad ? s : r, toScratchpad ? r : s, 16);
        }
    }
}
