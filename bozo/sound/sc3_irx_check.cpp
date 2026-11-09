// sc3-irx-check: SC3's sound modules started natively with no game: each module the ELF loads by its disc path, in the
// ELF's order with its arguments, through the port's loader. Prints the RPC servers they register. An unbound import stops it, named.
#include "sc3_audio.h"

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

int main()
{
    bozo::libs::Libraries libraries;
    libraries.printOutput = true;
    auto &host = sc3::audio::attach(libraries);
    host.advanceTo(0);
    int failures = 0;
    for (const char *module : {"sio2man.irx", "padman.irx", "mcman.irx", "mcserv.irx", "libsd.irx", "sdrdrv.irx", "nuSound.irx", "cri_adxi.irx"})
    {
        const std::string path = std::string("cdrom0:\\irx\\") + module + ";1";
        try
        {
            const bool adx = path.find("cri_adxi") != std::string::npos;
            std::printf("%s: %d\n", path.c_str(),
                        libraries.sif.moduleLoaderWithArguments(path, adx ? std::vector<std::string>{"thpri=39", "spucore=2"} : std::vector<std::string>{}));
        }
        catch (const std::exception &error)
        {
            std::printf("%s: FAILED: %s\n", path.c_str(), error.what());
            ++failures;
        }
    }
    uint64_t cycle = 0;
    try
    {
        for (int tick = 0; tick < 120; ++tick)
            host.advanceTo(cycle += 294912000 / 60);
    }
    catch (const std::exception &error)
    {
        std::printf("two seconds of ticks: FAILED at EE cycle %llu: %s\n", static_cast<unsigned long long>(cycle), error.what());
        ++failures;
    }
    std::printf("RPC servers:");
    for (const auto &[id, server] : libraries.sif.servers)
        std::printf(" 0x%x", id);
    std::printf("\n%llu native calls, %d failures\n", static_cast<unsigned long long>(host.nativeCalls()), failures);
    return failures ? 1 : 0;
}
