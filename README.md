# smbw-mobile

Work in progress: an iOS native-execution port of **Super Mario Bros. Wonder** (Switch, base game v0 and update 1.2.1), built from the generic parts of [d2r-mobile](../d2r-mobile) (MIT). Bring your own game files; nothing from the game is in this repository.

Same approach as d2r-mobile: the game's own aarch64 code runs natively. A rewriter changes only the instructions that cannot run under Apple's kernel (x18, thread pointers, `svc`, timers). The host replaces Switch system services, and the NVN graphics API is reimplemented on Metal.

## Status (2026-10-14)

| Step | State |
|---|---|
| Rewriter (`dm/tools/patch_nso.py`, `DM_RULESET=native`) | Runs unchanged on Mario `main` / `sdk`, v0 and 1.2.1 |
| Mac host | **Plays through the menus (player count, character select) into the opening course**: the course, effects and HUD render. Title screen at 60 fps, memory flat (about 900 MB physical footprint on the title screen). Menu and dialogue text currently render blank on the Mac (they are fine on the iPhone); not investigated yet |
| Shaders | All shaders the game ships (bfsha archives, particle effects, engine `agl` packages, UI layouts, `main`) are extracted by `shader_split.py` and translated offline: 12,811 in v0, 16,268 across v0 + 1.2.1 (the update recompiled 3,451 pixel shaders and added 6); 16,266 compile. 2 `agl` post-effect compute shaders make SPIRV-Cross throw. Vertex shaders get the outputs their paired pixel shaders read (all 14,603 pairs link) |
| Input | Scripted input `DM_KEYS="12:A,14:A,20:A,26:A,32:A"` (seconds:button) reaches the opening course; `DM_WINDOW=1` for a window with keyboard input |
| Current frontier | **Playable on the iPhone with sound through several levels**, on v0 and on 1.2.1 (run, jump, collect coins; characters, NPCs, sky, text, button icons render; music and effects play). The Mac runs are scripted and reach the opening course |
| Known gaps | Partial / mid-pass clears; sparse virtual-pool mappings; conditional render; `DrawElementsIndirect`; TBDR optimizations are off by default (they don't understand bindless textures yet); resource heaps get +16 KB slack because one model's setup came up 0x70 bytes short of the size table (cause unknown); no amiibo (no NFC reader); the bundled prewarm list is empty, so the first run after an install stutters while pipelines compile (see "Prewarm lists" below) |
| Audio | The game's own SDK renderer runs in-process in its CPU / manual-execution mode (`hle_audren.c`); the host renders one 5 ms frame per buffer the output hands back and sends the device-sink mix (5.1 folded to stereo) through Core Audio (`DM_AUDIO=1` on Mac, default on iOS). About 0.1 ms of CPU per frame on an M-series Mac |
| iOS bundle | `dm/ios/tools/build_ipa.sh` builds one unsigned IPA (iPhone + iPad): game code embedded in the executable, iOS shader libraries, RomFS (3.5 GB) inside the app, standard-controller virtual gamepad and physical controllers. **Runs on an iPhone** (sideloaded): an 11-minute session through several levels at 60 fps with no crash and no audio dropouts |

## Build and run on Mac

Requires Xcode, the .NET SDK, spirv-cross and the game files extracted to `~/smbw-work/data/v0/{ExeFS,RomFS}`.

```
# 1. rewrite the code modules
DM_RULESET=native python3 dm/tools/patch_nso.py ~/smbw-work/data/v0/ExeFS/{main,sdk} ~/smbw-work/build/patched_v0 {main,sdk}
# 2. shaders: extract, then translate (writes <cache>/b-<chain version>/)
<venv python with lz4> dm/gfx/tools/shader_split.py ~/smbw-work/data/v0/RomFS ~/smbw-work/data/v0/ExeFS/main ~/smbw-work/shader/pkg dm/gfx/nvn/shader_map.tsv dm/gfx/nvn/shader_facts.tsv dm/gfx/nvn/shader_pairs.tsv
dotnet publish dm/gfx/tools/dmsh_xlate -c Release -o ~/smbw-work/build/gfx/dmsh_xlate
XLATE=~/smbw-work/build/gfx/dmsh_xlate/dmsh_xlate python3 dm/gfx/tools/dmsh_chain.py --header
XLATE=... python3 dm/gfx/tools/dmsh_build.py ~/smbw-work/shader/pkg ~/smbw-work/build/cache/metal_shader_cache
python3 dm/gfx/tools/c1_guard.py ~/smbw-work/build/cache/metal_shader_cache/b-<V> dm/gfx/nvn/c1_full.tsv
# 3. host
make -C dm/host dmhost
# 4. button-icon font for the Nintendo-extension shared font (needs fontTools: pip install fonttools)
python3 dm/tools/make_ext_font.py ~/smbw-work/build/font/nintendo_ext.ttf
~/smbw-work/run/boot.sh <logname> [seconds]     # sets DM_MSL_DIR to the shader cache and DM_FONT_DIR to ~/smbw-work/build/font
```

### Update 1.2.1 (v327680)

hactool rejects the update's patch NCA ("Invalid BKTR layout!"); `dm/tools/nx_extract` (LibHac) layers it over the base game instead and takes the title keys from the NSPs' tickets. Files identical to v0 become APFS clones, so the merged RomFS adds about 114 MB.

```
dotnet build dm/tools/nx_extract -c Release -o ~/smbw-work/build/nx_extract
~/smbw-work/build/nx_extract/nx_extract <prod.keys> <v0 NSP> <v327680 NSP> ~/smbw-work/data/v327680 ~/smbw-work/data/v0/RomFS
DM_RULESET=native python3 dm/tools/patch_nso.py ~/smbw-work/data/v327680/ExeFS/{main,sdk} ~/smbw-work/build/patched_v327680 {main,sdk}
# shaders: list both versions, base first, so one shader build serves both (then dmsh_chain.py --header, dmsh_build.py, c1_guard.py, make as above)
<venv python with lz4> dm/gfx/tools/shader_split.py ~/smbw-work/data/v0/RomFS,~/smbw-work/data/v327680/RomFS ~/smbw-work/data/v0/ExeFS/main,~/smbw-work/data/v327680/ExeFS/main ~/smbw-work/shader/pkg dm/gfx/nvn/shader_map.tsv dm/gfx/nvn/shader_facts.tsv dm/gfx/nvn/shader_pairs.tsv
GV=v327680 ~/smbw-work/run/boot.sh <logname> [seconds]     # GV picks data/$GV and build/patched_$GV
PATCHED=~/smbw-work/build/patched_v327680 ROMFS=~/smbw-work/data/v327680/RomFS dm/ios/tools/build_ipa.sh
```

What 1.2.1 changes for the port:

- `main` is a new build (`ff773e90…`, SDK 16.2.12; v0 is 16.2.0). The one per-build rewriter cut (resource heap +16 KB) was carried over with `dm/tools/nso_fnmap.py`. The rewriter now writes each module's build ID into `fixups.tsv`, and the host limits two inherited D2R behaviours to D2R's build: the 3D render scale (it writes and calls fixed `main` addresses, which in Mario are unrelated code and data) and the owned add-on content.
- New SDK calls the host answers: `nn::nfp` (amiibo: one NFC device on the connected controller that never finds a tag), `pl::IsRunningOnOunce` (false, i.e. a Switch 1), `ec::ShowShopApplicationInformationForUpgrade` (no-op), `account::PopOpenUsers` (none, so the game opens the preselected user as in v0). `oe::GetDisplayVersion` reports 1.2.1.
- 3,451 SecredUber pixel shaders were recompiled and 6 effect shaders added; the shader tables hold both versions.

Agent-facing guides for analysing and porting other titles are in [docs/agents](docs/agents/README.md).

## Build the iOS app (unsigned IPA)

Needs Xcode 27, xcodegen, fontTools (`FONT_PY=` a python that has it, if `python3` does not), and the Mac build steps above (the rewritten modules in `~/smbw-work/build/patched_v0`; for 1.2.1 set `PATCHED` and `ROMFS` as in the update section). The app also bundles `dm/font/standard.ttf` (copied from macOS's Arial Unicode, `STD_FONT=` to override) and the generated `nintendo_ext.ttf`.

```
# iOS shader libraries for the current chain version (writes ~/smbw-work/build/ios/dmsh_b/b-<V>/; about 15 min)
IOSOUT=~/smbw-work/build/ios/dmsh_b XLATE=... python3 dm/gfx/tools/dmsh_build.py ~/smbw-work/shader/pkg ~/smbw-work/build/cache/metal_shader_cache
# the IPA (default ~/Desktop/SMBWMobile.ipa)
dm/ios/tools/build_ipa.sh [out.ipa]
# diagnostic build: bake environment switches into the app (one KEY=VALUE per line, # comments)
DM_ENV_FILE=diag_env.txt dm/ios/tools/build_ipa.sh
```

If Xcode's Locations setting puts build products in a custom location, the script takes the app's path from the build log.

`dm/ios/tools/build_ipa_from_nsp.py` runs the whole chain (extract, rewrite, shaders, IPA) from only this repository, a folder with the v0 and 1.2.1 NSPs, and `prod.keys`. Settings are at the top of the file, and it runs in Spyder or with `python3`. Every intermediate file goes to `smbw_build/` in the NSP folder. The shader translator it uses is `dm/gfx/tools/dmsh_xlate_bin`, the build whose fingerprints match `dmsh_chain.lock` (a rebuilt one does not).

Sign and install it with your own tool (AltStore, Sideloadly, ...), which replaces the placeholder bundle ID `org.smbwmobile.app`. The app shell is `dm/ios/Sources`:

- `main.m`: environment, log, boot thread.
- `front_ios.m`: the `front.h` functions and a centred 16:9 Metal view.
- `DmGamepad.m`: translucent touch gamepad drawn over the picture and kept close to the edges: ZL above L and ZR above R in the top corners, −/+ in the bottom corners with the D-pad and A/B/X/Y (Nintendo positions) just above them, left and right sticks (fixed rings) just inside those. Hidden while a physical controller is connected; physical buttons map by position.

The host log goes to the app's Documents (`log.txt`, previous run `log.prev.txt`), visible in the Files app. Game files are also accepted at `Documents/romfs`. Environment switches can also be put in `Documents/env.txt` (same format as `DM_ENV_FILE`; it takes precedence over the bundled file), so a sideloaded app can be switched without rebuilding.

Everything the app writes lives in its Documents: `save/` (game saves), `pso_user.tsv` (see below), the logs. Sideloading tools may give a reinstalled app a new data folder (LiveContainer did, under its `Data/Application/<UUID>/Documents`); the old saves and prewarm list stay in the previous folder and can be copied over with the Files app while the app is closed.

### Prewarm lists

Metal compiles every combination of shaders and pipeline settings for the GPU the first time it is used (tens of milliseconds each; a burst of them on entering a level is a visible freeze). The host keeps lists of what it had to compile and rebuilds those pipelines on a background queue at the next launch:

- `pso_user.tsv`, next to the save directory (`Documents/pso_user.tsv` on iOS, `run/pso_user.tsv` for the Mac `boot.sh`; `DM_PSO_USER=path` overrides): appended while playing, one line per shader (`S`), render pipeline (`R`, the hex of `pso_skey` in `dm/gfx/nvn/nvn_pso_skey.h`) or compute pipeline (`C`) that had to be built on the spot.
- `dm/gfx/nvn/pso_manifest.tsv`: the seed list bundled with the app (and read by the Mac host next to `shader_map.tsv`). Prewarmed before `pso_user.tsv`. Currently empty.

The lines name shaders by content key and describe the pipeline settings; they hold no compiled code and nothing personal, so a list recorded on one iPhone works on any other device (each one compiles for its own GPU and iOS caches the result). Lines a build cannot use (unknown shader, a different key layout) are skipped. To seed the bundled list, append recorded lines from one or more devices to `pso_manifest.tsv` below its two header lines (duplicates are dropped at load; keep the recorded order, it is the order of first use) and rebuild the IPA. Record on the game version you ship: pipelines of the 3,451 shaders that 1.2.1 replaced are compiled for nothing on 1.2.1.

## Tools (dm/tools)

| Tool | What it does |
|---|---|
| `nx_extract/` | NSP → ExeFS / RomFS with LibHac, merging an update over the base game (C#, `dotnet build`) |
| `nso_fnmap.py` | Carries offsets from one build to another (function matching with addresses masked; globals via their references) |
| `nso2elf.py` | Converts NSO to ELF for Ghidra / REA |
| `insn_census.py` | Counts the instruction sites the rewriter must handle |
| `nso_syms.py` | Compares the SDK import surface of two games |
| `nvn_slots_got.py` | Builds the NVN fetch table and call sites (GOT-indirect loaders) |
