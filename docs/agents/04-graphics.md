# 04 — Graphics: NVN on Metal for a new title

The graphics layer records NVN commands into its own command format (`dm/gfx/nvn/hle_nvn.c`) and replays them as Metal 4 at submit time (`nvn_mtl.m`). Shaders are never translated at run time: the original Maxwell shader binaries are translated offline (Ryujinx's translator → SPIR-V → spirv-cross → metallib) and looked up by key. See `dm/gfx/README.md` for the full pipeline; this page lists what changes per title.

## 1. Find every shader container

Do not stop at the obvious archive. In Mario the main `bfsha` held 4,888 of the 12,811 shaders the game actually uses.

`dm/gfx/tools/shader_split.py <RomFS> <ExeFS main> <pkg dir> <map out> <facts out>` scans **every** RomFS file (zstd-decompressed, SARC expanded recursively) and `main`, and recognizes:

- **BNSH** (`BNSH\0\0\0\0` + `grsc` block at +0x60): inside `bfsha`, particle effects (`esetb`, VFXB), UI layouts (`blarc`), and `main` itself. Variation table at +0x80 (count at +0x7c); binary program at variation +0x10; six stage pointers from program +0x08 (vs, hs, ds, gs, ps, cs); stage code descriptor: control ptr +0x08, code ptr +0x10, code size +0x18, control size +0x1c. All pointers relative to the BNSH start.
- **BAHS** (agl `sharcb`, engine utility shaders): code region at header +0x1c (size +0x18). Binary entries in the table area are 16 bytes `{code offset, 0, control size (0x880/0x88c), signed offset from this entry to the control section}`. Control sections are shared between programs, so pair by entry, never by searching for code near a control.
- **Code area**: graphics = 0x30-byte header (magic `0x12345678`) + 0x50 SPH + instructions (at +0x80); compute = 0x30-byte header (magic `0x12345679`) + **zero padding, instructions at +0x100** (all 71 of Mario's). **Control section**: magic `0x98761234`; +0x700 = c1 offset in the code area, +0x704 = code size, +0x748 = compute thread-group size (u32 ×3), +0x754 = compute shared memory bytes.
- **The program start must be exact.** Ryujinx's decoder treats every 32-byte boundary *relative to the start* as a scheduling word. Mario's compute code was first fed from +0x30: the groups were misaligned by 0x10, instructions were dropped or misread, and 12 compute shaders (particles, post effects) translated into an endless loop that hung the GPU at the first gameplay scene. Check a new title: the first non-zero word after the header must be a control word, and the trailing `EXIT` (`e30000000007000f`) / `BRA`-to-self (`e2400fffff87000f`) must sit in instruction slots of 32-byte groups counted from your chosen start. `dmsh_build.py` now refuses compute code whose padding is not zero.

Validate a new title's coverage at run time: `DM_NVN_SHDUMP=<dir>` makes the host dump the control section and code of every program it can't find in the map; search those bytes in the game files to find the container that was missed.

## 2. Key shaders by control section + code

The host key is `sha256(control[0:0x880] + code)[:16]` (`prog_get` in `nvn_mtl.m`, `shader_split.py`). D2R keyed by the control section alone; in Mario, 6,367 shader pairs share a control section but differ in code. A different code length or layout in a new title must be reflected in both places.

## 3. Find the binding layout by census

The NVN driver puts binding tables in driver constant banks, and the layout changed between NVN versions. Don't guess: translate everything once (the translator alone takes seconds) and tabulate what the shaders reference.

| Binding | D2R (NVN 53) | Mario (NVN 55) | How it was measured |
|---|---|---|---|
| Bound texture handles (texture buffer) | `0x168 + 8 × slot`, 128 slots | **`0x20 + 8 × slot`**, 32 slots | Immediate handles of all TEX instructions: 0x20…0xF0 in steps of 8 |
| Image handles | `0x120 + 8 × slot` | `0x120 + 8 × slot` | agl compute shaders: 0x120 / 0x128 / 0x130 |
| Separate samplers | `0x568 + 8 × slot` | not used | — |
| Uniform buffers | `c[3 + slot]` | same | |
| Storage buffers | `c0[base + 16 × slot]`, base vs 0x110 / cs 0x310 / ps 0x510 | same | |

`dmsh_build.py` (`TEX_BASE`, `IMG_BASE`, `decode_tex`) and `Program.cs` (`TexBase`) hold these constants; the replay layer fills driver banks with the same layout (`NVN_RB_DRV2` in `encode_args`). After a full build, the summary must say `绑定没解出槽号的着色器 0` (no unresolved bindings).

## 4. Bindless textures

Mario's shaders read 38 % of their textures through handles stored in the game's own uniform buffers (`fp_t_cb7_2A` = handle at word 0x2A of c[7] = uniform slot 4). These became binding kinds `tex_ubo` / `samp_ubo` (`NVN_RB_TEXU` / `NVN_RB_SAMPU`): at replay the host reads the 32-bit word from the bound uniform buffer (low 20 bits texture ID, high 12 bits sampler ID).

Consequence: any optimization that infers "which textures a draw reads" from bound slots is wrong for such shaders. The TBDR optimizations (store / load elision, pass merging) are therefore off by default in this repository (`tbdr_on()`, `DM_NVN_TBDR=1` to enable).

## 5. Depth convention

`nvnDeviceSetDepthMode` near = −w (GL, z in [−w, w]) versus near = 0 (D3D / Metal). Metal clips anything with z < 0. Mario uses the GL mode; the translator must rewrite z' = z/2 + w/2 at the end of every vertex shader (`QueryGraphicsState` depth mode true + `QueryHostSupportsDepthClipControl() => false` in `Program.cs`). Symptom when wrong: the full-screen copy into the window draws nothing, so the window stays black even with a solid-colour fragment shader.

## 6. Other per-title surprises seen in Mario

- **8-bit index buffers**: Metal has only 16/32-bit indices. Widened to 16-bit at replay into the frame arena. Mario's full-screen passes and its final copy use them.
- **Texture swizzle**: `TextureBuilderSetSwizzle` / `TextureViewSetSwizzle` are applied with Metal swizzled texture views (NVN and Metal channel enums have the same values). The font atlas is `(1, 1, 1, R)`; without it glyphs render as white boxes.
- **Transient render targets**: the game builds new texture objects every frame over the same storage and never finalizes them. Host textures are shared by `(pool, offset, description)`; otherwise each new object allocates a new private Metal texture (≈13 MB/frame leak).
- **Inline uniform updates** (`CommandBufferUpdateUniformBuffer`) are replayed as in-order GPU copies; reading the uniform buffer from the CPU at encode time gives the frame's *last* value, not the draw's.
- **Compute code areas have a header** in Mario (`0x12345679`), not in D2R; `dmsh_build.py` strips it.
- **Virtual memory pools** (flag 0x200): aliased onto the physical pool named by `MapVirtual` when all requests share one base (Mario's case); otherwise unimplemented.
- **SPIRV-Cross failures**: 2 agl post-effect compute shaders still produce SPIR-V that SPIRV-Cross can't parse (`nullptr` exception). Before the compute start-offset fix it was 7: misdecoded code produced the bad SPIR-V.
- **CPU-written linear textures**: a texture with `NVN_TEXTURE_FLAGS_LINEAR` in a CPU-visible pool is created on the pool's `MTLBuffer` (`tex_linear`), so CPU writes (Mario's glyph cache) are visible to the GPU. Block-linear CPU-written textures still need the packaged-texture re-upload path.
- **Vertex formats**: Mario uses `RGB10A2SN` (0x69) → `MTLVertexFormatInt1010102Normalized`. Unmapped formats fall back to a 4-component read and print `顶点属性格式 … 未映射` once.
- **Instanced array draws** (`DrawArraysInstanced`) carry instance count and base instance in the `OP_DRAW` record.

## 7. Diagnostic switches (host)

| Switch | Output |
|---|---|
| `DM_NVN_SHOT=f1,f2:path_%d.png` | Window screenshots at those presented frames |
| `DM_NVN_PASSDUMP=f` or `f1-f2` (+ `DM_NVN_PASSDUMP_ALL=1`) | Every render pass of those frames: `P` attachments, `K` shader keys, `B` each binding's actual source, `d`/`D` draw state, `Y` inline updates, `X` texture clears |
| `DM_NVN_TEXDUMP=f:path_%d.png` (+ `DM_NVN_TEXSNAP=i,…`) | Every colour render target of frame f, as PNG |
| `DM_NVN_CBDUMP=dir` (with PASSDUMP) | Raw uniform buffer contents per binding |
| `DM_NVN_HILITE=fskey,…` | Replace those fragment shaders with solid colours: tells "geometry is wrong" from "shading is wrong" |
| `DM_NVN_SHDUMP=dir` | Control + code of programs missing from the shader map |
| `DM_NVN_ALLOCPROF=1` | Metal allocation growth per replayed op type and per function, command allocator sizes, new textures |
| `MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=nslog` | Metal validation without aborting |

A first-frame bring-up that worked for Mario: screenshot is black → `TEXDUMP` shows which targets have content → `PASSDUMP` the pass that writes the window → `HILITE` its fragment shader → `CBDUMP` its uniforms → read the translated `.metal` of its vertex shader.
