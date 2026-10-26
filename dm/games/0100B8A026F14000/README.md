# FINAL FANTASY RESONANCE (`0100B8A026F14000`) — the Hong Kong release

The region variant of the JP release ([0100606026F12000](../0100606026F12000/README.md)): the same engine build (Unreal Engine 5.6.1, `SDK MW+EpicGames+UnrealEngine-5_6_1`), the same `sdk` and `subsdk0` (STEINS;GATE RE:BOOT's builds, registered in `dm/tools/sdk_patches.py`), and the same shader libraries (73,496 shaders). Its `main` carries another build ID, so everything the JP profile registers by build ID needs this profile's own row — and its code is byte-identical at every site that matters: the 62 GB default of `nninitStartup` (`main+0x137c0`), the FMallocBinned3 constructor call (`main+0x137f0`, the only occurrence of its encoding in 94 MB of text), the allocator object (`main+0x77c6210`) and the pool base store (`main+0x7c148a0`).

Status: brought up on an iPhone Air from the NSP script's provisional profile first, which is where the two build-keyed registrations this profile adds came from.

- The provisional profile kept the engine's raw 62 GB address-space request (a Mac reserves it; the iPhone cannot): the first launch died at `nnosInitialize` with `FATAL reserve guest AS`.
- Without a registered build neither the constructor cut nor the allocator takeover applied, so with the JP release's capped region the game got past `nninitStartup` and then faulted (`sig=11 addr=0x30 pc=main+0x1385c`) on the first `FMemory` allocation: the vtable at the allocator object was still zeroed.
- With this profile — the JP release's memory layout (`-SwitchVirtualAddressSpaceSize=4064`, Engage's 5 GB block), the cut in [`patches.py`](patches.py) and the row in `hle_uemalloc.c` — the game boots from the launcher through the opening into the difficulty choice; the host reports `FINAL FANTASY RESONANCE 1.0.1 HK: Unreal's allocator (FMallocBinned3 at main+0x77c6210) runs on a host malloc zone` and the heap grows past 0.75 GB. Traditional Chinese (`DM_LANG=zh-Hant`); the DLC (見習騎士起步衝刺包, `0100B8A026F15002`) packs.
- **Resolved: the first scene after the difficulty choice.** Two host/translator defects stood between the difficulty choice and that scene, both fixed in this tree rather than worked around in the profile:
  - The translator's `GenerateCall` took every call argument for a local variable, and the game's `GlobalDistanceFieldCompositeObjects.usf` shaders pass a constant, so 994 of its compute shaders failed to translate ([the note in `docs/reference/shaders.md`](../../../docs/reference/shaders.md#a-call-argument-that-the-ast-optimizer-turned-into-a-constant)).
  - `imga_extent` compared the bound texture's type against the type the translator declared when the answer table had none, and aborted the run on the first dispatch ([its note](../../../docs/reference/shaders.md#the-image-extent-of-a-bound-texture-against-a-declared-guess)).
  
  With both, the game plays past the difficulty choice into the first scene on an iPhone Air at 60 fps.
- **The particle effects need two further fixes that are not part of this profile**, reported and diagnosed in [issue #168](https://github.com/seedds/nxnative/issues/168): Unreal's Niagara vertex factories read particle attributes through a phi of `c[2]` handles, which `BindlessElimination` used to delete (every vertex of the fire and smoke meshes collapsed to a point), and buffer textures bound below Metal's 16-byte alignment read from the boundary beneath them on an M4, which made the same effects flicker. With [b1tdr3amer24's `ffr-bindless-texel-fixes`](https://github.com/b1tdr3amer24/nxnative/tree/ffr-bindless-texel-fixes) applied on top of this profile, the roadside fires, fire light, smoke, fireball and shield effects render and stop flickering on an iPhone Air, at 60 fps, the game's own speed.

## What it needs

| Setting | Where | Why |
|---|---|---|
| everything the JP release needs | [`../0100606026F12000/game.tsv`](../0100606026F12000/game.tsv) | the same engine, the same sdk / subsdk0 builds, the same shader libraries (the tables here are from this build's own files; `shader_facts_manual.tsv` carries the JP table's integer output row) |
| FMallocBinned3 constructor cut `main+0x137f0` | [`patches.py`](patches.py), `main` `b7f965b9…` | this build's own row of the JP cut; the host runs Unreal's heap (`hle_uemalloc.c` layout `UE5_6`) |
| allocator takeover row | `dm/host/hle_uemalloc.c` `k_ue[]`, `main` `b7f965b9…` | the object / vtable / cut offsets are the JP build's, verified on this build |
