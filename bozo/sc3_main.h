// What SC3's three programs share: the port installed and checked against the generated code, and the disc image.
#pragma once

#include "sc3_port.h"

#include "bozo/host/free_host.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifndef BOZO_RUN_DEFAULT_ELF
#define BOZO_RUN_DEFAULT_ELF ""
#endif
#ifndef SC3_DEFAULT_ISO
#define SC3_DEFAULT_ISO ""
#endif
#ifndef SC3_DEFAULT_SYMBOL_MAP
#define SC3_DEFAULT_SYMBOL_MAP ""
#endif

namespace sc3
{
    // The build's symbol map for the free host, on an engine that reads them (a pin older than symbol maps has no field).
    inline void useSymbolMap(bozo::host::FreeOptions &options)
    {
        [](auto &o) {
            if constexpr (requires { o.symbolMap; })
                o.symbolMap = SC3_DEFAULT_SYMBOL_MAP;
        }(options);
    }

    // The port's overrides, false (with the addresses listed) when one is not an external of the generated code.
    inline bool installPort(const char *program, bozo::port::Registry &registry)
    {
        bozo::port::port()->install(registry);
        const std::vector<uint32_t> missing = bozo::port::undeclared(registry, sc3_external_table, sc3_external_count);
        if (missing.empty())
            return true;
        std::fprintf(stderr, "%s: %zu registered overrides are not externals of the generated code (regenerate):", program, missing.size());
        for (uint32_t a : missing)
            std::fprintf(stderr, " 0x%x", a);
        std::fprintf(stderr, "\n");
        return false;
    }

    // --iso, else SC3_ISO, else the build's SC3_ISO, else the one .iso beside the ELF or in its parent's iso/ folder.
    inline std::string findDiscImage(const std::string &elf)
    {
        if (const char *env = std::getenv("SC3_ISO"); env && *env)
            return env;
        if (*SC3_DEFAULT_ISO)
            return SC3_DEFAULT_ISO;
        namespace fs = std::filesystem;
        const fs::path dir = fs::path(elf).parent_path();
        std::error_code ec;
        for (const fs::path &where : {dir, dir.parent_path() / "iso"})
        {
            std::vector<fs::path> found;
            for (fs::directory_iterator it(where, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec) && (it->path().extension() == ".iso" || it->path().extension() == ".ISO"))
                    found.push_back(it->path());
            if (found.size() == 1)
                return found.front().string();
        }
        return "";
    }

    // A frame that ends with no wait of its own (a movie's, in ADXM's sleep) still takes a field of EE time: what the game's
    // threads, the mixer and a deterministic movie clock run on.
    constexpr uint64_t kFramePeriodCycles = bozo::port::kEeClockHz * 1001u / 60000u;

    // PLAN's exit criterion: no guest code on a normal route reaches a register answered with no device behind it. The
    // count is 0 at release; the sites come with --profile-io.
    inline void reportAcknowledged(const bozo::host::FreeStats &stats)
    {
        uint64_t gifVif = 0, imr = 0, sigLblId = 0, condition = 0, scratchpad = 0, channels = 0;
        for (const auto &[site, counts] : stats.ioSites)
        {
            const uint64_t hits = counts[0] + counts[1];
            const uint32_t a = site.second == bozo::host::kCop0ConditionSite ? site.second : site.second & 0x1FFFFFFFu;
            if (a == bozo::host::kCop0ConditionSite)
                condition += hits;
            else if (a == 0x10003010u || a == 0x10003020u || a == 0x10003800u || a == 0x10003820u || a == 0x10003C00u || a == 0x10003C20u)
                gifVif += hits;
            else if (a == 0x12001010u)
                imr += hits;
            else if (a == 0x12001080u)
                sigLblId += hits;
            else if (a == 0x1000D000u || a == 0x1000D400u)
                scratchpad += hits;
            else if (a >= 0x10008000u && a < 0x1000A040u)
                channels += hits;
        }
        std::fprintf(stderr, "[sc3:acknowledged] %llu hits: GIF/VIF status %llu, GS IMR %llu, SIGLBLID %llu, BC0F/BC0T %llu, scratchpad CHCR %llu; VIF0/VIF1/GIF channels %llu\n",
                     static_cast<unsigned long long>(gifVif + imr + sigLblId + condition + scratchpad),
                     static_cast<unsigned long long>(gifVif), static_cast<unsigned long long>(imr),
                     static_cast<unsigned long long>(sigLblId), static_cast<unsigned long long>(condition),
                     static_cast<unsigned long long>(scratchpad), static_cast<unsigned long long>(channels));
    }

    // A run's end, said plainly: the first call nothing answers yet is where the port stands.
    inline void reportEnd(const char *program, const bozo::host::FreeHost &host, uint64_t frames)
    {
        const std::string reason = host.endReason();
        if (reason.rfind("FAULT", 0) == 0)
            std::fprintf(stderr, "%s: stopped after %llu frames at the first call BozoEngine does not answer yet: %s\n", program,
                         static_cast<unsigned long long>(frames), reason.c_str());
        else
            std::fprintf(stderr, "%s: the game ended after %llu frames: %s\n", program, static_cast<unsigned long long>(frames),
                         reason.empty() ? "every thread ended" : reason.c_str());
    }
}
