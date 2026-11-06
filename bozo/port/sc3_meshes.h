// The mesh side's notes of what the game loads (sc3_loads.cpp) and of its frames (sc3_ot.cpp).
#pragma once

#include "sc3_port.h"

namespace sc3::meshes
{
    void frameDone();
    void noteRequest(R5900Context *ctx);
    void noteReadBefore(uint8_t *rdram, R5900Context *ctx);
    void noteReadAfter(uint8_t *rdram, R5900Context *ctx);
    void noteCopy(uint8_t *rdram, R5900Context *ctx);
}
