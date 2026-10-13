# 03 — Host bring-up

## The loop

```bash
DM_RULESET=native python3 dm/tools/patch_nso.py $W/data/v0/ExeFS/main $W/build/patched_v0 main   # and sdk
make -C dm/host dmhost
~/<title>-work/run/boot.sh <log name> [seconds] [VAR=value ...]
```

`boot.sh` runs `dmhost <patched dir>` with `DM_ROMFS`, `DM_SAVE`, `DM_TEMP`, `DM_MSL_DIR` set, under a perl `alarm` (a run that hits the time limit ends with `CRASH sig=14` — that is the alarm, not a crash). Logs go to `run/<name>.log`. Logs may contain non-UTF-8 bytes: use `grep -a` / `LC_ALL=C` when filtering.

## Reading the log

| Marker | Meaning |
|---|---|
| `>>> nnosInitialize`, `>>> nnMain` | Boot stages reached |
| `【红】…` | Something unimplemented or wrong, printed once per kind. Collect them with `grep -a "【红】" log \| sort \| uniq -c` after every run |
| `UNIMPL <name>` | A forced NVN / SDK stub was called |
| `[nvn] 帧 N：… \| name=count …` | Per-frame NVN call counts (calls made on the frame thread; command buffers recorded elsewhere replay later) |
| `[长帧]` | A long frame, with submit / GPU / pipeline / upload breakdown |
| `内存 至帧 N：常驻 … 显卡已分配 …` | Memory report (RSS, Metal allocated size). Printed when memory moves; steady growth here is a leak. A watchdog exits at 24 GB |
| `CRASH sig=… pc=… lr=…` + registers + `bt:` + `栈扫描` + thread dump | Crash report: see [05](05-debugging.md) |
| `线程 0x10xx 「name」… 等:<state> addr=…` | Thread dump: what each guest thread waits on (`地址` = address arbiter, `锁` = mutex with holder, `条件变量`, `同步对象`, `运行/未在等` = running) |

Useful switches: `DM_FS=1` (file opens, and not-found probes), `DM_SVC=1` (memory SVCs and other syscalls), `DM_THR=1` (thread SVCs), `DM_WATCH=<s>` (dump all threads after s seconds), `DM_WATCH_STALL=<s>` (dump when the frame loop stops progressing).

## Failure catalogue

Each entry: symptom → root cause → fix (where the fix lives). Check these first; most new-title failures are one of them in a new form.

### Rewriter

- **Job system hangs; a queue claims thousands of tasks but every slot is empty** → in an `ldxr … cmp … b.ne out … stxr … cbnz` window, the rewritten stub emitted its exits in the wrong order, so successful stores took the failure path → emit the fall-through exit first (`patch_nso.py`). D2R had no such windows; Mario had 82.
- **SIGILL / trap on a system register read** → newer SDK reads a register macOS traps (`CTR_EL0` in SDK 16's `FlushDataCache`) → rewrite to a constant.

### Memory and address space

- **First `AllocateAddressRegion` fails with 0x1803** → SDK 16's AddressRegionManager takes 0x3fe00000 at the start of the alias region as a node area → the alias region must be larger than 1 GB.
- **Large resources silently skipped, later a null deref where the resource is used** → the game reserves "virtual address memory" (Mario: 1.75 GB in `HeapDef.byml`) and maps it into the alias region with `MapPhysicalMemory` on demand; the alias region left after the node area was too small. Also visible as `SetHeapSize` asking for much less than the pool. → repartition the 6 GB guest block (`hle_svc.c` `mem_init`): Mario uses alias 2.875 GB, stacks 256 MB, heap 2.625 GB. 6 GB is the most iOS reserves without the extended-virtual-addressing entitlement.
- **`MapPhysicalMemory` faults on part of a page** → round to 16 KB host pages.
- **Write through a null pointer at +0x1c0 during startup** → `GetInfo(UserExceptionContextAddress)` must return a real buffer.
- **Heap allocations fail late in a busy scene** → the reported application pool (3285 MB, `APP_POOL`) and used memory decide the game's heap size; report Switch-accurate numbers.

### Threads

- **Per-core job queues never drained** → a `SetThreadCoreMask` that pins a thread to other cores (ideal core −3 = keep) must move the ideal core into the new mask, as Horizon does, so `GetCurrentProcessorNumber` reports a core the thread can run on.
- **Crash indexing a per-core table with a huge offset (index −2)** → threads created with ideal core −2 (*use process default*) were recorded as core −2 and `GetCurrentProcessorNumber` returned −2 → resolve −2 to the process default core (0) and never return a negative core (`hle_thread.c`). Seen in Mario's font engine (`main+0x107dbc0`).

### Services and SDK

- **Abort inside `nn::sm::Initialize`** → an SDK path reached a system service the host doesn't replace → hook the public SDK entry point named in the backtrace (`HLE_FORCE` in the relevant `hle_*.c`).
- **Device / subsystem setup returns quietly and the next stage never starts** → a version or capability check failed (NVN API version, a feature query) → compare what the game queries with what the host answers.
- **Font engine runs on garbage** → the system shared font must look like BFTTF: the SDK calls `ScalableFontEngineHelper::Decode(ptr, size)`, which XOR-decodes in place unless the first word is the "already decoded" marker (LE `0x18029a7f`) → serve `marker + bswap(size ^ 0x49621806) + TTF` (`hle_sdk16.c`).
- **File-not-found noise** → games probe many optional paths (debug parameters, other-language voice files); with `DM_FS=1` these show as `fs … 不存在`. Only a missing file followed by a failure matters.

- **A buffer is created with a null pool right after a scene change** → NVN *virtual* memory pools (flag 0x200) have no storage until `nvnMemoryPoolMapVirtual` maps physical pool pages in; if the host returns failure, the game treats the pool as unusable. Mario maps its single 128 MB virtual pool onto one physical pool in one request, so the host aliases it (`hle_nvn.c`). True sparse mappings are still unimplemented.
- **A null-pool buffer even with virtual pools working** → the game's own heap ran out. Trace it with probes ([05](05-debugging.md)): the gsys GPU allocator at `main+0x4290c0` and the heap's `tryAlloc` (vtable slot +0x30) show which heap, its size and every allocation into it; the `!xN` heap walk shows the free and used blocks at the moment of failure.
  - Mario: each resource gets its own `sead::ExpHeap` sized *resource-size-table value + 0x1008* (`main+0x1e603c` → `ExpHeap::create` `main+0x4aac38`). The table values are Nintendo's measured peaks with almost no margin (Yoshi's animation file uses 99 % of its budget). `Model/NpcCommonerA.bfres` in the opening course came up about 0x70 bytes short: its last GPU chunk (0x9000 bytes, 4 KB aligned) fit, but the chunk's 0x598-byte descriptor did not. Every allocation size before it is fixed by game code and data, so the difference from hardware is still unknown.
  - Fix: a registered code cut gives every resource heap +16 KB (`CUTS` in `dm/tools/patch_nso.py`, keyed by the module build ID, original bytes verified). Per-title code patches go there, with the reason.
  - Read the probe log carefully: `tryAlloc` probes show *requests*, not successes, so a running total that already exceeds the heap means an earlier request failed.

- **`FATAL NVN 命令内存：游戏池已空…`** → our command records are larger than real NVN commands, so command buffers the game sizes for hardware overflow; when the game's out-of-memory callback declines to add memory, the host lends a segment. Many small command buffers each overflowing once per frame exhausted a fixed pool of 64 × 1 MB. The pool now grows on demand (256 KB segments, reused after 8 frames, abort only beyond 1 GB; `hostseg_take` in `hle_nvn.c`).
- **Frames stop, no crash, the main thread sits in `nvnSyncWait(sync, -1)`, and the watchdog's wait-state line shows the main fence one short (`已提交 N / 已完成 N-1`)** → a GPU command buffer never completes. Check GPU load from outside: `ioreg -r -d 1 -c IOAccelerator | grep 'Device Utilization'` at 100 % means a shader is looping forever. Find the shaders first used in the last frames (`DM_NVN_PASSDUMP`, compare the `K` keys) and look at their `.metal` for an empty `do {} while (true);` with no `return` (see [04](04-graphics.md) section 1: the compute program start).

- **Menus and dialogue boxes draw but contain no text; images and pictogram digits are fine** → the game rasterizes glyphs on the CPU into a *linear* texture in a CPU-visible pool (Mario: 2048×8192 R8, flags IMAGE|LINEAR). A private Metal texture never sees those writes. Linear textures in CPU-visible pools are now created on the pool buffer (`tex_linear` in `nvn_mtl.m`). `DM_NVN_TEXCPU=1` lists non-packaged textures in CPU-visible pools.
- **Controller-button icons in text show as `?`** → those glyphs come from the system shared font's Nintendo-extension set; the host serves a desktop font for every shared-font type. Open.

### NVN object sizes (memory corruption)

The game allocates NVN objects with the sizes of *its* SDK's headers, which shrank in newer SDKs. A host record larger than the game's allocation overwrites neighbouring memory:

- `NVNtexture` is 0x80 bytes in SDK 16 (heap corruption: the old 0x88 record overwrote the next heap block's links).
- `NVNtextureView` is 16 bytes in SDK 16 (stack smash: a 16-byte local next to the saved frame pointer; the function returned to `0x100000000`).

Check every host object against the new SDK: `_Static_assert(sizeof(host) <= N)` next to the struct, and when a crash returns to a nonsense address, look at the 0x100 bytes below `sp` in the crash report for a zeroed frame.

### Game state

- **"Stuck" at a screen with one draw per frame** → usually not stuck: most draws are in recorded command buffers replayed later; dump the passes ([04](04-graphics.md)) before assuming the game logic is blocked.
