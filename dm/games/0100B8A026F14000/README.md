# FINAL FANTASY RESONANCE (`0100B8A026F14000`) — the Hong Kong release

The region variant of the JP release ([0100606026F12000](../0100606026F12000/README.md)): the same engine build (Unreal Engine 5.6.1, `SDK MW+EpicGames+UnrealEngine-5_6_1`), the same `sdk` and `subsdk0` (STEINS;GATE RE:BOOT's builds, registered in `dm/tools/sdk_patches.py`), and the same shader libraries (73,496 shaders). Its `main` carries another build ID, so everything the JP profile registers by build ID needs this profile's own row — and its code is byte-identical at every site that matters: the 62 GB default of `nninitStartup` (`main+0x137c0`), the FMallocBinned3 constructor call (`main+0x137f0`, the only occurrence of its encoding in 94 MB of text), the allocator object (`main+0x77c6210`) and the pool base store (`main+0x7c148a0`).

Status: brought up on an iPhone Air from the NSP script's provisional profile first, which is where the two build-keyed registrations this profile adds came from.

- The provisional profile kept the engine's raw 62 GB address-space request (a Mac reserves it; the iPhone cannot): the first launch died at `nnosInitialize` with `FATAL reserve guest AS`.
- Without a registered build neither the constructor cut nor the allocator takeover applied, so with the JP release's capped region the game got past `nninitStartup` and then faulted (`sig=11 addr=0x30 pc=main+0x1385c`) on the first `FMemory` allocation: the vtable at the allocator object was still zeroed.
- With this profile — the JP release's memory layout (`-SwitchVirtualAddressSpaceSize=4064`, Engage's 5 GB block), the cut in [`patches.py`](patches.py) and the row in `hle_uemalloc.c` — the game boots from the launcher through the opening into the difficulty choice; the host reports `FINAL FANTASY RESONANCE 1.0.1 HK: Unreal's allocator (FMallocBinned3 at main+0x77c6210) runs on a host malloc zone` and the heap grows past 0.75 GB. Traditional Chinese (`DM_LANG=zh-Hant`); the DLC (見習騎士起步衝刺包, `0100B8A026F15002`) packs.
- **Not yet re-tested: the first scene after the difficulty choice.** A package built from the provisional profile, which lacked the JP profile's `shader_facts_manual.tsv` row (its shader libraries are the same, so this profile carries the same row), went black and then crashed on the first `Metal` pipeline at exactly that point — the symptom that row exists to prevent. The tables here include it and a rebuild from this profile passes the NSP script's table check, but the resulting package has not been played through that scene.

## What it needs

| Setting | Where | Why |
|---|---|---|
| everything the JP release needs | [`../0100606026F12000/game.tsv`](../0100606026F12000/game.tsv) | the same engine, the same sdk / subsdk0 builds, the same shader libraries (the tables here are from this build's own files; `shader_facts_manual.tsv` carries the JP table's integer output row) |
| FMallocBinned3 constructor cut `main+0x137f0` | [`patches.py`](patches.py), `main` `b7f965b9…` | this build's own row of the JP cut; the host runs Unreal's heap (`hle_uemalloc.c` layout `UE5_6`) |
| allocator takeover row | `dm/host/hle_uemalloc.c` `k_ue[]`, `main` `b7f965b9…` | the object / vtable / cut offsets are the JP build's, verified on this build |
