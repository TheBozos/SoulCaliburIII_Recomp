#pragma once

#include <cstdint>

class PS2Runtime;

namespace sc3
{
    void installCensus(PS2Runtime &runtime);

    bool censusTracingDraws();
    void censusNoteDraw(uint64_t tex0, uint64_t tag, uint32_t triangles);
}
