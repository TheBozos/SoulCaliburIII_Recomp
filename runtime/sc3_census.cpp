#include "sc3_census.h"

#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sc3
{
    namespace census
    {
    namespace
    {
        constexpr uint32_t kFrameKick = 0x0010F098u;
        constexpr uint32_t kFileRequest = 0x00150418u;
        constexpr uint32_t kCdRead = 0x00424E28u;
        constexpr uint32_t kMemcpy = 0x0042D2ACu;
        constexpr uint32_t kBatchFlush = 0x00104AF0u;
        constexpr uint32_t kGifPacketClose = 0x0010F808u;
        constexpr uint32_t kOtInsertDepth = 0x0010D9A0u;
        constexpr uint32_t kGsHandler = 0x0010E658u;
        constexpr uint32_t kTextureCallReturns[] = {0x00104DCCu, 0x00104E48u};
        constexpr uint32_t kTextureOtCallReturns[] = {0x00104E34u, 0x00104E94u};
        constexpr uint32_t kSyncPoints[] = {0x0010FBB8u, 0x0010FC08u, 0x0010FC60u, 0x0010FC90u};
        constexpr uint32_t kOtInserts[] = {0x0010D9A0u, 0x0010DDF8u, 0x0010DC90u, 0x0010E1F8u, 0x0010D2F0u};
        constexpr uint32_t kSignalCounts = 0x005BE792u;

        using Function = PS2Runtime::RecompiledFunction;

        struct State
        {
            uint8_t *rdram = nullptr;
            uint8_t *scratchpad = nullptr;
            std::map<uint32_t, Function> originals;
            std::set<uint32_t> hidden;
            std::string hidePath, hideSpec;
            std::map<uint32_t, uint64_t> hiddenCalls;
            bool traceLoads = false;
            uint64_t traceFirst = ~0ull, traceCount = 0;
            bool traceOt = false;
            std::map<std::pair<uint32_t, uint32_t>, uint64_t> otCalls;
            std::atomic<uint64_t> frame{0};
            std::mutex logMutex;
            uint64_t recordFrame = ~0ull;
            std::string recordDir;
            std::map<uint32_t, Function> recordOriginals;
            FILE *records = nullptr;
            uint32_t recordSeq = 0;
        };

        constexpr uint32_t kRecordedDefault[] = {
            0x001040B0u, 0x00105278u, 0x00104AF0u, 0x00108A58u, 0x0010A178u, 0x0010A4E8u, 0x00109618u, 0x001096A0u, 0x00109728u,
            0x001097B0u, 0x00109990u, 0x00109A18u, 0x00109A60u, 0x00109A88u, 0x00109AC8u, 0x0010A100u, 0x0010B330u, 0x0010B3F0u,
            0x0010B4D8u, 0x0010D9A0u, 0x0010DC90u, 0x0010DDF8u, 0x0010E1F8u, 0x0010D2F0u, 0x0010F8B8u, 0x0010F808u, 0x00108980u,
            0x00109AF0u, 0x0010AC40u, 0x001088A0u, 0x00108850u, 0x0010F4B8u, 0x00108D40u, 0x00108F28u, 0x00108F48u, 0x00109278u,
        };

        State g_state;

        uint32_t peek32(uint32_t address)
        {
            uint32_t value = 0;
            if ((address & 0xF0000000u) == 0x70000000u)
                std::memcpy(&value, g_state.scratchpad + (address & 0x3FFCu), 4);
            else
                std::memcpy(&value, g_state.rdram + (address & PS2_RAM_MASK & ~3u), 4);
            return value;
        }

        uint8_t peek8(uint32_t address) { return g_state.rdram[address & PS2_RAM_MASK]; }

        bool tracingFrame()
        {
            const uint64_t frame = g_state.frame.load(std::memory_order_relaxed);
            return frame >= g_state.traceFirst && frame - g_state.traceFirst < g_state.traceCount;
        }

        void callOriginal(uint32_t address, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_state.originals.at(address)(rdram, ctx, runtime);
        }

        std::vector<uint32_t> parseAddresses(const std::string &text)
        {
            std::vector<uint32_t> out;
            size_t start = 0;
            while (start < text.size())
            {
                size_t end = text.find_first_of(", ", start);
                if (end == std::string::npos)
                    end = text.size();
                if (end > start)
                    out.push_back(static_cast<uint32_t>(std::strtoul(text.substr(start, end - start).c_str(), nullptr, 16)));
                start = end + 1;
            }
            return out;
        }

        void followHideFile()
        {
            State &s = g_state;
            if (s.hidePath.empty())
                return;
            std::ifstream file(s.hidePath);
            std::string spec;
            std::getline(file, spec);
            if (spec == s.hideSpec)
                return;
            s.hideSpec = spec;
            s.hidden.clear();
            for (const uint32_t address : parseAddresses(spec))
                s.hidden.insert(address);
            std::fprintf(stderr, "[sc3:hide] hiding '%s' from the next frame\n", spec.c_str());
        }

        void hideable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t address = ctx->pc;
            if (g_state.hidden.count(address) != 0u)
            {
                ++g_state.hiddenCalls[address];
                setReturnU32(ctx, 0u);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            callOriginal(address, rdram, ctx, runtime);
        }

        void report()
        {
            State &s = g_state;
            const uint64_t frame = s.frame.load(std::memory_order_relaxed);
            if (frame % 300u != 0u)
                return;
            if (!s.hiddenCalls.empty())
            {
                std::fprintf(stderr, "[sc3:hide] frame %llu: hidden calls:", static_cast<unsigned long long>(frame));
                for (const auto &[address, calls] : s.hiddenCalls)
                    std::fprintf(stderr, " %#x x%llu", address, static_cast<unsigned long long>(calls));
                std::fprintf(stderr, "\n");
                s.hiddenCalls.clear();
            }
            if (s.traceOt && !s.otCalls.empty())
            {
                std::fprintf(stderr, "[sc3:ot] frame %llu: insertions per 300 frames (function<-caller):", static_cast<unsigned long long>(frame));
                for (const auto &[key, calls] : s.otCalls)
                    std::fprintf(stderr, " %#x<-%#x x%llu", key.first, key.second, static_cast<unsigned long long>(calls));
                std::fprintf(stderr, "\n");
                s.otCalls.clear();
            }
        }

        void dumpMemory(const char *name, const uint8_t *data, size_t size)
        {
            const std::string path = g_state.recordDir + "/" + name;
            if (FILE *f = std::fopen(path.c_str(), "wb"))
            {
                std::fwrite(data, 1, size, f);
                std::fclose(f);
            }
        }

        // One record a call: "SC3R" u32, the function (bit 31: after it returned), a sequence number, the GPRs' low
        // words, the FPRs, VU0's vf registers, scratchpad 0x0000-0x0400 and 0x1b00-0x1d00.
        void writeRecord(uint32_t function, R5900Context *ctx)
        {
            State &s = g_state;
            const uint32_t head[3] = {0x52334353u, function, s.recordSeq++};
            std::fwrite(head, sizeof(head), 1, s.records);
            for (int r = 0; r < 32; ++r)
            {
                const uint32_t value = getRegU32(ctx, r);
                std::fwrite(&value, 4, 1, s.records);
            }
            std::fwrite(ctx->f, 4, 32, s.records);
            std::fwrite(ctx->vu0_vf, 16, 32, s.records);
            std::fwrite(s.scratchpad, 1, 0x400u, s.records);
            std::fwrite(s.scratchpad + 0x1B00u, 1, 0x200u, s.records);
        }

        void recorded(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t address = ctx->pc;
            State &s = g_state;
            const bool on = s.records && s.frame.load(std::memory_order_relaxed) == s.recordFrame;
            if (on)
                writeRecord(address, ctx);
            s.recordOriginals.at(address)(rdram, ctx, runtime);
            if (on && s.records)
                writeRecord(address | 0x80000000u, ctx);
        }

        void recordFrameEdge(uint64_t frame)
        {
            State &s = g_state;
            if (s.recordFrame == ~0ull)
                return;
            if (frame == s.recordFrame)
            {
                const std::string path = s.recordDir + "/parts.bin";
                s.records = std::fopen(path.c_str(), "wb");
                dumpMemory("rdram_begin.bin", s.rdram, PS2_RAM_SIZE);
                dumpMemory("scratch_begin.bin", s.scratchpad, 0x4000u);
                std::fprintf(stderr, "[sc3:record] frame %llu: recording to %s\n", static_cast<unsigned long long>(frame), path.c_str());
            }
            else if (frame == s.recordFrame + 1u && s.records)
            {
                std::fclose(s.records);
                s.records = nullptr;
                dumpMemory("rdram_end.bin", s.rdram, PS2_RAM_SIZE);
                dumpMemory("scratch_end.bin", s.scratchpad, 0x4000u);
                std::fprintf(stderr, "[sc3:record] frame %llu: %u records, memory dumped\n", static_cast<unsigned long long>(frame), s.recordSeq);
            }
        }

        void frameKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint64_t frame = g_state.frame.fetch_add(1, std::memory_order_relaxed) + 1u;
            recordFrameEdge(frame);
            if ((frame & 15u) == 0u)
                followHideFile();
            report();
            if (tracingFrame())
                std::fprintf(stderr, "[sc3:trace] f%llu kick\n", static_cast<unsigned long long>(frame));
            hideable(rdram, ctx, runtime);
        }

        void fileRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t sp = getRegU32(ctx, 29);
            std::fprintf(stderr,
                         "[sc3:load] f%llu request ra=%#x a0=%#x mode=%u pos=%#x size=%#x dest=%#x t1=%#x s0=%#x s1=%#x s2=%#x\n",
                         static_cast<unsigned long long>(g_state.frame.load()), getRegU32(ctx, 31), getRegU32(ctx, 4),
                         getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7), getRegU32(ctx, 8), getRegU32(ctx, 9),
                         peek32(sp), peek32(sp + 8u), peek32(sp + 16u));
            callOriginal(kFileRequest, rdram, ctx, runtime);
        }

        void cdRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            std::fprintf(stderr, "[sc3:load] f%llu sceCdRead lbn=%#x sectors=%u buf=%#x ra=%#x\n",
                         static_cast<unsigned long long>(g_state.frame.load()), getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6),
                         getRegU32(ctx, 31));
            callOriginal(kCdRead, rdram, ctx, runtime);
        }

        void memcpyTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (getRegU32(ctx, 6) >= 0x1000u)
                std::fprintf(stderr, "[sc3:load] f%llu memcpy dst=%#x src=%#x size=%#x ra=%#x\n",
                             static_cast<unsigned long long>(g_state.frame.load()), getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6),
                             getRegU32(ctx, 31));
            callOriginal(kMemcpy, rdram, ctx, runtime);
        }

        void batchFlush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (tracingFrame())
                std::fprintf(stderr, "[sc3:trace] f%llu batch obj=%#x list=%#x a2=%#x ra=%#x\n",
                             static_cast<unsigned long long>(g_state.frame.load()), getRegU32(ctx, 4), getRegU32(ctx, 5),
                             getRegU32(ctx, 6), getRegU32(ctx, 31));
            hideable(rdram, ctx, runtime);
        }

        void gifPacketClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31);
            if (tracingFrame() && (ra == kTextureCallReturns[0] || ra == kTextureCallReturns[1]))
            {
                const uint32_t cursor = peek32(0x70000000u);
                const uint32_t target = peek32(cursor + 4u);
                std::fprintf(stderr, "[sc3:trace] f%llu texture call %#x tag %#x ra=%#x at target: %08x %08x %08x %08x %08x %08x\n",
                             static_cast<unsigned long long>(g_state.frame.load()), target, peek32(cursor), ra, peek32(target),
                             peek32(target + 4u), peek32(target + 16u), peek32(target + 20u), peek32(target + 32u), peek32(target + 36u));
            }
            callOriginal(kGifPacketClose, rdram, ctx, runtime);
        }

        void otInsert(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t address = ctx->pc;
            const uint32_t ra = getRegU32(ctx, 31);
            if (g_state.traceOt)
                ++g_state.otCalls[{address, ra}];
            if (address == kOtInsertDepth && tracingFrame() && (ra == kTextureOtCallReturns[0] || ra == kTextureOtCallReturns[1]))
            {
                const uint32_t start = getRegU32(ctx, 5) - 0x10u;
                std::fprintf(stderr, "[sc3:trace] f%llu texture call (ot) %#x tag %#x ra=%#x\n",
                             static_cast<unsigned long long>(g_state.frame.load()), peek32(start + 4u), peek32(start), ra);
            }
            hideable(rdram, ctx, runtime);
        }

        void syncPoint(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (tracingFrame())
                std::fprintf(stderr, "[sc3:trace] f%llu sync %#x ra=%#x\n", static_cast<unsigned long long>(g_state.frame.load()), ctx->pc,
                             getRegU32(ctx, 31));
            callOriginal(ctx->pc, rdram, ctx, runtime);
        }

        void gsHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const bool trace = tracingFrame();
            const uint32_t before = peek32(kSignalCounts & ~3u);
            const uint32_t sig = runtime->memory().read32(0x12001080u);
            callOriginal(kGsHandler, rdram, ctx, runtime);
            if (trace)
                std::fprintf(stderr, "[sc3:trace] f%llu gs-handler sig=%#x counts %u/%u -> %u/%u\n",
                             static_cast<unsigned long long>(g_state.frame.load()), sig, (before >> 16) & 0xFFu, (before >> 24) & 0xFFu,
                             peek8(kSignalCounts), peek8(kSignalCounts + 1u));
        }

        void wrap(PS2Runtime &runtime, uint32_t address, Function hook)
        {
            if (g_state.originals.count(address) != 0u)
                return;
            Function original = runtime.lookupFunction(address);
            if (!original)
            {
                std::fprintf(stderr, "[sc3] census: no function at %#x\n", address);
                return;
            }
            g_state.originals[address] = original;
            runtime.registerFunction(address, hook);
        }
    }
    }

    bool censusTracingDraws()
    {
        return census::g_state.traceCount != 0u && census::tracingFrame();
    }

    void censusNoteDraw(uint64_t tex0, uint64_t tag, uint32_t triangles)
    {
        std::fprintf(stderr, "[sc3:trace] f%llu draw tex0=%#llx tag=%#llx tris=%u counts %u/%u\n",
                     static_cast<unsigned long long>(census::g_state.frame.load()), static_cast<unsigned long long>(tex0),
                     static_cast<unsigned long long>(tag), triangles, census::peek8(census::kSignalCounts), census::peek8(census::kSignalCounts + 1u));
    }

    void installCensus(PS2Runtime &runtime)
    {
        census::State &s = census::g_state;
        s.rdram = runtime.memory().getRDRAM();
        s.scratchpad = runtime.memory().getScratchpad();
        const char *hide = std::getenv("SC3_HIDE_EE");
        const char *loads = std::getenv("SC3_TRACE_LOAD");
        const char *frames = std::getenv("SC3_TRACE_FRAMES");
        const char *ot = std::getenv("SC3_TRACE_OT");
        s.traceLoads = loads && *loads == '1';
        s.traceOt = ot && *ot == '1';
        if (frames && *frames)
        {
            s.traceFirst = std::strtoull(frames, nullptr, 10);
            const char *comma = std::strchr(frames, ',');
            s.traceCount = comma ? std::strtoull(comma + 1, nullptr, 10) : 1u;
        }
        std::vector<uint32_t> candidates;
        if (hide && *hide == '@')
        {
            s.hidePath = hide + 1;
            if (const char *all = std::getenv("SC3_HIDE_EE_ALL"))
                candidates = census::parseAddresses(all);
            census::followHideFile();
        }
        else if (hide && *hide)
        {
            candidates = census::parseAddresses(hide);
            s.hidden.insert(candidates.begin(), candidates.end());
        }
        if (const char *record = std::getenv("SC3_RECORD_FRAME"); record && *record)
        {
            s.recordFrame = std::strtoull(record, nullptr, 10);
            const char *dir = std::getenv("SC3_RECORD_DIR");
            s.recordDir = dir && *dir ? dir : ".";
        }
        if (candidates.empty() && !s.traceLoads && s.traceCount == 0u && !s.traceOt && s.recordFrame == ~0ull)
            return;
        census::wrap(runtime, census::kFrameKick, census::frameKick);
        if (s.traceLoads)
        {
            census::wrap(runtime, census::kFileRequest, census::fileRequest);
            census::wrap(runtime, census::kCdRead, census::cdRead);
            census::wrap(runtime, census::kMemcpy, census::memcpyTraced);
        }
        if (s.traceCount != 0u)
        {
            census::wrap(runtime, census::kBatchFlush, census::batchFlush);
            census::wrap(runtime, census::kGifPacketClose, census::gifPacketClose);
            for (const uint32_t address : census::kSyncPoints)
                census::wrap(runtime, address, census::syncPoint);
            census::wrap(runtime, census::kGsHandler, census::gsHandler);
        }
        if (s.traceOt || s.traceCount != 0u)
            for (const uint32_t address : census::kOtInserts)
                census::wrap(runtime, address, census::otInsert);
        for (const uint32_t address : candidates)
            census::wrap(runtime, address, census::hideable);
        if (s.recordFrame != ~0ull)
        {
            std::vector<uint32_t> recorded(std::begin(census::kRecordedDefault), std::end(census::kRecordedDefault));
            if (const char *list = std::getenv("SC3_RECORD_FUNCTIONS"); list && *list)
                recorded = census::parseAddresses(list);
            for (const uint32_t address : recorded)
                if (address != census::kFrameKick && s.recordOriginals.count(address) == 0u)
                    if (census::Function original = runtime.lookupFunction(address))
                    {
                        s.recordOriginals[address] = original;
                        runtime.registerFunction(address, census::recorded);
                    }
        }
        std::fprintf(stderr, "[sc3] census: %zu function(s) hideable%s%s%s\n", candidates.size(), s.traceLoads ? ", loads traced" : "",
                     s.traceCount ? ", frames traced" : "", s.traceOt ? ", OT insertions counted" : "");
    }
}
