// sc3-headless: Soulcalibur III on BozoEngine with no window. Every library native, the disc image read, BozoEngine's
// own timing (free mode, deterministic), SC3's overrides native.
//
//   sc3-headless [--frames n] [--elf SLUS_212.16] [--iso image] [--pad-script [n=]file]... [--card-dir dir]
//                [--out run.ref] [--report n] [--print] [--quiet] [--profile-io] [--wav file] [--no-sound]
#include "sc3_main.h"

#include "bozo/app/determinism.h"
#include "bozo/audio/wav.h"
#include "bozo/ee/dispatcher.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <string>

int main(int argc, char **argv)
{
    bozo::host::FreeOptions opt;
    opt.elf = BOZO_RUN_DEFAULT_ELF;
    sc3::useSymbolMap(opt);
    uint64_t report = 500;
    std::string wavPath;
    for (int k = 1; k < argc; ++k)
    {
        const std::string a = argv[k];
        auto value = [&]() -> std::string { return k + 1 < argc ? argv[++k] : ""; };
        if (a == "--frames")
            opt.frames = static_cast<uint32_t>(std::stoul(value()));
        else if (a == "--elf")
            opt.elf = value();
        else if (a == "--iso")
            opt.iso = value();
        else if (a == "--pad-script")
        {
            std::string v = value();
            int player = 1;
            if (v.size() > 2 && v[1] == '=' && v[0] >= '1' && v[0] <= '4')
            {
                player = v[0] - '0';
                v = v.substr(2);
            }
            (player == 1 ? opt.padScript : opt.playerScripts[static_cast<size_t>(player - 2)]) = v;
        }
        else if (a == "--card-dir")
            opt.cardDir = value();
        else if (a == "--out")
            opt.out = value();
        else if (a == "--report")
            report = std::stoull(value());
        else if (a == "--quiet")
            opt.quiet = true;
        else if (a == "--print")
            opt.printGameOutput = true;
        else if (a == "--profile-io")
            opt.profileIo = true;
        else if (a == "--wav")
            wavPath = value();
        else if (a == "--no-sound")
            opt.sound = false;
        else
        {
            std::fprintf(stderr, "usage: sc3-headless [--frames n] [--elf path] [--iso image] [--pad-script [n=]file]... [--card-dir dir] "
                                 "[--out run.ref] [--report n] [--print] [--quiet] [--profile-io] [--wav file] [--no-sound]\n");
            return 2;
        }
    }
    if (opt.elf.empty())
    {
        std::fprintf(stderr, "sc3-headless: no ELF (--elf)\n");
        return 2;
    }
    if (opt.iso.empty())
        opt.iso = sc3::findDiscImage(opt.elf);
    if (opt.iso.empty())
    {
        std::fprintf(stderr, "sc3-headless: no disc image (--iso, SC3_ISO, or one .iso beside the ELF)\n");
        return 2;
    }
    bozo::det::setEnabled(true);

    bozo::port::Registry registry;
    if (!sc3::installPort("sc3-headless", registry))
        return 2;

    bozo::host::FreeHost host(opt);
    bozo::ee::Dispatcher d(host);
    d.setFunctions(sc3_function_table, sc3_function_count, sc3_resume_table, sc3_resume_count);
    std::string error;
    if (!host.setUp(d, sc3_external_table, sc3_external_count, &registry, error))
    {
        std::fprintf(stderr, "sc3-headless: %s\n", error.c_str());
        return 2;
    }
    std::printf("sc3-headless: %s, %zu externals, %zu overrides, ELF %s, disc %s\n", bozo::port::port()->title, sc3_external_count,
                registry.functions().size(), opt.elf.c_str(), opt.iso.c_str());

    const auto t0 = std::chrono::steady_clock::now();
    std::string waiter;
    std::map<std::string, uint64_t> waiters;
    auto state = [&](uint64_t frame) {
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("frame %llu: EE cycle %llu (%llu vblanks), loop %s; %.1f s\n", static_cast<unsigned long long>(frame),
                    static_cast<unsigned long long>(d.now()), static_cast<unsigned long long>(d.kernel().vblankCount()), waiter.c_str(),
                    secs);
        std::fflush(stdout);
    };
    host.onFrame = [&](uint64_t frame, const R5900Context *ctx) {
        if (ctx)
        {
            waiter = host.functionAt(ctx->r[31].uw[0]);
            ++waiters[waiter.substr(0, waiter.find('+'))];
        }
        if (report && (frame + 1) % report == 0 && !opt.quiet)
            state(frame + 1);
    };
    bozo::audio::WavWriter wav;
    if (!wavPath.empty())
    {
        if (!wav.open(wavPath))
        {
            std::fprintf(stderr, "sc3-headless: cannot write %s\n", wavPath.c_str());
            return 2;
        }
        host.onAudio = [&](uint64_t, const int16_t *samples, size_t count) { wav.write(samples, count); };
    }
    const uint64_t frames = host.run();
    if (wav.isOpen())
        wav.close();
    state(frames);
    const bozo::host::FreeStats &s = host.stats();
    std::printf("end: %llu frames; %llu library calls, %llu kernel calls, %llu override calls, %llu VU0 programs, %llu hardware "
                "accesses (%llu to registers the I/O model does not know)\n",
                static_cast<unsigned long long>(frames), static_cast<unsigned long long>(s.libraryCalls),
                static_cast<unsigned long long>(s.kernelCalls), static_cast<unsigned long long>(s.overrideCalls),
                static_cast<unsigned long long>(s.vu0Calls), static_cast<unsigned long long>(s.ioAccesses),
                static_cast<unsigned long long>(s.ioUnknown));
    std::printf("frame ends by loop function:");
    for (const auto &[fn, n] : waiters)
        std::printf(" %s %llu;", fn.c_str(), static_cast<unsigned long long>(n));
    std::printf("\n");
    for (const auto &[name, n] : s.unanswered)
        std::printf("no native answer: %s (%llu)\n", name.c_str(), static_cast<unsigned long long>(n));
    if (opt.profileIo)
        for (const auto &[site, rw] : s.ioSites)
            std::printf("device access: pc 0x%x address 0x%x: %llu reads, %llu writes\n", site.first, site.second,
                        static_cast<unsigned long long>(rw[0]), static_cast<unsigned long long>(rw[1]));
    std::fflush(stdout);
    sc3::reportEnd("sc3-headless", host, frames);
    return host.endReason().rfind("FAULT", 0) == 0 ? 1 : 0;
}
