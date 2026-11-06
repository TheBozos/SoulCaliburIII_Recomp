// sc3-check: BozoEngine's call-log checker (bozo-run, BozoEngine's tools/run/README.md) with SC3's overrides running
// natively, each call checked against the reference's log (reference/README.md):
//   sc3-check <log.clg> --pinned --ref <run.ref> [--iso <image>] [--pad-script <file>] [--overrides log] ...
// The ELF defaults to the one this build was generated from.
#include "run_main.h"

#include <cstring>
#include <string>
#include <vector>

#ifndef BOZO_RUN_DEFAULT_ELF
#define BOZO_RUN_DEFAULT_ELF ""
#endif

int main(int argc, char **argv)
{
    std::vector<char *> args(argv, argv + argc);
    bool hasElf = false;
    for (int i = 1; i < argc; ++i)
        hasElf = hasElf || std::strcmp(argv[i], "--elf") == 0;
    static char elfFlag[] = "--elf";
    static std::string elf = BOZO_RUN_DEFAULT_ELF;
    if (!hasElf && !elf.empty() && argc >= 2)
    {
        args.push_back(elfFlag);
        args.push_back(elf.data());
    }
    return bozo::run::main(static_cast<int>(args.size()), args.data());
}
