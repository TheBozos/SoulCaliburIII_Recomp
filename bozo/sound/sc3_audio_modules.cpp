#include "sc3_audio.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>

namespace sc3::audio
{
    namespace
    {
        constexpr uint32_t kHeapStart = 0x00028000u;
        constexpr uint32_t kNuSoundDelay = 0x00021510u;
    }

    std::string moduleName(const std::string &path)
    {
        const size_t slash = path.find_last_of("\\/:");
        std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
        name = name.substr(0, name.find(';'));
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::toupper(c); });
        return name + ";1";
    }

    bozo::native::irx::CompiledLibraries &attach(bozo::libs::Libraries &libraries)
    {
        static std::unique_ptr<bozo::native::irx::CompiledLibraries> host;
        host = std::make_unique<bozo::native::irx::CompiledLibraries>(libraries, kHeapStart);
        std::vector<std::string> names;
        for (auto &image : images())
        {
            names.push_back(image.name);
            host->addImage(std::move(image));
        }
        host->bindNative(kNuSoundDelay, [](bozo::native::irx::Env &e)
                         {
                             e.cpu.gpr[2] = 1;
                             e.cpu.pc = e.cpu.gpr[31];
                         });
        auto *h = host.get();
        libraries.sif.moduleLoaderWithArguments = [h, names, others = int32_t(0)](const std::string &path, const std::vector<std::string> &arguments) mutable
        {
            const std::string name = moduleName(path);
            if (std::find(names.begin(), names.end(), name) != names.end())
            {
                const int32_t id = h->startImage(name, arguments);
                std::printf("[sc3-audio] %s started natively: module %d\n", name.c_str(), id);
                return id;
            }
            return 0x1000 + ++others;
        };
        libraries.sif.transfer = [h](uint32_t address, const uint8_t *data, uint32_t bytes) { h->writeBuffer(address, data, bytes); };
        libraries.sif.allocateBuffer = [h](uint32_t bytes) { return h->allocateBuffer(bytes); };
        libraries.sif.freeBuffer = [h](uint32_t address) { return h->freeBuffer(address); };
        libraries.sif.resetProvider = [h] { h->reset(); };
        libraries.audioProvider = [h](uint64_t cycle) { return h->advanceTo(cycle); };
        return *host;
    }
}
