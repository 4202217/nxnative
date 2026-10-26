# FINAL FANTASY RESONANCE HK (0100B8A026F14000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's
# NSO build ID; CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py (this game's sdk and subsdk0 are the
# builds registered there: 0023a1b0… / ab62fe8e…, the ones the JP release's game profile 0100606026F12000 documents)
CUTS = {
  # FINAL FANTASY RESONANCE 1.0.1 (v1) HK main (b7f965b9…): the same FMallocBinned3 cut as the JP main (84e182f7…, dm/games/0100606026F12000/patches.py) at the very same site — the HK
  # build's nninitStartup (main+0x13370) is byte-identical around it: the 62 GB default at +0x137c0 (mov x8,#0xf80000000), the engine-region call at +0x137d0, and this unique bl at
  # +0x137f0 into the constructor at +0xae770 (vtable from rodata into [x0], pool base stored to main+0x7c148a0), the only occurrence of its encoding in 94 MB of text. The constructor
  # reserves 38 GB of address space for its pools; the call is cut and the host puts its allocator's vtable in the object instead (hle_uemalloc.c, layout UE5_6). Without the cut the boot
  # aborts at nnosInitialize's reserve of the guest address space on the iPhone (FATAL reserve guest AS)
  'b7f965b9d7227db8e29f04e84ea7331b': [
    (0x137f0, 'e06b0294', '1f2003d5', 'nninitStartup: bl 0xae770 (FMallocBinned3 constructor, 38 GB of address space) → nop; the host allocator is in the object already (hle_uemalloc.c)'),
  ],
}
