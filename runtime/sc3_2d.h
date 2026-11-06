#pragma once

#include <cstdint>
#include <vector>

class PS2Runtime;

namespace sc3
{
    // The game's 2D builders rewritten in C++ from their MIPS, drawn natively from the game's own variables at their place
    // in the ordering table (SC3_NATIVE_2D=1, or =@<file> following the file's first line): RECOMP_NOTES "Native 2D".
    void installNative2d(PS2Runtime &runtime, bool nativeGs);

    // A small movie's current picture by its player handle (the host decoder's): RGBA rows, alpha 0x80 = 1.0, and its
    // number; rgba is left as it is while known is still that number.
    using MoviePicture = bool (*)(uint32_t handle, uint32_t known, std::vector<uint32_t> &rgba, uint32_t &width, uint32_t &height,
                                  uint32_t &number);
    void setMoviePictures(MoviePicture pictures);

    // The native 2D draws this player's pictures from the host decoder: they need not be written into the game's memory.
    bool native2dMovie(uint32_t handle);

    // GIF chain markers of the native 2D's kind (its marker gate), for other native work placed where a game packet would
    // have gone: a key (Renderer::drawGsKeyed / passKeyed) linked into the in-order GIF list (0x10F808), the current OT
    // layer's last bucket (0x10E1F8), a bucket of the current layer by depth (0x10D9A0) or a fixed bucket of the whole
    // table (0x10DDF8). False when the frame takes no packet there (or the native 2D, whose gate reads them, is off).
    enum class MarkerPlace
    {
        GifList,
        LayerEnd,
        LayerDepth,
        Bucket,
    };
    bool nativeMarkers();
    uint64_t nativeMarkerKey();
    bool linkNativeMarker(uint64_t key, MarkerPlace place, float depth = 0.0f, int bucket = 0);
    // The scratchpad's packet half switched, as each packet the game closes switches it (0x10F808 and the insertions).
    void switchPacketHalf();
}
