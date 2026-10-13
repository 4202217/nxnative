# Porting a Switch title: agent guide

These pages are for an agent asked to analyse a Switch NSP and port it with the native-execution approach of this repository and [d2r-mobile](../../../d2r-mobile): the game's own aarch64 code runs natively on Apple hardware, a rewriter patches only the instructions Apple's kernel cannot run, the host replaces Horizon system services, and NVN is reimplemented on Metal.

Everything here was learned on two titles: Diablo II: Resurrected (SDK ~12, NVN 53, the original target of d2r-mobile) and Super Mario Bros. Wonder (SDK 16.2.0, NVN 55, this repository; its update 1.2.1 uses SDK 16.2.12). Where the two differed, both are given; a third title will differ again, so treat every number below as "measure it, don't assume it".

| Page | Covers |
|---|---|
| [01-extract.md](01-extract.md) | Keys, NSP → NCA → ExeFS / RomFS, update NSPs, the workspace layout |
| [02-survey.md](02-survey.md) | Static survey of a new title before touching the host: SDK version, imports vs. host hooks, SVCs, instructions to rewrite, NVN surface, data containers, Ghidra / REA setup |
| [03-bringup.md](03-bringup.md) | Getting the host to boot the game: the run loop, how to read the log, and a catalogue of every failure pattern met so far with its root cause and fix |
| [04-graphics.md](04-graphics.md) | NVN on Metal for a new title: object sizes, shader containers and keys, binding layouts, bindless textures, depth mode, index sizes, swizzle, transient render targets, the diagnostic switches |
| [05-debugging.md](05-debugging.md) | Recipes: crash reports, lldb scripts, Ghidra headless decompilation, objdump, memory growth, render-pass and texture dumps, on-device diagnostics, offline pipeline link checks |

## Ground rules

- **Bring your own files.** Keys, NSPs and anything extracted from them stay in the user's workspace (`~/<title>-work`), never in the repository, never in a commit, never printed. Print key *names* and lengths only.
- **Measure, then change.** Each fix in this repository started from a concrete observation (a crash address and its decompilation, a census over all shaders, a memory profile). Write the evidence into the code comment next to the fix, the way the existing code does; the next title will need it.
- **One title per repository.** smbw-mobile is a copy of d2r-mobile's generic parts with D2R-specific code removed. Constants that are title- or SDK-specific (texture handle base, depth mode, address-space partition, program ID) are changed in place with a comment, not hidden behind runtime switches.
- **Keep the bilingual comment style** of the code base (Chinese line, then the English line), and the `【红】` prefix for red log lines that mark something unimplemented or wrong.
- **Test where it runs.** The Mac host (`dm/host/dmhost`) is the development loop; the iOS app reuses the same host and graphics code. Never claim something works on iOS without running it there.

## Checklist for a new title

1. Extract base (and update, if in scope) — [01](01-extract.md).
2. Survey the binaries and the RomFS — [02](02-survey.md). Write down: SDK version, NVN API version, module list, imports the host lacks, SVCs, instruction census, NVN loader pattern, shader containers.
3. Make a new repository from the previous port: copy the generic host / gfx / tools / third_party, delete title-specific modules, set the program ID.
4. Run the rewriter (`DM_RULESET=native`) and check its counts against `insn_census.py`.
5. Boot on the Mac and iterate on the failure catalogue in [03](03-bringup.md) until the game reaches its frame loop.
6. Extract and translate the shaders, find the binding layout by census, and get the first frame — [04](04-graphics.md).
7. Check memory over a minute of running (`footprint`, the host's memory report line); fix growth before moving on.
8. Input, audio, then the iOS bundle (d2r-mobile's `dm/ios`, RomFS inside the app).
9. Test on a phone: an unsigned IPA the user sideloads, switches through `env.txt`, results through `Documents/log.txt` ([05](05-debugging.md)). Several bugs only showed there (the command-buffer byte and the audio buffer size in [03](03-bringup.md), pipeline link failures in later levels in [04](04-graphics.md)).
10. For a game update: extract it over the base game, diff the imports, carry the per-build addresses over, and rebuild the shader tables for both versions ([01](01-extract.md)).
