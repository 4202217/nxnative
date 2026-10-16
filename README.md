# nxnative

Nintendo Switch games running natively on iPhone, iPad and Mac. You bring your own game (its NSP files and your `prod.keys`); one script turns them into an unsigned IPA that you sideload. Nothing from any game is in this repository, and the project is not affiliated with Nintendo or any game publisher.

It is not an emulator: the game's own aarch64 code runs directly on the Apple CPU.
- A rewriter changes only the instructions that cannot run under Apple's kernel (x18, thread pointers, `svc`, timers).
- A host program stands in for the Switch's system services.
- The NVN graphics API is reimplemented on Metal, with the game's shaders translated ahead of time.

## Acknowledgements

This project is built on other people's work:

- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** (MIT) developed this native-execution approach, for Diablo II: Resurrected. nxnative (first called smbw-mobile) started as a copy of its generic parts: the NSO rewriter, the host that stands in for Horizon, the NVN-on-Metal layer and the iOS app shell. Fixes go both ways.
- **Ryujinx** (MIT): its shader translator (`Ryujinx.Graphics.Shader`, `Ryujinx.Common` and `Spv.Generator` 1.3.2, unmodified in `dm/third_party/ryujinx`) turns the games' Maxwell shaders into SPIR-V. Its emulation of the kernel and system services is also a reference when the host's behaviour is in doubt.
- **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)**: its kernel, mesosphère, is the reference for Horizon's synchronization semantics (address arbiter, condition variables).
- **yuzu**: Maxwell instruction encodings for the shader reference tool, and a cross-check for kernel behaviour.
- **[LibHac](https://github.com/Thealexbarney/LibHac)**: reads NSPs and layers updates over the base game in `dm/tools/nx_extract`. **hactool** did the first extractions.
- **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)**: SPIR-V to Metal Shading Language.
- **deko3d** (zlib): its texture layout size algorithm is ported in `dm/gfx/nvn/nvn_tex.c`.
- **libvpx** (VP9 movies), **libdeflate**, **bcdec** and **NVIDIA FLIP** ship in `dm/third_party` with their licenses ([list](dm/third_party/README.md)).
- **Capstone**, **lz4**, **fontTools** and **XcodeGen** are used by the build.

## Games

| Game | Profile | NSPs the folder needs | State |
|---|---|---|---|
| Super Mario Bros. Wonder | `010015100B514000` | the base game (1.0.0) and update 1.2.1 | **Playable on the iPhone with sound.** An 11-minute session through several levels ran at 60 fps with no crash. The IPA packs 1.2.1 (`-p v0` packs 1.0.0) and is about 3.9 GB |
| Fire Emblem Engage | `0100A6301214E000` | the base game and update 2.0.0; DLC NSPs (Expansion Pass) are packed too when present | **In bring-up.** On the Mac it plays the prologue, battles included. The first iPhone run (2026-10-15) went through a new game's opening movie and name entry at 60 fps with no crash. After a trip to the background, one of the game's two audio outputs stopped being fed; this is being investigated. It runs at 60 fps at the right speed: the Switch version runs at 30, and one patch to its code switches it to the 60 fps timing the game already has, so nothing runs too fast. The IPA is about 15 GB |
| Metroid Dread | `010093801237C000` | the base game and update 2.1.0 | **In bring-up, Mac only so far.** A new game runs on the Mac through the title, the story recap and the opening cutscene into the first area of ZDR, at 60 fps with movies and music. No IPA has been tested yet |
| 13 Sentinels: Aegis Rim | `01003FC01670C000` | the base game and update 1.10 | **In bring-up, Mac only so far.** A new game runs on the Mac through the logos, the title, the prologue and its cutscenes into the first battle, at 60 fps with movies and sound. No IPA has been tested yet |
| Super Mario 3D World + Bowser's Fury | `010028600EBDA000` | the base game and update 1.2.2 | **In bring-up, Mac only so far.** On the Mac, Super Mario 3D World runs from the game select through its title, the player join screen and the opening cutscene into World 1-1, at 60 fps with sound; a 5-minute session in the level ran with no crash and flat memory. Bowser's Fury runs too: its title, the opening cutscene and the open world of Lake Lapcat. No IPA has been tested yet |
| The Legend of Zelda: Breath of the Wild | `01007EF00011E000` | the base game and update 1.9.0; DLC NSPs are packed too when present | **In bring-up, Mac only so far.** On the Mac it runs through the title menu and a new game into the Shrine of Resurrection with sound and lighting. A patch to the game's frame-interval logic lifts its 30 fps limit to 60 without changing the game speed (it falls back to 30 by itself when a frame takes too long). In the shrine the Mac holds 60 fps at the handheld resolution (`DM_OPMODE=0`, 1280×720) and averages about 48 at the docked 1600×900. No IPA has been tested yet |

For any other game, the script writes a survey instead of an IPA (see [Port another game](#port-another-game)).

## Pack an IPA

You need:
- a Mac with Xcode, XcodeGen, the .NET SDK 10, zstd and spirv-cross;
- a Python 3 with `lz4`, `capstone` and `fonttools`;
- your `prod.keys`;
- a folder holding one game's NSPs. The script finds the game by the `[title ID]` in the file names or by the NSP's ticket, and the update by its `[vN]` tag.

Free disk space: about the size of the NSPs plus 6 GB, and room for the IPA.

```
~/Games/MarioWonder/
  Super Mario Bros. Wonder [010015100B514000][v0].nsp
  Super Mario Bros. Wonder [010015100B514800][v327680].nsp
```

From a terminal (`-h` lists every option):

```
# Super Mario Bros. Wonder → ~/Games/MarioWonder/SMBWMobile.ipa
python3 scripts/build_ipa_from_nsp.py ~/Games/MarioWonder -k ~/keys/prod.keys

# the same, written to the Desktop with your own home-screen name
python3 scripts/build_ipa_from_nsp.py ~/Games/MarioWonder -k ~/keys/prod.keys -o ~/Desktop/Mario.ipa --app-name "Mario Wonder"

# pack Mario 1.0.0 instead of 1.2.1 (the folder still needs both NSPs: the shader tables cover both versions)
python3 scripts/build_ipa_from_nsp.py ~/Games/MarioWonder -k ~/keys/prod.keys -p v0

# Fire Emblem Engage → ~/Games/FEEngage/FEEngage.ipa
python3 scripts/build_ipa_from_nsp.py ~/Games/FEEngage -k ~/keys/prod.keys

# keep the extracted game files (to run the game on the Mac, or to skip extraction next time)
python3 scripts/build_ipa_from_nsp.py ~/Games/MarioWonder -k ~/keys/prod.keys --keep-extracted

# a diagnostic build with environment switches baked in (one KEY=VALUE per line)
python3 scripts/build_ipa_from_nsp.py ~/Games/MarioWonder -k ~/keys/prod.keys --env-file diag_env.txt
```

In Spyder, open `scripts/build_ipa_from_nsp.py`, fill in the settings at the top (`NSP_DIR`, `PROD_KEYS`, and optionally `PACKAGE`, `OUT_IPA`, `APP_NAME`, `BUNDLE_ID`), and press F5. Empty paths are asked for at run time.

What the script does:
1. It finds the game profile from the title ID.
2. It extracts the NSPs.
3. It rewrites the code modules, checking each build ID against the profile.
4. It splits out the shaders and checks them against the profile's tables.
5. It builds the iOS shader libraries.
6. It calls `build_ipa.sh`.

Everything it writes stays in the NSP folder: intermediate files and logs in `<id>_build/`, the IPA next to the NSPs. The extracted game files are deleted at the end unless you pass `--keep-extracted`. It exits with 0 when done and 1 on an error. For Mario, the first run takes about 35 minutes, mostly the shader libraries; a second run takes about 10.

## Install and play

Sign and install the IPA with your own tool (AltStore, Sideloadly, LiveContainer, ...). The app takes its name and icon from the base game NSP, and its bundle ID is `org.nxnative.<base game title ID>` (for example `org.nxnative.0100a6301214e000`); `APP_NAME` / `BUNDLE_ID` in the script override them. Your signing tool may replace the bundle ID.

- **Controls:** a translucent touch gamepad over the picture:
  - ZL above L in the top-left corner, ZR above R in the top-right;
  - − and + side by side at the top centre;
  - the left stick in the bottom-left corner, and A/B/X/Y (Nintendo positions) in the bottom-right, the same size and mirrored.

  There is no on-screen D-pad or right stick. A connected physical controller hides the touch gamepad and keeps every button, mapped by position.
- **Saves and logs** live in the app's Documents, visible in the Files app: `save/`, `log.txt` (the previous run in `log.prev.txt`) and `pso_user.tsv`. When reporting a problem, send `log.txt`.
- **Reinstalling:** sideloading tools may give a reinstalled app a new data folder; LiveContainer did, under its `Data/Application/<UUID>/Documents`. To keep your saves and prewarm list, copy them from the old folder while the app is closed.
- **Switches without rebuilding:** environment switches can go in `Documents/env.txt` (one `KEY=VALUE` per line). Game files are also accepted at `Documents/romfs`.

The first time each new scene appears, it can stutter while Metal compiles its pipelines; later launches prewarm them (see [Prewarm lists](#prewarm-lists)).

## How it works

| Part | State |
|---|---|
| Rewriter (`dm/tools/patch_nso.py`, `DM_RULESET=native`) | Runs unchanged on every module of both games. Per-build cuts are keyed by the module's build ID |
| Host (`dm/host`) | Stands in for the Horizon kernel and the system services the games call: threads and synchronization, memory, file system with transactional saves, HID, audio, movies (VP9 through libvpx), software keyboard, focus and lifecycle |
| Graphics (`dm/gfx`) | NVN on Metal. Shaders are extracted by `shader_split.py` from Nintendo's containers and from Unity's `Shader` objects, then translated offline. Mario: 16,266 of 16,268 shaders compile. Engage: 9,268 of 9,269 |
| Audio | Games that carry the SDK's audio renderer (Mario) run it in-process; games that use AudioOut directly (Engage: Wwise) feed it straight. Output goes through Core Audio (default on iOS, `DM_AUDIO=1` on the Mac) |
| Input | iOS: the touch gamepad and physical controllers. Mac: scripted input (`DM_KEYS=<seconds>:<button>,…`, with `DM_KBD=<text>` answering the system keyboard) or `DM_WINDOW=1` for a window with keyboard input |
| iOS app | One unsigned IPA for iPhone and iPad. The rewritten code is embedded in the executable; the shader libraries, the RomFS and the game icon are inside the app |
| Known gaps | Conditional render is always drawn; sparse virtual-pool mappings; `DrawElementsIndirect`. TBDR optimizations are off by default (they don't understand bindless textures yet). No amiibo. The bundled prewarm lists are empty. On the Mac, Mario's menu and dialogue text render blank (fine on the iPhone) |

## Port another game

Each game has a profile in `dm/games/<id>/`:
- `game.tsv` holds the title ID, known builds, code modules, RomFS marker, memory split, NVN driver tables, app name and bundle ID;
- the shader tables: `shader_map.tsv`, `shader_facts.tsv`, `shader_pairs.tsv`, `c1_full.tsv`, `pso_manifest.tsv`;
- `dmsh_chain.h` holds the chain version.

The shader tools pick a profile with `DM_GAME=<id>`; `make -C dm/host` and `build_ipa.sh` pick it with `GAME=<id>`. Profiles are named after the base game's title ID. There is no default game: without `GAME` / `DM_GAME` the tools stop and list the profiles. `python3 dm/games/game.py list` lists the profiles. Host code that relies on one build's fixed addresses checks that build's ID.

To add a game:
1. Run `scripts/build_ipa_from_nsp.py` on its NSP folder. With no matching profile, it writes a survey to `<title ID>_survey/survey.md`. The survey covers the modules, SDK, engine, containers, rewrite sites, and the imports compared with the host's hooks.
2. Add a profile as described in [dm/games/README.md](dm/games/README.md).
3. Bring the game up following [docs/agents](docs/agents/README.md): extraction, binary survey, a catalogue of host bring-up failures, graphics, and debugging recipes.

## Build by hand (Mac, for development)

You need the same tools as the script. The shader translator is the prebuilt `dm/gfx/tools/dmsh_xlate_bin`, the build that matches `dmsh_chain.lock`; a fresh `dotnet publish` gives different fingerprints. The commands use three variables:
- `G`, the profile;
- `W`, the game's workspace outside this repository;
- `VER`, a version folder named after the NSP version.

```
G=010015100B514000; W=~/$G-work; VER=v327680
# 1. extract: the base game alone (update -), or an update layered over it (the last argument lets unchanged files become clones)
dotnet build dm/tools/nx_extract -c Release -o $W/build/nx_extract
$W/build/nx_extract/nx_extract <prod.keys> <base NSP> - $W/data/v0
$W/build/nx_extract/nx_extract <prod.keys> <base NSP> <update NSP> $W/data/$VER $W/data/v0/RomFS
# 2. rewrite the profile's code modules
for m in $(python3 dm/games/game.py $G modules); do DM_RULESET=native python3 dm/tools/patch_nso.py $W/data/$VER/ExeFS/$m $W/build/patched_$VER $m; done
# 3. shaders: extract from every version in the profile's shader_versions (comma-separated, base first), then translate into <cache>/b-<chain version>/
DM_GAME=$G python3 dm/gfx/tools/shader_split.py $W/data/v0/RomFS,$W/data/$VER/RomFS $W/data/v0/ExeFS/main,$W/data/$VER/ExeFS/main $W/shader/pkg dm/games/$G/shader_map.tsv dm/games/$G/shader_facts.tsv dm/games/$G/shader_pairs.tsv
DM_GAME=$G python3 dm/gfx/tools/dmsh_chain.py --header
DM_GAME=$G python3 dm/gfx/tools/dmsh_build.py $W/shader/pkg $W/build/cache/metal_shader_cache
python3 dm/gfx/tools/c1_guard.py $W/build/cache/metal_shader_cache/b-<chain version> dm/games/$G/c1_full.tsv
# 4. the host, and the button-icon font for the Nintendo-extension shared font
make -C dm/host GAME=$G dmhost
python3 dm/tools/make_ext_font.py $W/build/font/nintendo_ext.ttf
# 5. run on the Mac
DM_ROOT=$W DM_ROMFS=$W/data/$VER/RomFS DM_SAVE=$W/run/save DM_TEMP=$W/run/temp DM_MSL_DIR=$W/build/cache/metal_shader_cache DM_FONT_DIR=$W/build/font dm/host/dmhost $W/build/patched_$VER
# 6. iOS shader libraries (about 15 min for Mario), then the IPA (default ~/Desktop/<the profile's ipa>)
IOSOUT=$W/build/ios/dmsh_b DM_GAME=$G python3 dm/gfx/tools/dmsh_build.py $W/shader/pkg $W/build/cache/metal_shader_cache
GAME=$G dm/ios/tools/build_ipa.sh [out.ipa]
```

Notes on these steps:
- `build_ipa.sh` takes the rewritten modules from `$W/build/patched_<package>` and the RomFS from `$W/data/<package>/RomFS`; `PATCHED` and `ROMFS` override them.
- It also bundles a standard font (macOS's Arial Unicode, `STD_FONT=` overrides it) and the generated `nintendo_ext.ttf`.
- The icon is the `icon.jpg` that `nx_extract` saves next to `RomFS/`; `DM_ICON=` points at another image.
- `DM_ENV_FILE=` bakes environment switches into the app, and `DM_DISPLAY_NAME=` renames it.
- If Xcode's Locations setting puts products in a custom place, the script finds the app through the build log.
- hactool cannot read every update (Mario 1.2.1's patch NCA gives "Invalid BKTR layout!"), which is why `nx_extract` uses LibHac.

## Prewarm lists

Metal compiles each combination of shaders and pipeline settings for the GPU the first time it is used. Each takes tens of milliseconds, so a burst of them on entering a level is a visible freeze. The host records what it had to compile and rebuilds those pipelines on a background queue at the next launch:

- `pso_user.tsv` sits next to the save directory: `Documents/pso_user.tsv` on iOS, the parent of `DM_SAVE` on the Mac (`DM_PSO_USER=path` overrides it). It is appended while playing, one line per item built on the spot: a shader (`S`), a render pipeline (`R`, the hex of `pso_skey` in `dm/gfx/nvn/nvn_pso_skey.h`) or a compute pipeline (`C`).
- `dm/games/<id>/pso_manifest.tsv` is the seed list bundled with the app, prewarmed before `pso_user.tsv`.

The lines name shaders by content key and hold no compiled code and nothing personal, so a list recorded on one device works on any other. To seed the bundled list:
1. Record on the Mac with prewarm off, so everything is built on demand: `DM_PSO_MANIFEST=off DM_PSO_RECORD=<file>`, playing the scenes players reach first. Engage's list covers boot, the opening movie, the title, a save load and the prologue battle.
2. Append the recorded lines below the header comments, in the recorded order. Duplicates are dropped at load.
3. Rebuild the IPA.

Record on the game version you ship. At launch the log names each list it read and its line count (`管线预热清单 … 行`), or says that `pso_user.tsv` does not exist yet.

## Game notes

**Super Mario Bros. Wonder (`010015100B514000`)**
- **Versions:** v0 (1.0.0) and 1.2.1 (v327680). With the update layered over v0, unchanged files become APFS clones, so the merged RomFS adds only about 114 MB to the 3.5 GB base.
- **Shaders:** 12,811 in v0 and 16,268 across both versions; the tables hold both.
- **1.2.1:** it is a new `main` build (SDK 16.2.12), whose resource-heap cut (+16 KB slack) was carried over with `dm/tools/nso_fnmap.py`. It also makes new SDK calls, which the host answers: `nn::nfp` finds no amiibo, `pl::IsRunningOnOunce` is false, the shop call is a no-op, and `account::PopOpenUsers` returns none.
- **Mac input:** `DM_KEYS="12:A,14:A,20:A,26:A,32:A"` reaches the opening course.

**Fire Emblem Engage (`0100A6301214E000`)**
- **Engine:** Unity 2020.3 IL2CPP, SDK 12.3.7. The shaders are read out of Unity bundles (`unityfs.py`).
- **Memory:** a 5 GB guest block, the total proven on a phone, with a 2.5 GB application pool. A per-build cut shrinks the SDK's address-region node area to 62 MB.
- **Audio:** Wwise drives AudioOut directly.
- **Movies:** VP9, decoded on the host.

## Tools (`dm/tools`)

| Tool | What it does |
|---|---|
| `nx_extract/` | NSP → ExeFS / RomFS / icon with LibHac, merging an update over the base game (C#, `dotnet build`). `--manifest` writes ExeFS and a RomFS file list only, for surveys |
| `nso_fnmap.py` | Carries offsets from one build to another (function matching with addresses masked; globals via their references) |
| `nso2elf.py` | Converts NSO to ELF for Ghidra / REA |
| `insn_census.py` | Counts the instruction sites the rewriter must handle |
| `nso_syms.py` | Compares the SDK import surface of two games, or of one game (`-` for the reference) with the host hooks |
| `nvn_slots_got.py` | Builds the NVN fetch table and call sites (GOT-indirect loaders) |

## License

MIT (see [LICENSE](LICENSE)). Third-party code keeps its own license; see [dm/third_party](dm/third_party/README.md).
