# 05 — Debugging recipes

## Reading a crash report

```
[dm] CRASH sig=11 addr=0 pc=main+0x373e04 lr=main+0x373dd8 sp=0x7066dd6370
x0=… x28=…
[dm] bt: main+0x373dd8 main+0x2ea2ac …          frame-pointer backtrace
[dm] fp=…  sp 下方 0x100 字节: …                 the 32 words below sp (the frame that was just popped)
[dm] 栈扫描 sp=…: [-72]main+0x57a14 [+24]main+0x57618 …   every word near sp that resolves to a module or host symbol
[dm] 线程 0x1001 「主线程」…                       all guest threads, what they wait on, their backtraces
```

- `pc` and `lr` are module-relative (`main+…`, `sdk+…`) or host symbols. Module offsets are the same as in the decompressed NSO and the ELF (Ghidra adds 0x100000).
- Look at the registers before the code: a pointer that is a valid pointer plus garbage high bits, an index of `0xfffffffe`, or `pc == lr == 0x100000000` each tell a different story. a fault at `0x74a740e114 = 0x70a740c570 + 0xfffffffe·4 + 0x1bac` was an index of −2 widened as unsigned; `pc == lr` on a nonsense value is a `ret` through an overwritten saved LR (look at the zeroed frame below `sp`).
- When the frame-pointer backtrace is empty, the stack scan's negative offsets (below `sp`) show the return addresses of the calls the crashing function made last.

## Ghidra / objdump

```bash
# decompile the functions containing these module offsets
JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home \
  /opt/homebrew/opt/ghidra/libexec/support/analyzeHeadless ~/<title>-work/ghidra/proj <proj> -process <module>.elf \
  -noanalysis -readOnly -scriptPath <repo>/dm/tools/ghidra -postScript Decompile.java 0x373e04,0x2ea2ac

# raw disassembly (works where Ghidra defined no function)
objdump -d --start-address=0x2ea1c0 --stop-address=0x2ea2b0 ~/<title>-work/elf/<module>.elf
```

Prefix an offset with `@` (`... Decompile.java @0x49740`) to (re)create the function at exactly that address. Code reached only through vtables has no function, and Ghidra's analysis marked some `nn::os` imports (`LockMutex`) as non-returning, which cut many functions short right after the first lock. The script clears that flag and the call-return overrides on such calls before decompiling.

Map GOT slots to NVN names with the survey table (`census/*_nvn_slots.tsv`): a call through `[x8, #0x3e8]` after `adrp x8, 0x35ad000` is GOT entry 0x35ad3e8.

## Probes without a debugger (`DM_PROBE`)

`DM_PROBE=main:4290c0,main:9712c` (module:offset, comma-separated; `host/probe.c`, Mac only) replaces the instruction with `brk`, and on every hit prints the guest thread (`T=`), x0–x7 (and strings they point to), x19–x23, lr, x8 and a backtrace, then runs the original instruction from a trampoline. Put probes at function entries or where x16 is dead; the probed instruction must not be PC-relative (no `adrp`, `adr`, `b`, `bl`, literal `ldr`: probe the next instruction instead). Output from several threads interleaves; parse it with a regex rather than by line. Probes slow the game, which can shift heap addresses between runs.

Per-probe suffixes, in this order:

| Suffix | Effect |
|---|---|
| `@off`, `@*off`, `@xN+off`, `@*xN+off` | Print 16 bytes at x0+off (or xN+off); `*` dereferences that address first |
| `?xN=val`, `?[xN+off]=val` | Only report hits where the register (or the 64-bit word at xN+off) equals `val` (hex); others pass silently. E.g. `main:4743c0?[x0+30]=781548` = only allocations into the heap whose size field is 0x781548 |
| `!xN` | Treat xN as a `sead::ExpHeap` and print its free and used blocks (offset+size) |

`DM_PROBE_ALL=1` adds x9–x18; `DM_PROBE_X2=<bytes>` dumps the memory x2 points to.

Use it when lldb is unavailable: after macOS revokes the debugger authorization (`DevToolsSecurity -status` says developer mode is disabled), `lldb` launches hang waiting for a GUI approval prompt. `sudo DevToolsSecurity -enable` (run by the user) restores it.

## lldb

The host uses SIGSEGV for dirty-page tracking, so lldb must pass it through without stopping. Address randomization is off under lldb, so guest addresses repeat across runs. Template (`~/<title>-work/dbg/*.py`, run with `perl -e 'alarm 240; exec @ARGV' lldb -b -o "command script import x.py"`):

```python
import lldb
HOST = '/path/to/dm/host/dmhost'
def __lldb_init_module(dbg, _):
    dbg.SetAsync(False); ci = dbg.GetCommandInterpreter(); res = lldb.SBCommandReturnObject()
    ci.HandleCommand('settings set platform.plugin.darwin.ignored-exceptions EXC_BAD_ACCESS|EXC_BAD_INSTRUCTION', res)
    t = dbg.CreateTarget(HOST); t.BreakpointCreateByName('run_module_inits')
    li = lldb.SBLaunchInfo(['<patched dir>']); li.SetEnvironmentEntries(['DM_ROMFS=…', 'DM_SAVE=…', 'DM_TEMP=…'], True)
    p = t.Launch(li, lldb.SBError())
    for sig in ('SIGSEGV', 'SIGBUS', 'SIGUSR1', 'SIGUSR2', 'SIGPIPE'): ci.HandleCommand('process handle %s -n false -p true -s false' % sig, res)
    base = None
    while p.GetState() == lldb.eStateStopped:
        fr = p.GetSelectedThread().GetFrameAtIndex(0)
        if base is None and fr.GetFunctionName() == 'run_module_inits':
            base = t.EvaluateExpression('(unsigned long)g_mod[0].base').GetValueAsUnsigned()   # g_mod[0] = main
            t.BreakpointCreateByAddress(base + 0x2ea228)                                       # module offset
        elif base is not None and fr.GetPC() == base + 0x2ea228:
            print('x0=%#x' % fr.FindRegister('x0').GetValueAsUnsigned(), flush=True)          # flush: a timeout kill loses buffered output
        p.Continue()
    p.Kill()
```

Patterns that worked: print every call of one function with its arguments; stop when a return value is null and print the backtrace; trace the branch path through one function (one-shot breakpoints on every branch site). Hardware watchpoints on words touched by `ldxr`/`stxr` may stay silent.

## Memory growth

1. The host's memory report line (`内存 至帧 …`) every few hundred frames; `footprint -p <pid>` from outside.
2. `vmmap -summary <pid>` twice a few seconds apart: which region grows. "owned unmapped (graphics)" = private Metal resources.
3. `DM_NVN_ALLOCPROF=1`: growth per replayed op type and per function (`tex_mtl` / pass begin / pipelines / argument buffers), command allocator sizes, and a log of new textures with serial, size, format and storage. A stream of new serials with the same sizes = objects re-created every frame.

Instruments' Metal Resource Events didn't record this process from the command line (`xctrace record --launch`, with `--env` for the variables); the in-host probes were faster.

## Render-pass investigation

See [04](04-graphics.md) section 7. Notes: pass dumps are large and may contain non-UTF-8 bytes; filter with `LC_ALL=C grep -a "\[pass\] F600 " | grep -av " o cmd"`. A Python one-liner with PIL (the dm venv has it) gives mean colour and non-black share of each dumped PNG, which is quicker than opening 20 images.
