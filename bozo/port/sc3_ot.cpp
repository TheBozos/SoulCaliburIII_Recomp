// The frame's draw order with no GS: a native mirror of what the game links for a frame. The GIF channel plays the GIF
// list, then the ordering table's buckets in index order (newest first within a bucket), with an earlier view's layer of
// the table entered from inside the list. The VIF1 channel plays the VIF1 list. The two lists stop at each other's sync
// points (0x10F9D0), so a segment of the GIF list and its VIF1 counterpart are drawn together, the uploads and 2D of the
// GIF side first. No chain is built or read: the rewritten builders and hooks append here what the game would have linked.
#include "sc3_draw.h"
#include "sc3_meshes.h"
#include "sc3_textures.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace sc3::ot
{
    namespace
    {
        constexpr uint32_t kRenderer = 0x005BE700u; // +0 frame not built, +0x38 frame being built, +0x50 the layer's table, +0x54 the table, +0x62 layers, +0x64 buckets a layer, +0x80 the frame the kick sends
        constexpr uint32_t kSyncPoint = 0x0010F9D0u;
        constexpr uint32_t kSetLayer = 0x0010D718u;
        constexpr uint32_t kBuckets = 0x2000u;

        uint32_t word(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, rdram + address, 4);
            return v;
        }

        struct ListEntry
        {
            Draw draw;
            int32_t layerFirst = -1, layerEnd = -1; // a layer of the table entered from the list
        };

        struct List
        {
            std::vector<std::vector<ListEntry>> segments;
            size_t current = 0;
            void add(ListEntry &&entry)
            {
                if (segments.size() <= current)
                    segments.resize(current + 1u);
                segments[current].push_back(std::move(entry));
            }
            void reset()
            {
                for (std::vector<ListEntry> &segment : segments)
                    segment.clear();
                current = 0;
            }
        };

        struct FrameMirror
        {
            List gif, vif;
            std::vector<std::vector<Draw>> buckets = std::vector<std::vector<Draw>>(kBuckets);
            std::vector<uint32_t> usedBuckets;
            uint64_t draws = 0;
        };

        struct State
        {
            FrameMirror frames[2];
            bool ready = false;
            uint64_t kicks = 0, drawsSinceReport = 0, batchesSinceReport = 0, emptyKicks = 0;
        } g_state;

        FrameMirror &building(const uint8_t *rdram) { return g_state.frames[word(rdram, kRenderer + 0x38u) & 1u]; }

        bozo::render::Screen screen()
        {
            bozo::render::Screen s;
            s.originX = 2048.0f - 320.0f;
            s.originY = 2048.0f - 224.0f;
            s.width = 640.0f;
            s.height = 448.0f;
            return s;
        }

        void syncPoint(uint8_t *rdram, R5900Context *ctx, Runtime *runtime)
        {
            FrameMirror &frame = building(rdram);
            const uint32_t channel = getRegU32(ctx, 5);
            if (channel == 1u)
                ++frame.gif.current;
            else if (channel == 2u)
                ++frame.vif.current;
            func_0x10f9d0(rdram, ctx, runtime);
        }

        // 0x10D718 (n > 0, &head, &tail): the table's layer before n, entered from the list by a NEXT the caller appends.
        void setLayer(uint8_t *rdram, R5900Context *ctx, Runtime *runtime)
        {
            const int32_t n = getRegS32(ctx, 4);
            if (n > 0 && getRegU32(ctx, 5) != 0u && getRegU32(ctx, 6) != 0u)
            {
                const int32_t layers = static_cast<int16_t>(word(rdram, kRenderer + 0x60u) >> 16);
                const int32_t each = static_cast<int32_t>(word(rdram, kRenderer + 0x64u));
                const int32_t place = n < layers ? n : layers - 1;
                ListEntry entry;
                entry.layerFirst = (place - 1) * each;
                entry.layerEnd = place * each;
                building(rdram).gif.add(std::move(entry));
            }
            func_0x10d718(rdram, ctx, runtime);
        }

        void drawBuckets(FrameMirror &frame, bozo::render::Renderer &renderer, uint32_t first, uint32_t end)
        {
            for (uint32_t b = first; b < end && b < kBuckets; ++b)
            {
                std::vector<Draw> &bucket = frame.buckets[b];
                for (size_t i = bucket.size(); i-- > 0;)
                {
                    bucket[i](renderer);
                    ++frame.draws;
                }
                bucket.clear();
            }
        }

        void drawList(FrameMirror &frame, bozo::render::Renderer &renderer, std::vector<ListEntry> &segment)
        {
            for (ListEntry &entry : segment)
                if (entry.draw)
                {
                    entry.draw(renderer);
                    ++frame.draws;
                }
                else
                    drawBuckets(frame, renderer, static_cast<uint32_t>(entry.layerFirst), static_cast<uint32_t>(entry.layerEnd));
        }

        void walk(FrameMirror &frame, bozo::render::Renderer &renderer)
        {
            const size_t segments = std::max(frame.gif.segments.size(), frame.vif.segments.size());
            for (size_t s = 0; s < segments; ++s)
            {
                if (s < frame.gif.segments.size())
                    drawList(frame, renderer, frame.gif.segments[s]);
                if (s < frame.vif.segments.size())
                    drawList(frame, renderer, frame.vif.segments[s]);
            }
            drawBuckets(frame, renderer, 0u, kBuckets);
            frame.gif.reset();
            frame.vif.reset();
        }
    }

    bool linkBucket(uint8_t *rdram, int bucket, Draw draw)
    {
        if (word(rdram, kRenderer) != 0u)
            return false;
        bucket = bucket < 0 ? 0 : bucket >= 0x1FFF ? 0x1FFE : bucket;
        building(rdram).buckets[static_cast<uint32_t>(bucket)].push_back(std::move(draw));
        return true;
    }

    bool linkLayerBucket(uint8_t *rdram, int bucket, Draw draw)
    {
        if (word(rdram, kRenderer) != 0u)
            return false;
        const int64_t base = (static_cast<int64_t>(word(rdram, kRenderer + 0x50u)) - word(rdram, kRenderer + 0x54u)) / 16;
        const int64_t place = base + bucket;
        if (bucket < 0 || place < 0 || place >= kBuckets)
            return false;
        building(rdram).buckets[static_cast<uint32_t>(place)].push_back(std::move(draw));
        return true;
    }

    bool appendGif(uint8_t *rdram, Draw draw)
    {
        if (word(rdram, kRenderer) != 0u)
            return false;
        ListEntry entry;
        entry.draw = std::move(draw);
        building(rdram).gif.add(std::move(entry));
        return true;
    }

    bool appendVif(uint8_t *rdram, Draw draw)
    {
        if (word(rdram, kRenderer) != 0u)
            return false;
        ListEntry entry;
        entry.draw = std::move(draw);
        building(rdram).vif.add(std::move(entry));
        return true;
    }

    std::shared_ptr<Draw> gifSlot(uint8_t *rdram)
    {
        auto slot = std::make_shared<Draw>();
        if (!appendGif(rdram, [slot](bozo::render::Renderer &renderer)
                       {
                           if (*slot)
                               (*slot)(renderer);
                       }))
            return nullptr;
        return slot;
    }

    void kick(uint8_t *rdram, R5900Context *, Runtime *)
    {
        State &s = g_state;
        bozo::render::Renderer &renderer = bozo::render::Renderer::instance();
        if (!s.ready)
        {
            s.ready = true;
            renderer.setEnabled(true);
            renderer.setPresentsAlone(true);
            renderer.beginFrame(screen());
        }
        FrameMirror &frame = s.frames[(word(rdram, kRenderer + 0x80u) & 0xFFFFu) & 1u];
        frame.draws = 0;
        walk(frame, renderer);
        renderer.endFrame();
        renderer.beginFrame(screen());
        twoD::frameDone();
        meshes::frameDone();
        textures::frameDone();
        s.drawsSinceReport += frame.draws;
        s.emptyKicks += frame.draws == 0u ? 1u : 0u;
        if (++s.kicks % 250u == 0u)
        {
            std::fprintf(stderr, "[sc3:ot] 250 kicks: %llu draws, %llu kicks with none\n", static_cast<unsigned long long>(s.drawsSinceReport),
                         static_cast<unsigned long long>(s.emptyKicks));
            s.drawsSinceReport = s.emptyKicks = 0;
        }
    }

    void install(Registry &runtime)
    {
        loads::install(runtime);
        offscreen::install(runtime);
        runtime.registerFunction(kSyncPoint, &syncPoint);
        runtime.registerFunction(kSetLayer, &setLayer);
    }
}
