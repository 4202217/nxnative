# smbw-mobile

Work in progress: Nintendo Switch games running natively on iPhone, iPad and Mac. The game's own aarch64 code runs natively:
- a rewriter changes only the instructions that cannot run under Apple's kernel (x18, thread pointers, `svc`, timers);
- the host replaces the Switch system services;
- the NVN graphics API is reimplemented on Metal.

The project is built from the generic parts of [d2r-mobile](../d2r-mobile) (MIT). Everything specific to one game lives in a game profile, and one game is supported so far: **Super Mario Bros. Wonder**. Bring your own game files; nothing from any game is in this repository.

## Games (2026-10-14)

| Game | Profile | State |
|---|---|---|
| Super Mario Bros. Wonder (base game v0, update 1.2.1) | `smbw` | **Playable on the iPhone with sound through several levels**: an 11-minute session at 60 fps with no crash and no audio dropouts. Scripted Mac runs reach the opening course. Notes are in the [Super Mario Bros. Wonder](#super-mario-bros-wonder-smbw) section |
| Fire Emblem Engage | `fe_engage` | Bring-up (Unity 2020.3 IL2CPP, SDK 12.3.7): **on the Mac it plays the prologue**; left idle at the title it runs its 3D attract demo. `shader_split.py` reads the shaders out of the Unity bundles (9,268 of 9,269 compile). Movies play through a host decoder (VP9 with libvpx). Scripted input starts a new game, saves, and plays the prologue battle: player and enemy phases, combat animations, game over and retry; 12-minute runs end without a crash. Not yet: iOS (the guest block is 5 GB, the total proven on a phone) |

## Status

| Part | State |
|---|---|
| Rewriter (`dm/tools/patch_nso.py`, `DM_RULESET=native`) | Runs unchanged on Mario's `main` / `sdk`, v0 and 1.2.1 |
| Mac host | Runs Mario through the menus into the opening course: the course, effects and HUD render. The title screen runs at 60 fps with flat memory (about 900 MB physical footprint). Menu and dialogue text currently render blank on the Mac (they are fine on the iPhone); this is not investigated yet |
| Shaders | `shader_split.py` extracts the shaders from Nintendo's containers (bfsha archives, particle effects, `agl` packages, UI layouts, shaders embedded in `main`) and from Unity's `Shader` objects. They are translated offline, and each vertex shader gets the outputs its paired pixel shader reads. For Mario, 16,266 of 16,268 shaders compile; 2 `agl` post-effect compute shaders make SPIRV-Cross throw. All 14,603 pairs link |
| Audio | The game's own SDK renderer runs in-process in its CPU / manual-execution mode (`hle_audren.c`). The host renders one 5 ms frame per buffer the output hands back. It sends the device-sink mix (5.1 folded to stereo) through Core Audio (`DM_AUDIO=1` on Mac, default on iOS). This costs about 0.1 ms of CPU per frame on an M-series Mac |
| Input | On iOS, a touch gamepad and physical controllers. On the Mac, scripted input (`DM_KEYS=<seconds>:<button>,…`, with `DM_KBD=<text>` answering the system keyboard) or `DM_WINDOW=1` for a window with keyboard input |
| iOS bundle | `build_ipa.sh` builds one unsigned IPA for iPhone and iPad. The game code is embedded in the executable, and the iOS shader libraries and the RomFS are inside the app |
| Known gaps | Partial / mid-pass clears; sparse virtual-pool mappings; conditional render; `DrawElementsIndirect`. TBDR optimizations are off by default because they don't understand bindless textures yet. There is no amiibo (no NFC reader). The bundled prewarm list is empty, so the first run after an install stutters while pipelines compile (see [Prewarm lists](#prewarm-lists)) |

## Game profiles

Each game has a folder `dm/games/<id>/`. It holds:
- `game.tsv`: title ID, known builds, code modules, RomFS marker, memory split, NVN driver tables, app name and bundle ID;
- the shader tables: `shader_map.tsv`, `shader_facts.tsv`, `shader_pairs.tsv`, `c1_full.tsv`, `pso_manifest.tsv`;
- the chain version `dmsh_chain.h`.

How the profile is chosen and used:
- The shader tools pick a profile with `DM_GAME=<id>`. `make -C dm/host` and `build_ipa.sh` pick it with `GAME=<id>`. All of them default to `smbw`.
- `python3 dm/games/game.py list` lists the profiles.
- Host code that relies on one build's fixed addresses checks that build's ID.
- Code inherited from D2R that relies on D2R's addresses or files runs only on D2R's build.

To add a game:
1. Run `scripts/build_ipa_from_nsp.py` on its NSP folder (see [Build from the NSPs](#build-from-the-nsps)). With no matching profile, it writes a survey to `<title ID>_survey/survey.md`.
2. Add a profile as described in [dm/games/README.md](dm/games/README.md).
3. Bring the game up following the guides in [docs/agents](docs/agents/README.md).

## Build from the NSPs

`scripts/build_ipa_from_nsp.py` runs the whole chain: extract, rewrite, shaders, IPA. It needs only three things:
- this repository;
- a folder with one game's NSPs: the base game plus the updates in the profile's `shader_versions` (for Mario, v0 and 1.2.1). DLC is skipped;
- `prod.keys`.

The script runs in Spyder (open it and press F5) or from a terminal. That Python needs lz4, capstone and fonttools. In Spyder it uses the settings at the top of the file:
- the NSP folder and keys (asked for at run time if empty);
- the version to pack (default: the profile's `package`);
- the output IPA (default `~/Desktop/<the profile's ipa>`);
- `APP_NAME`, the home-screen name. Left empty, it is the game name from the base NSP.

From a terminal, options override those settings (`-h` lists them all):

```
python3 scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> [-p <version>] [-o <out.ipa>] [--app-name <name>] [--keep-extracted]
```

It exits with 0 when done and 1 on an error.

The script picks the profile from the NSPs' title ID and accepts only the builds the profile lists. Every intermediate file goes to `<id>_build/` in the NSP folder. For a game with no profile, it writes a survey instead of an IPA. The survey lists the modules, SDK, engine, containers, rewrite sites, and imports compared with the host hooks.

## Build step by step (Mac)

You need Xcode 27, the .NET SDK, spirv-cross, and a Python with lz4 and fontTools. The shader translator is the prebuilt `dm/gfx/tools/dmsh_xlate_bin`, the build that matches `dmsh_chain.lock`; a fresh `dotnet publish` gives different fingerprints.

The commands use three variables:
- `G`, the profile;
- `W`, the game's workspace outside this repository;
- `VER`, a game version folder, named after the NSP version.

```
G=smbw; W=~/$G-work; VER=v327680
# 1. extract: the base game alone (update -), or an update layered over it (the last argument lets unchanged files become clones)
dotnet build dm/tools/nx_extract -c Release -o $W/build/nx_extract
$W/build/nx_extract/nx_extract <prod.keys> <base NSP> - $W/data/v0
$W/build/nx_extract/nx_extract <prod.keys> <base NSP> <update NSP> $W/data/$VER $W/data/v0/RomFS
# 2. rewrite the profile's code modules
for m in $(python3 dm/games/game.py $G modules); do DM_RULESET=native python3 dm/tools/patch_nso.py $W/data/$VER/ExeFS/$m $W/build/patched_$VER $m; done
# 3. shaders: extract from every version in the profile's shader_versions (comma-separated, base first), then translate into <cache>/b-<chain version>/
DM_GAME=$G <python with lz4> dm/gfx/tools/shader_split.py $W/data/v0/RomFS,$W/data/$VER/RomFS $W/data/v0/ExeFS/main,$W/data/$VER/ExeFS/main $W/shader/pkg dm/games/$G/shader_map.tsv dm/games/$G/shader_facts.tsv dm/games/$G/shader_pairs.tsv
DM_GAME=$G python3 dm/gfx/tools/dmsh_chain.py --header
DM_GAME=$G python3 dm/gfx/tools/dmsh_build.py $W/shader/pkg $W/build/cache/metal_shader_cache
python3 dm/gfx/tools/c1_guard.py $W/build/cache/metal_shader_cache/b-<chain version> dm/games/$G/c1_full.tsv
# 4. the host, and the button-icon font for the Nintendo-extension shared font
make -C dm/host GAME=$G dmhost
python3 dm/tools/make_ext_font.py $W/build/font/nintendo_ext.ttf
# 5. run
DM_ROOT=$W DM_ROMFS=$W/data/$VER/RomFS DM_SAVE=$W/run/save DM_TEMP=$W/run/temp DM_MSL_DIR=$W/build/cache/metal_shader_cache DM_FONT_DIR=$W/build/font dm/host/dmhost $W/build/patched_$VER
```

hactool cannot read every update (Mario 1.2.1's patch NCA gives "Invalid BKTR layout!"). `dm/tools/nx_extract` (LibHac) layers the update over the base game and takes the title keys from the NSPs' tickets.

## Build the iOS app (unsigned IPA)

This needs the steps above and xcodegen. `build_ipa.sh` takes the rewritten modules from `$W/build/patched_<package>` and the RomFS from `$W/data/<package>/RomFS`, where `<package>` is the profile's `package` key. For another version, set `PATCHED` and `ROMFS`. The app also bundles `dm/font/standard.ttf` (copied from macOS's Arial Unicode; `STD_FONT=` overrides it) and the generated `nintendo_ext.ttf`. fontTools must be in `python3`, or set `FONT_PY=` to a Python that has it.

```
# iOS shader libraries for the current chain version (writes $W/build/ios/dmsh_b/b-<chain version>/; about 15 min for Mario)
IOSOUT=$W/build/ios/dmsh_b DM_GAME=$G python3 dm/gfx/tools/dmsh_build.py $W/shader/pkg $W/build/cache/metal_shader_cache
# the IPA (default ~/Desktop/<the profile's ipa>; DM_DISPLAY_NAME= renames the app)
GAME=$G dm/ios/tools/build_ipa.sh [out.ipa]
# diagnostic build: bake environment switches into the app (one KEY=VALUE per line, # comments)
GAME=$G DM_ENV_FILE=diag_env.txt dm/ios/tools/build_ipa.sh
```

If Xcode's Locations setting puts build products in a custom location, the script takes the app's path from the build log.

### Install and run

Sign and install the IPA with your own tool (AltStore, Sideloadly, ...). The tool replaces the placeholder bundle ID, which is the profile's `bundle_id`. The app shell is `dm/ios/Sources`:

- `main.m`: environment, log, boot thread.
- `front_ios.m`: the `front.h` functions and a centred 16:9 Metal view.
- `DmGamepad.m`: a translucent touch gamepad, drawn over the picture and kept close to the edges:
  - ZL above L in the top-left corner, ZR above R in the top-right;
  - − and + in the bottom corners, with the D-pad and A/B/X/Y (Nintendo positions) just above them;
  - left and right sticks (fixed rings) just inside those.

  The gamepad is hidden while a physical controller is connected. Physical buttons map by position.

The host log goes to the app's Documents (`log.txt`, with the previous run in `log.prev.txt`), visible in the Files app. Game files are also accepted at `Documents/romfs`. Environment switches can also go in `Documents/env.txt`, in the same format as `DM_ENV_FILE`. It takes precedence over the bundled file, so a sideloaded app can be switched without rebuilding.

Everything the app writes lives in its Documents: `save/` (game saves), `pso_user.tsv` (see below) and the logs. Sideloading tools may give a reinstalled app a new data folder (LiveContainer did, under its `Data/Application/<UUID>/Documents`). The old saves and prewarm list stay in the previous folder. Copy them over with the Files app while the app is closed.

### Prewarm lists

Metal compiles each combination of shaders and pipeline settings for the GPU the first time it is used. Each one takes tens of milliseconds, so a burst of them on entering a level is a visible freeze. The host keeps lists of what it had to compile and rebuilds those pipelines on a background queue at the next launch:

- `pso_user.tsv` sits next to the save directory: `Documents/pso_user.tsv` on iOS, the parent of `DM_SAVE` on the Mac (`DM_PSO_USER=path` overrides it). It is appended while playing, one line per item built on the spot: a shader (`S`), a render pipeline (`R`, the hex of `pso_skey` in `dm/gfx/nvn/nvn_pso_skey.h`) or a compute pipeline (`C`).
- `dm/games/<id>/pso_manifest.tsv` is the seed list bundled with the app; the Mac host reads it next to `shader_map.tsv`. It is prewarmed before `pso_user.tsv`.

The lines name shaders by content key and describe the pipeline settings. They hold no compiled code and nothing personal, so a list recorded on one iPhone works on any other device; each device compiles for its own GPU, and iOS caches the result. A build skips lines it cannot use (an unknown shader, a different key layout).

To seed the bundled list:
1. Append recorded lines from one or more devices to `pso_manifest.tsv`, below its two header lines. Duplicates are dropped at load. Keep the recorded order, which is the order of first use.
2. Rebuild the IPA.

Record on the game version you ship.

## Super Mario Bros. Wonder (`smbw`)

- **Versions:** the base game v0 (1.0.0) and update 1.2.1 (v327680). The IPA packs 1.2.1 by default, with a 3.5 GB RomFS.
- **Extraction:** with the update layered over v0, files identical to v0 become APFS clones, so the merged RomFS adds about 114 MB.
- **Shaders:** 12,811 in v0 and 16,268 across v0 + 1.2.1. The tables hold both versions.
- **Resource heaps:** they get +16 KB slack, a per-build rewriter cut, because one model's setup came up 0x70 bytes short of the size table. The cause is unknown.
- **Mac input:** `DM_KEYS="12:A,14:A,20:A,26:A,32:A"` reaches the opening course.
- **Prewarm list:** the bundled `pso_manifest.tsv` is empty.

What 1.2.1 changes for the port:

- **A new `main` build** (`ff773e90…`, SDK 16.2.12; v0 is 16.2.0). The resource-heap cut was carried over with `dm/tools/nso_fnmap.py`. The rewriter now writes each module's build ID into `fixups.tsv`, and the host limits behaviours inherited from D2R to D2R's build. Two of them are:
  - the 3D render scale, which writes and calls fixed `main` addresses that in Mario are unrelated code and data;
  - the owned add-on content.
- **New SDK calls**, which the host answers:
  - `nn::nfp` (amiibo): one NFC device on the connected controller that never finds a tag;
  - `pl::IsRunningOnOunce`: false, i.e. a Switch 1;
  - `ec::ShowShopApplicationInformationForUpgrade`: no-op;
  - `account::PopOpenUsers`: none, so the game opens the preselected user as in v0.

  `oe::GetDisplayVersion` reports 1.2.1.
- **Shaders:** 3,451 SecredUber pixel shaders were recompiled and 6 effect shaders added. Prewarm lines recorded on v0 for the 3,451 replaced shaders are compiled for nothing on 1.2.1.

## Tools (dm/tools)

| Tool | What it does |
|---|---|
| `nx_extract/` | NSP → ExeFS / RomFS with LibHac, merging an update over the base game (C#, `dotnet build`). `--manifest` writes ExeFS and a RomFS file list only, for surveys |
| `nso_fnmap.py` | Carries offsets from one build to another (function matching with addresses masked; globals via their references) |
| `nso2elf.py` | Converts NSO to ELF for Ghidra / REA |
| `insn_census.py` | Counts the instruction sites the rewriter must handle |
| `nso_syms.py` | Compares the SDK import surface of two games, or of one game (`-` for the reference) with the host hooks |
| `nvn_slots_got.py` | Builds the NVN fetch table and call sites (GOT-indirect loaders) |
