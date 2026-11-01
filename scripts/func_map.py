#!/usr/bin/env python3
"""Soulcalibur III function map: every EE function of SLUS_212.16 and every recompiled IRX function, coloured by how
far it is from running without emulation.

    python3 scripts/func_map.py [--elf PATH] [--generated DIR] [--out DIR] [--coverage FILE]

Reads the game ELF (static MIPS analysis), recomp/sc3.toml, the generated tree (function list, stub bindings,
generated/iop/), runtime/ (game overrides) and PS2Recomp's runtime sources (stub handlers, syscall dispatcher).
Writes map.json into --out and appends the counts to history.json there (or --history).
Everything it writes is derived from the game: keep it out of the repository.
"""

import argparse
import datetime
import json
import os
import re
import struct
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

TOOL_VERSION = 1
ROOT = Path(__file__).resolve().parent.parent

NATIVE, CLEAN, DIRECT, INDIRECT, STUB = range(5)
STATES = [
    ("native", "Native C++", "Hand-written or lifted C++ runs instead of the game's code (game overrides, VU programs)."),
    ("clean", "Recompiled, clean", "Generated C++ from the MIPS; no emulated hardware touched by the function or its callees."),
    ("direct", "Touches emulation", "The function itself reads or writes emulated hardware (or its VU0 micro mode): the to-do list."),
    ("indirect", "Calls emulation", "Calls, one or two levels down, a function that touches emulation or an emulation-backed stub."),
    ("stub", "Runtime stub / HLE", "Library function the runtime answers by name (or a kernel syscall wrapper)."),
]

FLAG_UNCERTAIN = 1
FLAG_EMU_STUB = 2
FLAG_RAN = 4
FLAG_WRAPPER = 8

DMA_CHANNELS = [
    (0x10008000, "D0 VIF0"), (0x10009000, "D1 VIF1"), (0x1000A000, "D2 GIF"), (0x1000B000, "D3 fromIPU"),
    (0x1000B400, "D4 toIPU"), (0x1000C000, "D5 SIF0"), (0x1000C400, "D6 SIF1"), (0x1000C800, "D7 SIF2"),
    (0x1000D000, "D8 fromSPR"), (0x1000D400, "D9 toSPR"),
]
DMA_REGS = {0x00: "CHCR", 0x10: "MADR", 0x20: "QWC", 0x30: "TADR", 0x40: "ASR0", 0x50: "ASR1", 0x80: "SADR"}
HW_REGIONS = [
    (0x10000000, 0x10002000, "timer", "EE timers"),
    (0x10002000, 0x10003000, "ipu", "IPU"),
    (0x10003000, 0x10003800, "gif", "GIF"),
    (0x10003800, 0x10003C00, "vif", "VIF0"),
    (0x10003C00, 0x10004000, "vif", "VIF1"),
    (0x10004000, 0x10005000, "fifo", "VIF0 FIFO"),
    (0x10005000, 0x10006000, "fifo", "VIF1 FIFO"),
    (0x10006000, 0x10007000, "fifo", "GIF FIFO"),
    (0x10007000, 0x10008000, "fifo", "IPU FIFO"),
    (0x10008000, 0x1000E000, "dma", "DMA channel"),
    (0x1000E000, 0x1000F000, "dmac", "DMAC control"),
    (0x1000F000, 0x1000F100, "intc", "INTC"),
    (0x1000F200, 0x1000F300, "sif", "SIF registers"),
    (0x1000F500, 0x1000F600, "dmac", "DMAC enable"),
    (0x10000000, 0x10010000, "eeio", "EE I/O"),
    (0x11000000, 0x11004000, "vumem", "VU0 micro memory"),
    (0x11004000, 0x11008000, "vumem", "VU0 data memory"),
    (0x11008000, 0x1100C000, "vumem", "VU1 micro memory"),
    (0x1100C000, 0x11010000, "vumem", "VU1 data memory"),
    (0x12000000, 0x12001000, "gs", "GS privileged"),
    (0x12001000, 0x12002000, "gs", "GS CSR/IMR"),
]
POLL_REGS = {0x1000E010: "D_STAT", 0x12001000: "GS CSR", 0x1000F000: "I_STAT", 0x1000F230: "SIF SMFLG"}
VU_CONTROL = {27: "CMSAR0", 28: "FBRST", 29: "VPU-STAT", 31: "CMSAR1"}
TOUCH_KINDS = {
    "dma": "DMA channel registers", "dmac": "DMAC control", "gif": "GIF registers", "vif": "VIF registers",
    "fifo": "hardware FIFO", "gs": "GS privileged registers", "vumem": "VU memory", "timer": "EE timers",
    "intc": "INTC", "sif": "SIF registers", "ipu": "IPU", "eeio": "other EE I/O",
    "vu0micro": "VU0 micro mode (VCALLMS)", "vuctl": "VU control registers", "tag": "VIF code / DMA tag constant",
    "addr": "hardware address passed on", "mmio": "analyzer-resolved MMIO",
}

EMU_KEYWORDS = [
    ("I/O registers", r"0x1[0-2]00(?!0000)[0-9A-Fa-f]{4}|IORegister"),
    ("DMAC model", r"[Dd]mac|DMAC|processDma|[Dd]maChannel|CHCR|Chcr"),
    ("GIF", r"\bgif\w*|\bGif(?!Pk)\w*|GIF"),
    ("VIF", r"\bvif\d?\w*|\bVif\d?\w*|VIF"),
    ("GS model", r"\bgs\(\)|\.gs\(|GsMem|gsMemory|\bm_gs\b|\bCSR\b|\bIMR\b"),
    ("VU1", r"vu1|Vu1|VU1"),
    ("IOP / SIF", r"\biop\w*|\bIop\w*|IOP|\bsif\w*|\bSif\w*|SIF|\brpc\w*|Rpc\b"),
    ("SPU2", r"spu2|Spu2|SPU2"),
    ("interrupt model", r"addIrqHandler|[Ii]rqHandler|raiseIrq|[Ii]ntc|INTC"),
]
WEAK_KEYWORDS = [("waits on the emulated vblank", r"waitVSync|currentVSyncTick|vsyncTick|[Vv][Bb]lank")]

LIBRARIES = [
    ("libmpeg / libipu", r"^_?sceMpeg|^_?sceIpu|^_(clearOnce|sendData|clearEach|getpic|updateRef|decodeOr|decPicture|cpr|dispRef|doC|ch3dma|dualPrime|getAll|motion|getRef|doMC|slice|decMB|waitIpu|skipMB|copyAdd|copyRef|rix|ri0|initSeq|sequence|groupOf|picture|quantMatrix|extensionAnd|copyright)|^dmaRefImage|^receiveData|^flushByte|^setD[34]_CHCR"),
    ("libm", r"^(ceilf|cosf|floorf|sinf|atanf|fabsf|__ieee754|__kernel_)"),
    ("libdma", r"^(WaitDma|WatchDma|CheckAddress|memclr|sceDma)"),
    ("libgraph / libvu0 / libdev", r"^(sceGs|sceGif|sceVu|sceDev)"),
    ("libpad", r"^(scePad|_pad_|_send_to_iop)"),
    ("libsdr", r"^(sceSd|_sdr)"),
    ("libcdvd / libscf", r"^(_?sceCd|cmd_sem|cdvd_|PowerOffCB|ret1|GetRomName|IsT10K|sceScf|tobcd|frombcd|adddate|subdate|addhour|subhour|AdjustTime)"),
    ("libmc", r"^(sceMc|mce)"),
    ("libpc / libgcc / C++ runtime", r"^(scePc|__muldi|__divdi|__moddi|__udivdi|__umoddi|__float|__fix|__throw|__rethrow|new_eh|eh_|__get_|init_reg|__eh_|__sj|old_find|find_|get_reg|copy_reg|next_stack|throw_helper|fptodp|_fpadd|dp|litodp|__make|decode_|fde_|end_fde|__register|__deregister|count_fdes|add_fdes|search_fdes|frame_init|extract_cie|execute_cfa|__frame|__unpack|__pack|__fpcmp)"),
    ("newlib libc", r"^(atexit|atoi|exit|malloc|free|_malloc|memcmp|memcpy|memset|__malloc|_init_signal|_signal|_raise|__sigtramp|_sprintf|sscanf|str|_str|__sprint|__sbprintf|_vfprintf|cvt|exponent|__svfscanf|__sccl|vsprintf|__swsetup|quorem|_dtoa|fflush|std|__sf|__sinit|fread|_free|__sfvwrite|_fwalk|_setlocale|__smakebuf|_mbtowc|memchr|memmove|_Balloc|_Bfree|_multadd|_s2b|_hi0|_lo0|_i2b|_multiply|_pow5|_lshift|__mcmp|__mdiff|_ulp|_b2d|_d2b|_ratio|_mprec|lflush|__srefill|__sread|__swrite|__sseek|__submore|ungetc|_calloc|_realloc)"),
    ("libkernel", r"^(VSync|write$|read$|sbrk|kill|_sceSDC|_sceIDC|topThread|InitThread|iWakeup|iRotate|iSuspend|DelayThread|StartThread|_DelayThread|QueuePeek|sceTty|kputchar|deci2|serialPutchar|ftoi|printfloat|_sceSnprintf|snprintf|sceVsnprintf|sceSnprintf|sceVprintf|_?i?sceSif|_sceRpc|_request|_alarm|_search_svdata|_sceFs|use_iob|new_iob|sceFs|get_iob|scePower|sceOpen|sceClose|sceLseek|sceRead|sceWrite|sceIoctl|_sceCallCode|sceMkdir|sceFormat|sceAddDrv|sceD|sceGetstat|sceChstat|sceRename|sceSync|sceMount|sceSymlink|sceReadlink|_lf_bind|DIntr|supplement|kCopy|kFind|GetSystemCall|InitSystemCall|PatchIsNeeded|InitExecPS2|SetArg|ExecPS2|LoadExecPS2|ExecOSD|InitAlarm|SetT2|InitTimer|EndTimer|GetTimer|StartTimer|StopTimer|SetNextComp|InsertAlarm|UnlinkAlarm|cbTimer|iGet|iAlloc|AllocTimer|iFree|iStart|iStop|SetTimer|iSet|ReleaseTimer|iRelease|kputs|SetTLB|SetDebug|InitTLB|_kTLB|_kExit|_kDebug)"),
]


def hx(value):
    return f"0x{value:06x}"


def read_text(path):
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def git(cwd, *args):
    try:
        out = subprocess.run(["git", "-C", str(cwd), *args], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return ""
    return out.stdout.strip() if out.returncode == 0 else ""


class Elf:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
            raise SystemExit(f"{path}: not a 32-bit little-endian ELF")
        shoff, = struct.unpack_from("<I", d, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
        raw = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
        strtab = raw[shstrndx][4]
        self.sections = []
        for name, typ, flags, addr, off, size, *_ in raw:
            end = d.index(b"\0", strtab + name)
            label = d[strtab + name:end].decode("ascii", "replace")
            if addr and size and typ != 8:
                self.sections.append({"name": label, "addr": addr, "size": size, "off": off, "exec": bool(flags & 4)})

    def section_of(self, addr):
        for s in self.sections:
            if s["addr"] <= addr < s["addr"] + s["size"]:
                return s
        return None

    def words(self, start, end):
        s = self.section_of(start)
        if not s:
            return []
        end = min(end, s["addr"] + s["size"])
        off = s["off"] + start - s["addr"]
        count = max(0, (end - start) // 4)
        return list(struct.unpack_from(f"<{count}I", self.data, off))


def load_config(path):
    text = read_text(path)
    if not text:
        raise SystemExit(f"missing {path}")
    try:
        import tomllib
        data = tomllib.loads(text)
        stubs = data.get("general", {}).get("stubs", [])
        entries = data.get("general", {}).get("entry_points", [])
        mmio = data.get("mmio", {})
    except Exception:
        stubs = re.findall(r'"([^"]+@0x[0-9A-Fa-f]+)"', text.split("entry_points", 1)[0])
        entries = re.findall(r'"([^"]+@0x[0-9A-Fa-f]+)"', text.split("entry_points", 1)[-1].split("[mmio]", 1)[0])
        mmio = dict(re.findall(r'^"(0x[0-9a-fA-F]+)"\s*=\s*"(0x[0-9a-fA-F]+)"', text, re.M))

    def pairs(items):
        out = []
        for item in items:
            name, _, addr = item.partition("@")
            if addr:
                out.append((name, int(addr, 16)))
        return out

    return {
        "stubs": pairs(stubs),
        "entries": pairs(entries),
        "mmio": {int(k, 16): int(v, 16) for k, v in mmio.items()},
    }


def load_functions(generated, config, elf):
    """Function starts, ends and stub bindings from the generated tree, else from recomp/ghidra_map.csv."""
    funcs = {}
    source = None
    if generated and Path(generated).is_dir():
        source = f"generated tree ({generated})"
        head_re = re.compile(r"// Function: (\S+)\n// Address: 0x([0-9a-f]+) - 0x([0-9a-f]+)")
        stub_re = re.compile(r"(ps2_stubs|ps2_syscalls)::(\w+)\(")
        for entry in os.scandir(generated):
            if not entry.name.endswith(".cpp") or not entry.is_file():
                continue
            with open(entry.path, encoding="utf-8", errors="replace") as fh:
                head = fh.read(4096)
            m = head_re.search(head)
            if m:
                start, end = int(m.group(2), 16), int(m.group(3), 16)
                funcs[start] = {"addr": start, "end": end, "gen": m.group(1), "kind": "recompiled"}
                continue
            name = re.match(r"(.+)_0x([0-9a-f]+)\.cpp$", entry.name)
            stub = stub_re.search(head)
            if name and stub:
                start = int(name.group(2), 16)
                funcs[start] = {"addr": start, "end": None, "gen": name.group(1), "kind": "stub",
                                "handler": stub.group(2), "ns": stub.group(1)}
    else:
        source = "recomp/ghidra_map.csv (no generated tree: function ends and stub bindings approximated)"
        for line in read_text(ROOT / "recomp" / "ghidra_map.csv").splitlines()[1:]:
            parts = line.split(",")
            if len(parts) >= 3:
                start = int(parts[1], 16)
                funcs.setdefault(start, {"addr": start, "end": None, "gen": parts[0], "kind": "recompiled"})
        for name, addr in config["stubs"]:
            funcs[addr] = {"addr": addr, "end": None, "gen": name, "kind": "stub", "handler": name, "ns": "ps2_stubs"}
    names = {}
    for name, addr in config["entries"] + config["stubs"]:
        if not name.startswith(("sub_", "entry_", "FUN_")):
            names[addr] = name
    starts = sorted(funcs)
    for i, start in enumerate(starts):
        f = funcs[start]
        nxt = starts[i + 1] if i + 1 < len(starts) else None
        sec = elf.section_of(start)
        limit = sec["addr"] + sec["size"] if sec else start + 4
        if f["end"] is None or f["end"] <= start:
            f["end"] = min(nxt or limit, limit, start + 0x400) if f["kind"] == "stub" else min(nxt or limit, limit)
        f["name"] = names.get(start) or (f["gen"] if not f["gen"].startswith(("sub_", "FUN_")) else "")
        f["section"] = sec["name"] if sec else "?"
    return [funcs[s] for s in starts], source


def strip_code(text):
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', text)


def bodies(text):
    """Every brace-delimited function body in a C++ file, by function name (later definitions append)."""
    out = defaultdict(list)
    code = strip_code(text)
    for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(([^;{}()]|\([^()]*\))*\)\s*(?:const\s*)?(?:noexcept\s*)?\{", code):
        name = m.group(1)
        if name in ("if", "for", "while", "switch", "catch", "return", "sizeof"):
            continue
        depth, i = 0, m.end() - 1
        while i < len(code):
            c = code[i]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        out[name].append(code[m.end():i])
    return out


class HandlerIndex:
    """PS2Recomp's stub and syscall handlers, classified by what their bodies (and the helpers they call) touch."""

    def __init__(self, ps2recomp):
        self.root = Path(ps2recomp) / "ps2xRuntime" / "src" / "lib" / "Kernel"
        self.defs = defaultdict(list)
        self.files = defaultdict(set)
        self.available = self.root.is_dir()
        if self.available:
            for path in sorted(self.root.rglob("*")):
                if path.suffix in (".cpp", ".h"):
                    for name, bs in bodies(read_text(path)).items():
                        self.defs[name].extend(bs)
                        self.files[name].add(path.relative_to(self.root).as_posix())
        self.cache = {}

    def classify(self, name):
        if name in self.cache:
            return self.cache[name]
        if not self.available or name not in self.defs:
            result = {"backing": "unknown", "evidence": "handler source not found", "file": ""}
            self.cache[name] = result
            return result
        text = "\n".join(self.defs[name])
        called = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", text)) - {name}
        helpers = [h for h in called if h in self.defs and len(self.defs[h]) <= 3]
        full = text + "\n" + "\n".join(b for h in helpers for b in self.defs[h])
        hits = [label for label, rx in EMU_KEYWORDS if re.search(rx, full)]
        weak = [label for label, rx in WEAK_KEYWORDS if re.search(rx, full)]
        result = {
            "backing": "emulation" if hits else "emulation?" if weak else "native",
            "evidence": ", ".join(hits + weak) if hits or weak else "no emulated subsystem referenced",
            "file": ", ".join(sorted(self.files[name])),
        }
        self.cache[name] = result
        return result


def syscall_table(ps2recomp):
    path = Path(ps2recomp) / "ps2xRuntime" / "src" / "lib" / "Kernel" / "Syscalls" / "Dispatcher.cpp"
    table, pending = {}, []
    for line in strip_code(read_text(path)).splitlines():
        cases = re.findall(r"case\s+(0x[0-9A-Fa-f]+|\d+)\s*:", line)
        if cases:
            pending.extend(int(c, 0) for c in cases)
        call = re.search(r"\b([A-Za-z_]\w*)\s*\(\s*rdram\s*,\s*ctx\s*,\s*runtime\s*\)", line)
        if call and pending:
            for n in pending:
                table[n] = call.group(1)
            pending = []
        if "return" in line and pending and not call:
            for n in pending:
                table.setdefault(n, f"inline 0x{n:02x}")
            pending = []
    return table


def load_overrides(runtime_dir):
    """registerFunction() calls of the game override that are on by default, with the handler they install."""
    consts, arrays, defs, regs = {}, {}, {}, []
    texts = {}
    for path in sorted(Path(runtime_dir).glob("*.cpp")):
        text = read_text(path)
        texts[path.name] = text
        for m in re.finditer(r"constexpr\s+uint32_t\s+(k\w+)\s*=\s*0x([0-9A-Fa-f]+)u?\s*;", text):
            consts[m.group(1)] = int(m.group(2), 16)
        for m in re.finditer(r"constexpr\s+uint32_t\s+(k\w+)\[\]\s*=\s*\{(.*?)\};", text, re.S):
            arrays[m.group(1)] = [(int(a, 16), n) for a, n in re.findall(r"0x([0-9A-Fa-f]+)u?\s*,?\s*(?://\s*(\w+))?", m.group(2))]
        for name, bs in bodies(text).items():
            defs.setdefault(name, bs)
    for fname, text in texts.items():
        lines = text.splitlines()
        for i, line in enumerate(lines):
            m = re.search(r"registerFunction\(\s*(\w+)\s*,\s*(\w+)\s*\)", line)
            if not m:
                continue
            indent = len(line) - len(line.lstrip())
            gated = None
            for j in range(i - 1, max(-1, i - 40), -1):
                prev = lines[j]
                pin = len(prev) - len(prev.lstrip())
                stripped = prev.strip()
                if not stripped or stripped in ("{", "}"):
                    continue
                if pin < indent:
                    if stripped.startswith("for "):
                        indent = pin
                        continue
                    if stripped.startswith(("if ", "else if ")):
                        cond = stripped
                        env = re.match(r"(?:else )?if \((?:const char \*\w+ = )?std::getenv\(\"(\w+)\"\)(.*)", cond)
                        if env and not re.match(r"\s*;\s*!", env.group(2)):
                            gated = env.group(1)
                            break
                        indent = pin
                        continue
                    if re.match(r"(void|bool|static|namespace)\b", stripped):
                        break
                    indent = pin
            if gated:
                continue
            target, handler = m.group(1), m.group(2)
            addrs = []
            if target in consts:
                addrs = [(consts[target], target[1:])]
            elif target == "address":
                loop = "\n".join(lines[max(0, i - 3):i])
                arr = re.search(r"for \(const uint32_t address : (k\w+)\)", loop)
                if arr:
                    addrs = [(a, n or arr.group(1)[1:]) for a, n in arrays.get(arr.group(1), [])]
            body = "\n".join(defs.get(handler, []))
            wrapper = bool(re.search(r"\bg_\w+\s*\(\s*rdram\s*,\s*ctx\s*,\s*runtime\s*\)", body) or
                           re.search(r"\bs\.\w+\s*\(\s*rdram\s*,\s*ctx\s*,\s*runtime\s*\)", body))
            for addr, label in addrs:
                regs.append({"addr": addr, "label": label[0].lower() + label[1:] if label else "", "handler": handler,
                             "file": fname, "wrapper": wrapper})
    out = {}
    for r in regs:
        out.setdefault(r["addr"], r)
    return out


def load_vu_programs(root):
    spec = read_text(Path(root) / "recomp" / "vu1_lift_spec.json")
    if not spec:
        return []
    programs = json.loads(spec).get("programs", {})
    port = read_text(Path(root) / "runtime" / "sc3_vu1_port.cpp")
    hand = {int(a, 16) for a in re.findall(r"\b0x(1388)u?\b", port)}
    out = []
    for key, p in programs.items():
        unit = "VU0" if p.get("unit", "").lower() == "vu0" or key.upper() == "VU0" else "VU1"
        entries = sorted({int(e, 16) for e in p.get("entries", [])} | ({0x1388} if key == "B" and hand else set()))
        for i, e in enumerate(entries):
            nxt = entries[i + 1] if i + 1 < len(entries) else e + 8 * max(1, p.get("pairs", 0) - e // 8)
            how = "hand-written C++ (runtime/sc3_vu1_port.cpp)" if key == "B" and e in hand else \
                f"lifted to C++ by tools/vu1_lift.py ({p.get('file', '')}); keeps the microcode's VU register model"
            out.append({"program": key, "unit": unit, "entry": e, "size": max(8, nxt - e), "how": how})
    return out


def phys(addr):
    if 0xB0000000 <= addr < 0xB3000000:
        return addr & 0x1FFFFFFF
    return addr


def hw_region(addr):
    a = phys(addr)
    for lo, hi, kind, label in HW_REGIONS:
        if lo <= a < hi:
            return kind, label, a
    return None


def describe_hw(addr, load):
    region = hw_region(addr)
    if not region:
        return None
    kind, label, a = region
    detail = label
    if kind == "dma":
        base = max(c for c, _ in DMA_CHANNELS if c <= a)
        name = dict(DMA_CHANNELS)[base]
        reg = DMA_REGS.get(a - base, f"+0x{a - base:x}")
        detail = f"{name} {reg}"
        if reg == "CHCR":
            detail += " (poll)" if load else " (kick)"
    elif a in POLL_REGS:
        detail = POLL_REGS[a] + (" (poll)" if load else "")
    return kind, f"{'read' if load else 'write'} {detail} @0x{a:08x}"


LOADS = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x37, 0x1E, 0x1A, 0x1B, 0x31, 0x35, 0x36}
STORES = {0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x3F, 0x1F, 0x2C, 0x2D, 0x39, 0x3D, 0x3E}
GPR_LOADS = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x37, 0x1E, 0x1A, 0x1B}
CALL_CLOBBER = set(range(1, 16)) | {24, 25, 31}


def tag_like(value):
    top = value >> 24
    if top in (0x14, 0x15, 0x17) and value & 0x00FF0000 == 0:
        return f"VIF MSCAL-family code 0x{value:08x}"
    if top in (0x50, 0x51) and value & 0x00FF0000 == 0 and value & 0xFFFF:
        return f"VIF DIRECT code 0x{value:08x}"
    if 0x60 <= top <= 0x7F and top != 0x70 and value & 0x0000FFFF and not value & 0x3C00 and value & 0x00FF0000:
        return f"VIF UNPACK code 0x{value:08x}"
    return None


def analyze(words, start, mmio, starts):
    """Static scan of one function's MIPS: hardware accesses, VU control, calls, syscalls."""
    regs = {0: 0}
    touches = []
    materialized = {}
    deref = set()
    callees, tails = set(), set()
    syscalls = []
    jalr = 0
    pending_clobber = 0
    for i, w in enumerate(words):
        pc = start + i * 4
        op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        imm = w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        funct = w & 0x3F
        written = None
        value = None
        if op == 0x0F:
            written, value = rt, (imm << 16) & 0xFFFFFFFF
        elif op in (0x09, 0x08, 0x19, 0x18):
            written = rt
            if rs in regs:
                value = (regs[rs] + simm) & 0xFFFFFFFF
        elif op == 0x0D:
            written = rt
            if rs in regs:
                value = regs[rs] | imm
        elif op in (0x0C, 0x0E, 0x0A, 0x0B, 0x1C, 0x1D):
            written = rt
        elif op == 0x00:
            if funct in (0x21, 0x25, 0x2D, 0x20, 0x2C) and rd:
                written = rd
                if rt == 0 and rs in regs:
                    value = regs[rs]
                elif rs == 0 and rt in regs:
                    value = regs[rt]
            elif funct == 0x09:
                jalr += 1
                pending_clobber = 2
                written = rd
            elif funct == 0x08:
                if rs != 31:
                    jalr += 1
            elif funct == 0x0C:
                number = regs.get(3)
                syscalls.append((pc, None if number is None else (number if number < 0x80000000 else number - 0x100000000)))
            elif funct in (0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x11, 0x13):
                pass
            elif rd:
                written = rd
        elif op == 0x03:
            callees.add(((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2))
            pending_clobber = 2
        elif op == 0x02:
            target = ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if target in starts and target != start:
                tails.add(target)
        elif op == 0x12:
            if rs & 0x10:
                if funct in (0x38, 0x39):
                    touches.append(("vu0micro", True, pc, ("VCALLMS" if funct == 0x38 else "VCALLMSR") +
                                    (f" 0x{((w >> 6) & 0x7FFF) * 8:x}" if funct == 0x38 else " (CMSAR0)")))
            elif rs == 0x02 and rd in VU_CONTROL:
                touches.append(("vuctl", True, pc, f"CFC2 {VU_CONTROL[rd]}" + (" (poll)" if rd == 29 else "")))
                written = rt
            elif rs == 0x06 and rd in VU_CONTROL:
                what = {31: " (starts VU1)", 28: " (VU reset)", 27: " (VU0 start address)"}.get(rd, "")
                touches.append(("vuctl", True, pc, f"CTC2 {VU_CONTROL[rd]}{what}"))
            elif rs in (0x01, 0x02):
                written = rt
            elif rs == 0x08:
                touches.append(("vu0micro", True, pc, "BC2x (waits on VU0)"))
        elif op in (0x10, 0x11):
            if rs in (0x00, 0x01, 0x02):
                written = rt
        if op in LOADS or op in STORES:
            load = op in LOADS
            hit = describe_hw((regs[rs] + simm) & 0xFFFFFFFF, load) if rs in regs else None
            if hit:
                touches.append((hit[0], True, pc, hit[1]))
                deref.add(regs[rs])
            elif pc in mmio:
                hit = describe_hw(mmio[pc], load)
                if hit:
                    touches.append(("mmio", True, pc, hit[1] + " (sc3.toml [mmio])"))
            if not load and rt in regs and rt:
                tag = tag_like(regs[rt])
                if tag:
                    touches.append(("tag", False, pc, f"stores {tag}"))
            if op in GPR_LOADS:
                written = rt
        if written:
            if value is None:
                regs.pop(written, None)
            else:
                regs[written] = value
                if hw_region(value) and op in (0x0F, 0x0D, 0x09, 0x19):
                    materialized.setdefault(value & 0xFFFF0000, (pc, value))
        if pending_clobber:
            pending_clobber -= 1
            if pending_clobber == 0:
                for r in CALL_CLOBBER:
                    regs.pop(r, None)
    used = {v & 0xFFFF0000 for v in deref}
    for high, (pc, value) in materialized.items():
        if value & 0xFFFF == 0 and hw_region(value)[0] in ("timer", "eeio", "vumem"):
            continue
        if high not in used and not any(t[0] not in ("addr", "tag", "vu0micro", "vuctl") for t in touches):
            region = hw_region(value)
            touches.append(("addr", False, pc, f"builds {region[1]} address 0x{phys(value):08x} without using it here"))
    return {"touches": touches, "callees": callees | tails, "syscalls": syscalls, "jalr": jalr}


GAME_CHUNK = 0x80000


def assign_libraries(funcs):
    named = [(i, f) for i, f in enumerate(funcs) if f["name"]]
    lib_of = {}
    for i, f in named:
        if f.get("syscall"):
            lib_of[i] = "libkernel"
            continue
        for label, rx in LIBRARIES:
            if re.search(rx, f["name"]):
                lib_of[i] = label
                break
    region = ["" for _ in funcs]
    keys = sorted(lib_of)
    runs = []
    for k in keys:
        if runs and lib_of[runs[-1][-1]] == lib_of[k]:
            runs[-1].append(k)
        else:
            runs.append([k])
    keys = [k for run in runs if len(run) > 1 for k in run]
    for a, b in zip(keys, keys[1:] + [None]):
        region[a] = lib_of[a]
        if b is not None and lib_of[a] == lib_of[b] and funcs[b]["addr"] - funcs[a]["addr"] < 0x20000:
            for k in range(a + 1, b):
                region[k] = lib_of[a]
    first_lib = keys[0] if keys else len(funcs)
    big = [k for k in keys if funcs[k]["addr"] >= 0x3E0000]
    game_end = big[0] if big else first_lib
    for k in range(len(funcs)):
        if not region[k]:
            region[k] = "game code" if k < game_end else "unnamed library code (CRI middleware and others)"
    for k, f in enumerate(funcs):
        if region[k] == "game code":
            lo = f["addr"] & ~(GAME_CHUNK - 1)
            region[k] = f"game code {hx(lo)}-{hx(lo + GAME_CHUNK - 1)}"
    return region


def irx_modules(generated):
    out = []
    iop = Path(generated) / "iop" if generated else None
    if not iop or not iop.is_dir():
        return out
    for path in sorted(iop.glob("*_iop.cpp")):
        text = read_text(path)
        reg = re.search(r'Registration kModule\("([^"]+)",\s*0x([0-9a-f]+)ull,\s*0x([0-9a-f]+)u,\s*0x([0-9a-f]+)u', text)
        if not reg:
            continue
        name = reg.group(1).split(".")[0]
        base, size = int(reg.group(3), 16), int(reg.group(4), 16)
        defs = list(re.finditer(r"^void (iop_[0-9a-f]+_([0-9a-f]+))\(Env &e\)\s*\{", text, re.M))
        funcs = []
        for k, m in enumerate(defs):
            body = text[m.end():defs[k + 1].start() if k + 1 < len(defs) else len(text)]
            addr = int(m.group(2), 16)
            interp = re.findall(r"// (0x[0-9a-f]{8}): the interpreter runs it", body)
            funcs.append({"addr": addr, "interp": len(interp), "words": sorted(set(interp))[:4]})
        funcs.sort(key=lambda f: f["addr"])
        for k, f in enumerate(funcs):
            f["size"] = (funcs[k + 1]["addr"] if k + 1 < len(funcs) else base + size) - f["addr"]
        out.append({"module": name, "base": base, "size": size, "funcs": funcs})
    return out


def irx_lists(build):
    """tools/iop_recomp.py --list outputs (build/iop_<MODULE>_list.txt from scripts/iop_recomp.sh), if present."""
    out = {}
    for path in sorted(build.glob("iop_*_list.txt")) if build.is_dir() else []:
        module = path.stem[4:-5].upper()
        for m in re.finditer(r"function (0x[0-9a-f]+): (\d+) instructions, interpreted \(([^)]*)\)", read_text(path)):
            out.setdefault(module, []).append((int(m.group(1), 16), int(m.group(2)) * 4, m.group(3)))
    return out


def iop_hle_services(ps2recomp):
    names = {}
    mods = Path(ps2recomp) / "ps2xIOP" / "src" / "modules"
    for path in sorted(mods.glob("*.cpp")) if mods.is_dir() else []:
        text = read_text(path)
        aliases = re.findall(r'"([A-Za-z0-9_]+)"', " ".join(re.findall(r"m?_?[mM]oduleAliases\s*=\s*\{([^}]*)\}", text)))
        aliases += re.findall(r'name\(\) const override\s*\{\s*return "([^"]+)"', text)
        for a in aliases or [path.stem]:
            names[a.upper()] = path.name
    return names


def runtime_rows(root, ps2recomp):
    over = read_text(Path(root) / "runtime" / "sc3_overrides.cpp")
    native = read_text(Path(root) / "runtime" / "sc3_native.cpp")
    vu0 = read_text(Path(root) / "runtime" / "sc3_vu0.cpp")
    lib = Path(ps2recomp) / "ps2xRuntime" / "src" / "lib"
    iop = Path(ps2recomp) / "ps2xIOP" / "src" / "emulator"
    rows = []

    def row(name, status, evidence):
        rows.append({"name": name, "status": status, "evidence": evidence})

    if "requestNativeGsRenderer" in over or (lib / "gs" / "gs_frontend.cpp").exists():
        row("GS front end", "present", "requestNativeGsRenderer() in sc3_overrides.cpp: GSNativeBackend draws the GS stream "
            "and keeps GS memory; 2D, HUD, post passes and the VU1 programs' render state still go through gs_frontend.cpp")
    else:
        row("GS front end", "absent", "no GS renderer requested")
    if "setSpu2Mixer" in over or (lib / "ps2_audio_spu2.cpp").exists():
        row("SPU2", "present", "ps2_audio_spu2.cpp mixes the voices the game's LIBSD/SDRDRV drive; SC3 adds movie audio to it")
    else:
        row("SPU2", "absent", "no SPU2 model found")
    if "Vu1StreamAdapter" in native or "setVif1Threaded(true)" in over:
        row("VIF1 reads", "present", "Vu1StreamAdapter (sc3_native.cpp) walks the DMA chains and unpacks every VIF1 stream "
            "into VU memory before the C++ programs read it; setVif1Threaded(true) in sc3_overrides.cpp")
    else:
        row("VIF1 reads", "absent", "no VIF1 stream reader installed")
    port_on = re.search(r"parsePort\(port\)\s*:\s*Port::On", native) is not None
    vu0_ported = "Mode::Interpreter" in vu0 and "*mode == '0'" in vu0
    if port_on and vu0_ported:
        row("VU interpreter", "diagnostic", "off by default: VU1 programs and VU0 microprograms run as C++ ports "
            "(SC3_NATIVE_VU1_PORT and SC3_NATIVE_VU0 default on); =0 brings the interpreter back as a diagnostic")
    else:
        row("VU interpreter", "present", "a VU program still runs on the interpreter by default")
    frames = re.search(r'getenv\("SC3_NATIVE_FRAMES"\);\s*!native', over) is not None
    if frames:
        row("vblank pacing", "partial", "frames paced by FramePacer (SC3_NATIVE_FRAMES default on), but the vblank and "
            "GS interrupts are still raised on EE time and the game's handlers run on them")
    else:
        row("vblank pacing", "present", "frames end on the emulated vblank")
    if (iop / "iop_emulator.cpp").exists():
        native_iop = 'setenv("PS2X_IOP_NATIVE", "1"' in over
        row("IOP kernel model", "present", "the IRX modules run on ps2xIOP's emulated IOP kernel (imports, threads, "
            "interrupts, DMA)" + (", recompiled to native code (PS2X_IOP_NATIVE=1)" if native_iop else ", interpreted"))
    else:
        row("IOP kernel model", "absent", "no IOP emulator found")
    return rows


def backing_text(c):
    if c["backing"] == "emulation":
        return "emulation-backed: " + c["evidence"]
    if c["backing"] == "emulation?":
        return "possibly emulation-backed (uncertain): " + c["evidence"]
    if c["backing"] == "native":
        return "native: " + c["evidence"]
    return c["evidence"]


def backing_flags(c):
    if c["backing"] == "native":
        return 0
    return FLAG_EMU_STUB | (FLAG_UNCERTAIN if c["backing"] != "emulation" else 0)


def classify(funcs, elf, config, handlers, sysnames, overrides, coverage):
    starts = {f["addr"] for f in funcs}
    by_addr = {f["addr"]: f for f in funcs}
    for f in funcs:
        f["flags"] = 0
        f["calls"] = []
        if coverage is not None and f["addr"] in coverage:
            f["flags"] |= FLAG_RAN
        if f["addr"] in overrides:
            o = overrides[f["addr"]]
            f["state"] = NATIVE
            f["name"] = f["name"] or o["label"]
            f["reason"] = f"game override {o['handler']} (runtime/{o['file']})" + \
                ("; wraps the original, which still runs on some paths" if o["wrapper"] else "")
            if o["wrapper"]:
                f["flags"] |= FLAG_WRAPPER
            continue
        if f["kind"] == "stub":
            c = handlers.classify(f["handler"])
            f["state"] = STUB
            f["name"] = f["name"] or f["handler"]
            f["backing"] = c["backing"]
            f["reason"] = f"{f['ns']}::{f['handler']} ({c['file'] or 'source not found'}): " + backing_text(c)
            f["flags"] |= backing_flags(c)
            continue
        words = elf.words(f["addr"], f["end"])
        a = analyze(words, f["addr"], config["mmio"], starts)
        f["analysis"] = a
        f["calls"] = sorted(c for c in a["callees"] if c in starts and c != f["addr"])
        if a["syscalls"] and len(words) <= 8 and all(n is not None for _, n in a["syscalls"]):
            number = a["syscalls"][0][1]
            name = sysnames.get(abs(number), f"syscall 0x{abs(number):02x}")
            c = handlers.classify(name) if not name.startswith(("inline", "syscall")) else \
                {"backing": "native", "evidence": "handled inline by the dispatcher", "file": "Syscalls/Dispatcher.cpp"}
            f["state"] = STUB
            f["name"] = f["name"] if f["name"] and not f["name"].startswith("entry_") else \
                name if not name.startswith(("inline", "syscall")) else f"syscall {number:#x}"
            f["syscall"] = True
            f["backing"] = c["backing"]
            f["reason"] = f"kernel syscall {number:#x} -> ps2_syscalls::{name} ({c['file']}): " + backing_text(c)
            f["flags"] |= backing_flags(c)
            continue
        certain = [t for t in a["touches"] if t[1]]
        unsure = [t for t in a["touches"] if not t[1]]
        if certain or unsure:
            f["state"] = DIRECT
            picked = (certain or unsure)
            kinds = Counter(t[0] for t in picked)
            f["kinds"] = sorted(kinds)
            f["reason"] = "; ".join(f"{t[3]} at 0x{t[2]:06x}" for t in picked[:4]) + \
                (f"; +{len(picked) - 4} more" if len(picked) > 4 else "")
            if not certain:
                f["flags"] |= FLAG_UNCERTAIN
                f["reason"] = "uncertain: " + f["reason"]
        else:
            f["state"] = CLEAN
            f["reason"] = "no hardware access, VU control or emulation-backed call found"
        if a["jalr"]:
            f["reason"] += f"; {a['jalr']} indirect call(s)/jump(s) not followed"
    touch = {f["addr"] for f in funcs if (f["state"] == DIRECT or (f["state"] == STUB and f["flags"] & FLAG_EMU_STUB))
             and not f["flags"] & FLAG_UNCERTAIN}
    depth1 = {}
    for f in funcs:
        if f["state"] != CLEAN:
            continue
        hit = [c for c in f["calls"] if c in touch]
        if hit:
            depth1[f["addr"]] = hit
    depth2 = {}
    for f in funcs:
        if f["state"] != CLEAN or f["addr"] in depth1:
            continue
        hit = [c for c in f["calls"] if c in depth1]
        if hit:
            depth2[f["addr"]] = hit

    def via(addr, seen=()):
        g = by_addr[addr]
        if g["state"] == STUB:
            return {"stub"}
        if g["state"] == DIRECT:
            return {"vu0" if set(g.get("kinds", [])) <= {"vu0micro"} else "hardware"}
        out = set()
        for c in depth1.get(addr, []):
            out |= via(c)
        return out

    def label(addr):
        g = by_addr[addr]
        return f"{g['name'] or 'sub'}@{hx(addr)}"

    for f in funcs:
        if f["addr"] in depth1:
            hit = depth1[f["addr"]]
            f["state"] = INDIRECT
            f["depth"] = 1
            f["reason"] = "calls " + ", ".join(
                label(c) + (" (emulation-backed stub)" if by_addr[c]["state"] == STUB else "") for c in hit[:3]) + \
                (f" +{len(hit) - 3} more" if len(hit) > 3 else "")
        elif f["addr"] in depth2:
            hit = depth2[f["addr"]]
            f["state"] = INDIRECT
            f["depth"] = 2
            f["reason"] = "calls " + ", ".join(f"{label(c)} -> {label(depth1[c][0])}" for c in hit[:2]) + \
                (f" +{len(hit) - 2} more" if len(hit) > 2 else "")
        if f["state"] == INDIRECT:
            f["via"] = sorted(set().union(*(via(c) for c in (depth1.get(f["addr"]) or depth2[f["addr"]]))))
    return funcs


def write_outputs(out_dir, history_path, payload, funcs):
    out_dir.mkdir(parents=True, exist_ok=True)
    counts = payload["counts"]
    history = []
    if history_path.exists():
        try:
            history = json.loads(history_path.read_text(encoding="utf-8"))
        except ValueError:
            history = []
    point = {"date": payload["meta"]["generated"], "sc3": payload["meta"]["sc3_commit"],
             "ps2recomp": payload["meta"]["ps2recomp_commit"], "tool": TOOL_VERSION, "counts": counts}
    same = lambda h: (h.get("sc3"), h.get("ps2recomp"), h.get("tool"), h.get("counts")) == \
        (point["sc3"], point["ps2recomp"], point["tool"], point["counts"])
    if history and same(history[-1]):
        history[-1]["date"] = point["date"]
    else:
        history.append(point)
    history_path.write_text(json.dumps(history, indent=1) + "\n", encoding="utf-8")
    payload["history"] = history
    (out_dir / "map.json").write_text(json.dumps(payload, indent=1) + "\n", encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--elf", help="game ELF (default: $SC3_ELF, then game/SLUS_212.16)")
    ap.add_argument("--generated", help="generated tree from scripts/recomp.sh (default: generated/)")
    ap.add_argument("--config", default=str(ROOT / "recomp" / "sc3.toml"))
    ap.add_argument("--ps2recomp", default=str(ROOT / "PS2Recomp"), help="PS2Recomp checkout (default: the submodule)")
    ap.add_argument("--out", default=str(ROOT / "build" / "funcmap"))
    ap.add_argument("--history", help="history file to append to (default: <out>/history.json)")
    ap.add_argument("--irx-lists", default=str(ROOT / "build"), help="directory of iop_<MODULE>_list.txt files")
    ap.add_argument("--coverage", help="text file of function addresses that ran (hex, one per line)")
    args = ap.parse_args()

    elf_path = args.elf or os.environ.get("SC3_ELF") or str(ROOT / "game" / "SLUS_212.16")
    if not Path(elf_path).is_file():
        raise SystemExit(f"missing ELF {elf_path} (scripts/extract_iso.sh, or pass --elf / set SC3_ELF)")
    generated = args.generated or str(ROOT / "generated")
    if not Path(generated).is_dir():
        print(f"no generated tree at {generated}: falling back to recomp/ghidra_map.csv", file=sys.stderr)
        generated = None
    ps2recomp = Path(args.ps2recomp)
    if not (ps2recomp / "ps2xRuntime").is_dir():
        print(f"no PS2Recomp sources at {ps2recomp} (git submodule update --init, or --ps2recomp): stubs unclassified",
              file=sys.stderr)

    elf = Elf(elf_path)
    config = load_config(args.config)
    funcs, source = load_functions(generated, config, elf)
    handlers = HandlerIndex(ps2recomp)
    sysnames = syscall_table(ps2recomp)
    overrides = load_overrides(ROOT / "runtime")
    coverage = None
    if args.coverage:
        coverage = {int(tok, 16) for tok in re.findall(r"(?:0x)?([0-9a-fA-F]{5,8})\b", read_text(args.coverage))}
    classify(funcs, elf, config, handlers, sysnames, overrides, coverage)
    regions = assign_libraries(funcs)
    vu = load_vu_programs(ROOT)

    records = []
    for f, region in zip(funcs, regions):
        records.append({"addr": f["addr"], "size": f["end"] - f["addr"], "name": f["name"], "state": f["state"],
                        "reason": f["reason"], "flags": f["flags"], "region": f"{f['section']} / {region}",
                        "kinds": f.get("kinds", []), "calls": [hx(c) for c in f["calls"][:16]],
                        "depth": f.get("depth", 0), "backing": f.get("backing", ""), "via": f.get("via", [])})
    for p in vu:
        records.append({"addr": p["entry"], "size": p["size"], "name": f"{p['unit']} program {p['program']}:0x{p['entry']:x}",
                        "state": NATIVE, "reason": p["how"], "flags": 0,
                        "region": f"VU microprograms / {p['unit']} program {p['program']}", "kinds": [], "calls": [],
                        "depth": 0, "backing": "", "via": []})

    ee = [r for r in records if not r["region"].startswith("VU ")]
    counts = Counter(STATES[r["state"]][0] for r in ee)
    counts = {k: counts.get(k, 0) for k, _, _ in STATES}
    counts["total"] = len(ee)
    counts["uncertain"] = sum(1 for r in ee if r["state"] == DIRECT and r["flags"] & FLAG_UNCERTAIN)
    counts["stub_emulation"] = sum(1 for r in ee if r["state"] == STUB and r["flags"] & FLAG_EMU_STUB)
    counts["stub_native"] = counts["stub"] - counts["stub_emulation"]
    counts["indirect_depth1"] = sum(1 for r in ee if r["depth"] == 1)
    counts["vu_programs_native"] = len(vu)
    counts["indirect_via_vu0_only"] = sum(1 for r in ee if r["state"] == INDIRECT and r["via"] == ["vu0"])
    free = counts["native"] + counts["clean"] + counts["stub_native"]
    kinds = Counter(k for r in ee if r["state"] == DIRECT for k in r["kinds"])

    irx = irx_modules(generated)
    hle = iop_hle_services(ps2recomp)
    listed = irx_lists(Path(args.irx_lists))
    irx_counts = Counter()
    for m in irx:
        provider = hle.get(m["module"].upper(), "")
        m["hle_provider"] = provider
        m["functions"] = []
        seen = set()
        for g in m.pop("funcs"):
            seen.add(g["addr"])
            breaks = all((int(word, 16) & 0xFC00003F) == 0x0D for word in g["words"])
            if g["interp"] and not breaks:
                state, reason = 1, f"{g['interp']} instruction(s) left to the interpreter: " + ", ".join(g["words"])
            else:
                state = 0
                reason = "recompiled by tools/iop_recomp.py; runs on the emulated IOP kernel"
                if g["interp"]:
                    reason += f"; {g['interp']} BREAK trap(s) left to the interpreter (gcc's divide-by-zero check)"
            if provider:
                reason += f"; ps2xIOP {provider} exists only as a fallback when the IRX is missing"
            m["functions"].append({"addr": g["addr"], "size": g["size"], "state": state, "reason": reason})
        for addr, size, why in listed.get(m["module"].upper(), []):
            if addr not in seen:
                m["functions"].append({"addr": addr, "size": size, "state": 1,
                                       "reason": f"left to the interpreter by tools/iop_recomp.py ({why})"})
        m["functions"].sort(key=lambda g: g["addr"])
        for g in m["functions"]:
            irx_counts[("recompiled", "interpreted", "overridden")[g["state"]]] += 1
    sc3_commit = git(ROOT, "rev-parse", "--short=10", "HEAD")
    pinned = git(ROOT, "ls-tree", "HEAD", "PS2Recomp").split()
    meta = {
        "game": "SLUS_212.16", "title": "Soulcalibur III (SLUS_212.16)", "page_title": "SC3 Native Map",
        "generated": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "sc3_commit": sc3_commit, "sc3_branch": git(ROOT, "rev-parse", "--abbrev-ref", "HEAD"),
        "ps2recomp_commit": pinned[2][:10] if len(pinned) > 2 else git(ps2recomp, "rev-parse", "--short=10", "HEAD"),
        "function_source": source, "tool_version": TOOL_VERSION,
        "emulation_free_pct": 100.0 * free / max(1, counts["total"]),
        "coverage": bool(coverage is not None),
        "sections": [{"name": s["name"], "addr": s["addr"], "size": s["size"]} for s in elf.sections if s["exec"]],
    }
    meta["subtitle"] = f"one square per function, address order; SC3 {sc3_commit or '?'}, PS2Recomp " \
                       f"{meta['ps2recomp_commit'] or '?'}, {meta['generated'][:10]}"
    payload = {"meta": meta, "counts": counts, "kinds": {TOUCH_KINDS.get(k, k): n for k, n in kinds.most_common()},
               "runtime": runtime_rows(ROOT, ps2recomp), "heuristics": HEURISTICS, "functions": records,
               "irx": irx, "irx_counts": {k: irx_counts.get(k, 0) for k in ("recompiled", "interpreted", "overridden")},
               "irx_hle_providers": hle}
    out_dir = Path(args.out)
    history_path = Path(args.history) if args.history else out_dir / "history.json"
    write_outputs(out_dir, history_path, payload, [f for f in funcs])
    print(f"{counts['total']} EE functions: native {counts['native']}, clean {counts['clean']}, direct {counts['direct']} "
          f"({counts['uncertain']} uncertain), indirect {counts['indirect']}, stubs {counts['stub']} "
          f"({counts['stub_emulation']} emulation-backed); {len(vu)} VU program entries native; "
          f"IRX {dict(payload['irx_counts'])}")
    print(f"wrote {out_dir / 'map.json'}, {history_path}")


HEURISTICS = {
    "rules": [
        ["Function list", "The function boundaries ps2_recomp used: the // Function / // Address header of every file "
         "in the generated tree. Files whose body only calls ps2_stubs:: or ps2_syscalls:: are the stubs. Names come "
         "from sc3.toml's stubs and entry_points and from the game override's constants. Without a generated tree the "
         "tool falls back to recomp/ghidra_map.csv."],
        ["Native C++", "A registerFunction() call in runtime/*.cpp that is not behind an environment-variable "
         "diagnostic (if (std::getenv(...)) blocks are skipped, 'unset or not 0' defaults are kept). Overrides that "
         "call the original back on some paths are flagged as wrappers. The VU microprogram entries come from "
         "recomp/vu1_lift_spec.json, plus B:0x1388's hand-written port."],
        ["Touches emulation (direct)", "A linear pass over the function's MIPS that tracks the constants built by "
         "LUI/ORI/ADDIU/moves and drops caller-saved registers after each call. A load or store whose address lands "
         "in EE I/O (0x10000000-0x1000FFFF: timers, IPU, GIF, VIF0/1, FIFOs, DMA channels, DMAC, INTC, SIF), VU memory "
         "(0x11000000-0x1100FFFF) or the GS privileged registers (0x12000000-0x12001FFF), also through KSEG1, counts. "
         "A CHCR read is a poll, a CHCR write a kick; D_STAT, GS CSR, I_STAT and SIF SMFLG reads are polls. VU: "
         "VCALLMS/VCALLMSR, BC2x and CFC2/CTC2 of CMSAR0, FBRST, VPU-STAT and CMSAR1. sc3.toml's [mmio] table "
         "(addresses the analyzer resolved) is checked as a second source: every one of its accesses is also found "
         "by the scan."],
        ["Uncertain", "Shown hollow. A store of a constant shaped like a VIF code (MSCAL, MSCALF, MSCNT, DIRECT, "
         "DIRECTHL, UNPACK with a plausible NUM and ADDR), or a hardware register address built but not "
         "dereferenced in the function (passed to a callee or stored). Uncertain hits never propagate to callers."],
        ["Calls emulation (indirect)", "Depth 1: a JAL or tail J to a function that touches emulation for certain, "
         "or to a stub whose handler is emulation-backed for certain. Depth 2: a call to a depth-1 function. "
         "Nothing propagates through a native override. 'Via' says what is at the bottom: hardware access, VU0 micro "
         "mode only, or an emulation-backed stub."],
        ["Stub backing", "The handler's body in PS2Recomp's ps2xRuntime/src/lib/Kernel, plus the helpers it calls "
         "directly, is searched for references to an emulated subsystem: I/O register addresses, the DMAC, GIF, VIF, "
         "GS memory or registers, VU1, the IOP and SIF, the SPU2, the interrupt model. A handler that only waits on "
         "the emulated vblank is 'possibly emulation-backed' (uncertain)."],
        ["Syscall wrappers", "A function of at most 8 instructions with a SYSCALL whose $v1 is a known constant is "
         "the kernel's wrapper: it is classed as a stub of the handler PS2Recomp's Dispatcher.cpp maps that number "
         "to (negative numbers are the interrupt-context variants)."],
        ["IRX modules", "The functions in generated/iop/*_iop.cpp (tools/iop_recomp.py). BREAK traps (gcc's "
         "divide-by-zero check) left to the interpreter do not count as a fallback; any other instruction does, and "
         "so does a function the --list output marks as interpreted. The ps2xIOP HLE services (libsd, mcserv, "
         "dbcman) only answer when the game's IRX is missing, so no export of SC3's modules is overridden."],
    ],
    "blind": [
        "Indirect calls are not followed: JALR, jump tables, function pointers, callbacks and interrupt handlers. "
        "Each function's details give how many it has.",
        "The constant tracking ignores control flow and memory: a hardware address loaded from a table or a global "
        "(libdma keeps channel pointers in data) is missed, and a register reused on two paths can be wrong.",
        "GIF tags and DMA tags are 64- and 128-bit values built with DSLL32, OR and PCPYLD: they are not recognised. "
        "Only some VIF codes are, and only when stored as a 32-bit constant.",
        "Functions that only fill buffers which the emulated DMA, VIF or GS read later (packet builders) look clean "
        "unless they store a recognised VIF code. Their data still feeds the emulation.",
        "The stub classification is lexical: a handler that reaches emulation through a deeper helper, or names a "
        "subsystem it does not use, is misjudged.",
        "Native C++ means the game's code no longer runs there, not that the replacement is emulation-free: the "
        "lifted VU programs keep VU registers, flags and VU memory; the movie overrides answer CRI's player on the host.",
        "Runtime-level emulation (the GS front end, the SPU2, the IOP kernel) is not tied to any one function: see the "
        "badges at the top.",
    ],
}


if __name__ == "__main__":
    main()
