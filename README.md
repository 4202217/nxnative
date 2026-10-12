# smbw-mobile

Work in progress: an iOS native-execution port of **Super Mario Bros. Wonder** (Switch, base game v0), built from the generic parts of [d2r-mobile](../d2r-mobile) (MIT). Bring your own game files; nothing from the game is in this repository.

Same approach as d2r-mobile: the game's own aarch64 code runs natively. A rewriter changes only the instructions that cannot run under Apple's kernel (x18, thread pointers, `svc`, timers). The host replaces Switch system services, and the NVN graphics API is reimplemented on Metal.

## Status (2026-10-12)

| Step | State |
|---|---|
| Rewriter (`dm/tools/patch_nso.py`, `DM_RULESET=native`) | Runs unchanged on Mario `main` / `sdk` |
| Mac host boot | Boots fully into the game loop: Metal device and 1920x1080 swapchain, about 40 game threads (scene, save data, module workers, UI loading, map units, task executors), silent audio renderer, first render commands. Runs indefinitely without crashing |
| Current frontier | Main thread waits on job-completion light events (`main+0x83dec`) that never fire. Most likely cause: render commands the host does not implement yet (`CallCommands`, `BindTexture`, `BindVertexBuffer`, `DrawElements`, `WriteTexels` / `FlushTexels`) |
| NVN | 534 functions fetched, 236 called. About 45 state-only functions added this round; the command-recording ones (binds, draws, queries, conditional render, ...) are still missing |
| Audio | Silent AudioRenderer (`hle_audren.c`): open / start / stop / update and device queries, event fired every 5 ms. Config editing (180 library imports) runs natively. Real mixing comes later |
| Shaders | `Shader/SecredUber...bfsha.zs` (BFSHA v9, plain zstd) needs a container reader feeding the existing translation chain |
| iOS bundle | Not started. Plan: reuse `dm/ios` from d2r-mobile, bundle the RomFS (3.5 GB) inside the app, standard-controller virtual gamepad |

## What changed relative to d2r-mobile

- Removed: all D2R gameplay / UI modules (`dm_*.c`), D2R diagnostics (`diag.c`), Blizzard TACT/CASC, Granny, movie, auto-targeting, and the archive validator. `host_stubs.c` holds the leftover hook points as no-ops.
- `hle_hid.c`: rewritten as a generic standard-controller layer covering all Npad styles plus six-axis, vibration, mouse and keyboard stubs.
- `hle_sdk16.c` (new): newer-SDK `oe` / `time` / shared font (`pl`) / vsync event / `diag` hooks.
- `hle_offline.c` (new): offline stubs for play reports, local wireless, sockets (C++ and C APIs), SSL, nifm, nsd, friends, ngc and err.
- `hle_fs.c`, `hle_account.c`: newer-SDK overloads (`MountRom(name)`, `ReadFile` variants, `GetDirectoryEntryCount`, user enumeration, ...).
- `loader.c`: forced hooks that a game never references are now only logged (`DM_FORCE_STRICT=1` restores the abort).
- NVN:
  - API version reported as 55.15 (`DM_NVN_API` overrides it).
  - 13 newer names were added to the table.
  - `tex_t` is packed to 0x80 bytes: Mario allocates `NVNtexture` as 0x80, and the old 0x88 record overran it and corrupted the heap.

## Build and run on Mac

Requires Xcode and the game files extracted to `~/smbw-work/data/v0/{ExeFS,RomFS}`.

```
python3 dm/tools/patch_nso.py ~/smbw-work/data/v0/ExeFS/main <out> main    # with DM_RULESET=native; same for sdk
make -C dm/host dmhost
~/smbw-work/run/boot.sh <logname> [seconds]
```

## Tools (dm/tools)

| Tool | What it does |
|---|---|
| `nso2elf.py` | Converts NSO to ELF for Ghidra / REA |
| `insn_census.py` | Counts the instruction sites the rewriter must handle |
| `nso_syms.py` | Compares the SDK import surface of two games |
| `nvn_slots_got.py` | Builds the NVN fetch table and call sites (GOT-indirect loaders) |
