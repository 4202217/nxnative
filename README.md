# smbw-mobile

Work in progress: an iOS native-execution port of **Super Mario Bros. Wonder** (Switch, base game v0), built from the generic parts of [d2r-mobile](../d2r-mobile) (MIT). Bring your own game files; nothing from the game is in this repository.

Same approach as d2r-mobile: the game's own aarch64 code runs natively. A rewriter changes only the instructions that cannot run under Apple's kernel (x18, thread pointers, `svc`, timers). The host replaces Switch system services, and the NVN graphics API is reimplemented on Metal.

## Status (2026-10-13)

| Step | State |
|---|---|
| Rewriter (`dm/tools/patch_nso.py`, `DM_RULESET=native`) | Runs unchanged on Mario `main` / `sdk` |
| Mac host | **Plays through the menus (player count, character select) into the opening course**: the course, effects, HUD, menu and dialogue text render in Chinese or English. Title screen at 60 fps, memory flat (about 900 MB physical footprint on the title screen) |
| Shaders | All 12,811 shaders the game ships (bfsha archives, particle effects, engine `agl` packages, UI layouts, `main`) are extracted by `shader_split.py` and translated offline; 12,809 compile. 2 `agl` post-effect compute shaders make SPIRV-Cross throw |
| Input | Scripted input `DM_KEYS="12:A,14:A,20:A,26:A,32:A"` (seconds:button) reaches the opening course; `DM_WINDOW=1` for a window with keyboard input |
| Current frontier | **The opening course is playable** (run, jump, collect coins; characters, NPCs, sky, text all render). Open: an intermittent water / refraction effect over the fountain draws as tall blue panels (sometimes with a wrong texture); controller-button icons in text show as `?` (the host's stand-in system font has no Nintendo-extension glyphs); no audio |
| Known gaps | Partial / mid-pass clears; sparse virtual-pool mappings; conditional render; `DrawElementsIndirect`; TBDR optimizations are off by default (they don't understand bindless textures yet); resource heaps get +16 KB slack because one model's setup came up 0x70 bytes short of the size table (cause unknown) |
| Audio | Silent AudioRenderer (`hle_audren.c`); real mixing comes later |
| iOS bundle | `dm/ios/tools/build_ipa.sh` builds one unsigned IPA (iPhone + iPad): game code embedded in the executable, iOS shader libraries, RomFS (3.5 GB) inside the app, standard-controller virtual gamepad and physical controllers. Compiles and links; **not yet run on a device** (this Mac has no signing identity, and the Simulator SDK has no Metal 4) |

## What changed relative to d2r-mobile

- Removed: all D2R gameplay / UI modules (`dm_*.c`), D2R diagnostics (`diag.c`), Blizzard TACT/CASC, Granny, movie, auto-targeting, and the archive validator. `host_stubs.c` holds the leftover hook points as no-ops.
- `hle_hid.c`: rewritten as a generic standard-controller layer covering all Npad styles plus six-axis, vibration, mouse and keyboard stubs.
- `hle_sdk16.c` (new): newer-SDK `oe` / `time` / shared font (`pl`) / vsync event / `diag` hooks.
- `hle_offline.c` (new): offline stubs for play reports, local wireless, sockets (C++ and C APIs), SSL, nifm, nsd, friends, ngc and err.
- `hle_fs.c`, `hle_account.c`: newer-SDK overloads (`MountRom(name)`, `ReadFile` variants, `GetDirectoryEntryCount`, user enumeration, ...).
- `loader.c`: forced hooks that a game never references are now only logged (`DM_FORCE_STRICT=1` restores the abort).
- `patch_nso.py`, two fixes:
  - **Exclusive-window bug, inherited from d2r-mobile.** When an ldxr/stxr window contained a branch out (`ldxr; cmp; b.ne out; stxr; cbnz`), a successful stxr fell into the branch-out exit. Every enqueue in Mario's job system retried forever and the job system hung. The fall-through exit is now emitted first. D2R's windows had no branch out, so D2R never hit it.
  - `mrs xN, ctr_el0` is rewritten to a constant (macOS traps it; SDK 16's `FlushDataCache` reads it).
- `hle_thread.c`: SetThreadCoreMask moves the ideal core into the new mask, as Horizon does, so `GetCurrentProcessorNumber` is right for pinned threads.
- NVN:
  - API version reported as 55.15 (`DM_NVN_API` overrides it).
  - 13 newer names were added to the table.
  - `tex_t` is packed to 0x80 bytes: Mario allocates `NVNtexture` as 0x80, and the old 0x88 record overran it and corrupted the heap.
  - `view_t` is packed to 16 bytes: SDK 16's `NVNtextureView` is 16 bytes and Mario keeps it in a 16-byte stack slot; the old 28-byte record overwrote the saved frame pointer and return address.
  - Shader key = sha256(control section + code area): Mario has many shaders with an identical control section and different code.
  - Bindless textures: 38 % of Mario's shaders read texture handles out of their own uniform buffers. New binding kinds `tex_ubo` / `samp_ubo` (`NVN_RB_TEXU` / `NVN_RB_SAMPU`) read the handle at replay time.
  - Bound texture handles live at `0x20 + 8 × slot` in NVN 55 (D2R's driver: `0x168`); images stay at `0x120 + 8 × slot`.
  - 8-bit index buffers are widened to 16-bit at replay (Metal has no 8-bit indices); Mario's full-screen passes and its final copy to the window use them.
  - Texture swizzle (`TextureBuilderSetSwizzle` / `TextureViewSetSwizzle`) is applied through Metal texture views (the font atlas is `(1, 1, 1, R)`).
  - Non-packaged textures are shared by storage (pool + offset + description): Mario rebuilds its transient render targets on new objects every frame and never finalizes them, which leaked about 13 MB of GPU memory per frame.
  - TBDR optimizations are off by default (`DM_NVN_TBDR=1` turns them on) until they recognize bindless readers.
  - Linear textures in CPU-visible pools are created directly on the pool buffer (zero copy). Mario's glyph cache (2048×8192 R8) is filled by the CPU; as a private GPU texture all menu and dialogue text was blank.
  - Host command-memory segments (lent when a command buffer outgrows the memory the game gave it) grow on demand instead of a fixed 64 × 1 MB.
  - `RGB10A2SN` vertex format; instanced `DrawArrays`.
  - Vertex formats follow the shader input type (integer inputs get integer formats of the matching sign).
- Shader chain:
  - `dm/gfx/tools/shader_split.py` (new) scans RomFS and `main` for BNSH and BAHS (agl `sharcb`) containers and writes the package directory, the shader map and the answer table (compute thread-group size from control +0x748, shared memory from +0x754).
  - The translator uses GL depth (`-1..1`) and no depth clip control, so vertex shaders remap z' = z/2 + w/2: Mario sets `nvnDeviceSetDepthMode` to near = -w, and without the remap its full-screen copy was clipped away.
  - Vertex shaders whose inputs are only used as integers are re-translated with `uint` inputs (`int_inputs`): skinned models read bone indices as raw integer bits, and the float default made every character invisible.
  - Compute code areas (magic 0x12345679) start their instructions at +0x100 (0x30 header + zero padding); `dmsh_build.py` strips all of it. Stripping only 0x30 misaligned the translator's 32-byte instruction groups: 12 particle / post-effect compute shaders became endless loops and hung the GPU at the first gameplay scene.
- Host:
  - Guest address space repartitioned: alias region 2.875 GB, heap 2.625 GB. Mario reserves 1.75 GB of `VirtualAddressMemory` (HeapDef) that it maps into the alias region on demand; with 1.25 GB, large layout archives got no address and were skipped.
  - Threads created with ideal core -2 run on the process default core (0); `GetCurrentProcessorNumber` never returns a negative core. Mario's font engine indexes per-core state with it.
  - The shared font is served with a BFTTF "already decoded" header so `ScalableFontEngineHelper::Decode` leaves it alone.
  - Crash reports scan the stack for return addresses and dump the popped frame.
  - `DM_PROBE` (`host/probe.c`): register-relative dumps (`@xN+off`), hit filters (`?[xN+off]=val`), a `sead::ExpHeap` walker (`!xN`), thread IDs, `DM_PROBE_ALL`.
- `patch_nso.py` code cuts for Mario `main` (build ID `cd6e42ae…`): resource heaps (`main+0x1e62fc`) get +16 KB.
- `dm/tools/ghidra/Decompile.java`: headless decompile by module offset; `@off` (re)creates functions reached only through vtables and undoes Ghidra's wrong "non-returning" mark on `nn::os` imports.

## Build and run on Mac

Requires Xcode, the .NET SDK, spirv-cross and the game files extracted to `~/smbw-work/data/v0/{ExeFS,RomFS}`.

```
# 1. rewrite the code modules
DM_RULESET=native python3 dm/tools/patch_nso.py ~/smbw-work/data/v0/ExeFS/{main,sdk} ~/smbw-work/build/patched_v0 {main,sdk}
# 2. shaders: extract, then translate (writes <cache>/b-<chain version>/)
<venv python with lz4> dm/gfx/tools/shader_split.py ~/smbw-work/data/v0/RomFS ~/smbw-work/data/v0/ExeFS/main ~/smbw-work/shader/pkg dm/gfx/nvn/shader_map.tsv dm/gfx/nvn/shader_facts.tsv
dotnet publish dm/gfx/tools/dmsh_xlate -c Release -o ~/smbw-work/build/gfx/dmsh_xlate
XLATE=~/smbw-work/build/gfx/dmsh_xlate/dmsh_xlate python3 dm/gfx/tools/dmsh_chain.py --header
XLATE=... python3 dm/gfx/tools/dmsh_build.py ~/smbw-work/shader/pkg ~/smbw-work/build/cache/metal_shader_cache
python3 dm/gfx/tools/c1_guard.py ~/smbw-work/build/cache/metal_shader_cache/b-<V> dm/gfx/nvn/c1_full.tsv
# 3. host
make -C dm/host dmhost
~/smbw-work/run/boot.sh <logname> [seconds]     # sets DM_MSL_DIR to the shader cache
```

Agent-facing guides for analysing and porting other titles are in [docs/agents](docs/agents/README.md).

## Build the iOS app (unsigned IPA)

Needs Xcode 27, xcodegen, and the Mac build steps above (the rewritten modules in `~/smbw-work/build/patched_v0`).

```
# iOS shader libraries for the current chain version (writes ~/smbw-work/build/ios/dmsh_b/b-<V>/; about 15 min)
IOSOUT=~/smbw-work/build/ios/dmsh_b XLATE=... python3 dm/gfx/tools/dmsh_build.py ~/smbw-work/shader/pkg ~/smbw-work/build/cache/metal_shader_cache
# the IPA (default ~/Desktop/SMBWMobile.ipa)
dm/ios/tools/build_ipa.sh [out.ipa]
```

Sign and install it with your own tool (AltStore, Sideloadly, ...), which replaces the placeholder bundle ID `org.smbwmobile.app`. The app shell is `dm/ios/Sources`:

- `main.m`: environment, log, boot thread.
- `front_ios.m`: the `front.h` functions and a centred 16:9 Metal view.
- `DmGamepad.m`: touch gamepad (left stick anywhere in the lower left, D-pad, A/B/X/Y in Nintendo positions, ZL/L, ZR/R, −/+). Hidden while a physical controller is connected; physical buttons map by position.

The host log goes to the app's Documents (`log.txt`, previous run `log.prev.txt`), visible in the Files app. Game files are also accepted at `Documents/romfs`.

## Tools (dm/tools)

| Tool | What it does |
|---|---|
| `nso2elf.py` | Converts NSO to ELF for Ghidra / REA |
| `insn_census.py` | Counts the instruction sites the rewriter must handle |
| `nso_syms.py` | Compares the SDK import surface of two games |
| `nvn_slots_got.py` | Builds the NVN fetch table and call sites (GOT-indirect loaders) |
