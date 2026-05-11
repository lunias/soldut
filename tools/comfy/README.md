# tools/comfy/ — Soldut chassis-atlas asset pipeline

The M5 art pipeline for the per-chassis mech sprite atlases
(`assets/sprites/<chassis>.png`).

**As of P15 (revised), the shipping path is `extract_gostek.py` — it
slices a Soldat-style "gostek part sheet" straight into the atlas.**
The AI-diffusion-canonical pipeline (ComfyUI: skeleton + style anchor →
SDXL + ControlNet + IP-Adapter → 17-tile crop) is still here, but it's
the *alternative* path now — it plateaued below the art bar (see
`TRADE_OFFS.md` → "Chassis art is hand-authored gostek part sheets, not
the AI-diffusion pipeline" and `documents/m5/11-art-direction.md` for
the design rationale of *what* the art should look like). Whether the
team keeps ComfyUI in the loop to *produce* gostek sheets — and what
workflow does it best — is open.

ComfyUI install (for the diffusion path): `~/ComfyUI` on the original
dev box, `D:\ComfyUI` on the current asset-gen box.

---

## The shipping path — gostek part-sheet extraction

A **gostek part sheet** is a single reference image
(`tools/comfy/gostek_part_sheets/<chassis>_gostek_v*.png`) with all 22
mech body parts laid out, each one flat-shaded, black-outlined, and
captioned `N. PART_NAME` in reading order:

```
1. TORSO   2. HIP_PLATE   3. HEAD
4. SHOULDER_L   5. SHOULDER_R
6. UPPER_ARM_L   7. UPPER_ARM_R   8. FOREARM_L   9. FOREARM_R   10. HAND_L   11. HAND_R
12. UPPER_LEG_L   13. UPPER_LEG_R
14. LOWER_LEG_L   15. LOWER_LEG_R
16. FOOT_L   17. FOOT_R
18.-22. STUMP_CAP ×5
```

This is the layout Soldat itself uses — its "gostek" model is per-part
rigid skinning, each sprite hooked to a skeleton point with a
position-point / rotation-reference / anchor (see
[`wiki.soldat.pl/index.php/Mod.ini`](https://wiki.soldat.pl/index.php/Mod.ini),
[`urraka.github.io/soldat-gostek`](https://urraka.github.io/soldat-gostek/),
and OpenSoldat's `client/GostekRendering.pas` /
`shared/mechanics/Sprites.pas`). Soldut's renderer
(`src/render.c::draw_mech_sprites` walking the 17-entry
`g_render_parts[]` z-order over `src/mech_sprites.c::s_default_parts`)
is the same shape — so a clean part sheet *is* the input the renderer
wants, no detailed-illustration crop needed.

How you produce the sheet is up to you — hand-drawn, traced, or
generated out-of-repo (Trooper's `trooper_gostek_v1.png` was
team-supplied). It just has to be: 22 distinct shapes on a white field,
each shape ≥ ~12 px in its smallest dimension and ≥ 0.25 % of the
image area, captioned in the order above, with the limbs drawn so the
*parent end is at the top* of each part shape (the renderer authors
limb sprites vertically and rotates them at draw time). 2 colours +
black outline on white is the natural register; the extractor's
optional palette snap will conform whatever you give it.

### Run it

```bash
# from the soldut repo, with the ComfyUI venv (it has PIL + numpy + scipy):
D:/ComfyUI/.venv/Scripts/python.exe tools/comfy/extract_gostek.py \
    tools/comfy/gostek_part_sheets/trooper_gostek_v1.png trooper --palette foundry

# keep the sheet's own colours instead of a 2-colour snap:
... tools/comfy/extract_gostek.py SHEET.png <chassis> --no-post
```

What it does:

1. **Detect** — `scipy.ndimage.label` over the non-white pixels;
   keep components ≥ 0.25 % of the image and ≥ 12 px min-dimension
   (this drops the caption text and any stray rules). Expects exactly
   22; warns and proceeds if not.
2. **Order** — band-cluster the 22 bboxes by top edge into reading
   rows, sort left→right within each row, concatenate rows top→bottom
   → caption sequence 1..22 → part name → `s_default_parts` slot
   (`NAME_TO_SLOT`; the 5 `STUMP_CAP`s fill the 5 stump slots in
   order).
3. **Crop + key** — crop each shape tight (+3 px pad), then key the
   white field transparent by flooding from the tile border
   (`scipy.ndimage.binary_propagation`) so an *enclosed* white panel
   inside a part stays opaque.
4. **Orient** — `SLOT_FIX` applies the per-slot rotate/flip needed to
   land the part in its atlas slot: forearms are drawn horizontally in
   the sheet but the atlas slot is vertical → `ROTATE_270`; the head
   gets `FLIP_TOP_BOTTOM` because the renderer's NECK→HEAD bone takes a
   −180° render rotation.
5. **Resize** — LANCZOS to the slot's `(draw_w, draw_h)` from the
   `ATLAS` table (which mirrors `s_default_parts`).
6. **Post** (unless `--no-post`) — 2-colour palette snap via
   `crop_canonical.post_process_tile` (Foundry `#1A1612` dark +
   `#D8731A` accent by default; `--palette` picks another map's pair).
   **No Bayer/halftone is baked in** — the runtime `halftone_post`
   shader does the screen at framebuffer resolution, so baking it into
   the sprite would double-dither.
7. **Composite** — `alpha_composite` each tile at its `(dst_x, dst_y)`
   from `ATLAS`. Because `ATLAS` mirrors `s_default_parts`, the output
   loads correctly with **no transcribe pass**.

Outputs:

- `assets/sprites/<chassis>.png` — the 1024×1024 RGBA atlas the game
  loads.
- `tools/comfy/output/<chassis>_gostek_atlas_preview.png` — the source
  sheet with detected/assigned boxes drawn on, plus the packed atlas
  at 2× in the corner. Eyeball this to confirm every part landed in
  the right slot with the right orientation.
- `tools/comfy/gostek_part_sheets/<chassis>_gostek_manifest.txt` — the
  reproducibility record (source file + size + palette + per-part src
  bbox → dst). This one is tracked; the preview lives under the
  gitignored `output/`.

### Tuning

- **A part landed rotated/mirrored wrong** (e.g. a foot upside-down, a
  forearm sideways) — adjust that slot's entry in `SLOT_FIX` in
  `extract_gostek.py` (`{"rot": ±90|180}` and/or `{"flipv": True}`),
  re-run, re-check the preview.
- **The detector found ≠ 22 shapes** — usually a caption merged into a
  part (lower `min_area_frac` won't help; tighten the gap between
  caption and shape in the sheet) or two parts touched (add whitespace
  between them in the sheet). The reading-row band tolerance is
  `band_frac` in `order_by_reading` (fraction of image height) — bump
  it if parts in the same visual row are being split across rows.
- **The 2-colour snap is too aggressive / not what you want** —
  `--no-post` keeps the sheet's colours; or add a palette to
  `crop_canonical.PALETTES` and pass `--palette <name>`.

### Then log it

Add a row to [`documents/art_log.md`](../../documents/art_log.md) and
an entry to [`assets/credits.txt`](../../assets/credits.txt) under
"SPRITES — chassis". For a gostek-extracted sprite the workflow/LoRA/
seed columns don't apply — note `tools/comfy/extract_gostek.py` + the
source sheet path + the palette, and record the sheet's *own*
provenance (who drew/generated it, from what, under what licence) if
known.

---

## The alternative path — AI-diffusion canonical (ComfyUI)

> Kept for reference and in case a future model / Soldut-trained LoRA
> closes the quality gap. **Not** the shipping path. If you just want
> chassis art, use `extract_gostek.py` above.

Full design rationale: [`documents/m5/11-art-direction.md`](../../documents/m5/11-art-direction.md).

### TL;DR — one command

After the one-time setup below, the diffusion pipeline is one command:

```bash
python3 tools/comfy/asset.py trooper            # full pipeline → in-game smoke test
python3 tools/comfy/asset.py trooper --review   # generate, show contact sheet, quit (you pick)
python3 tools/comfy/asset.py trooper --pick 3   # commit candidate #3
python3 tools/comfy/asset.py trooper --regenerate canonical  # redo just the slow stage
python3 tools/comfy/asset.py --list             # show approved + pending chassis
```

Pipeline stages, per chassis: **skeleton → anchor → canonical → stumps → crop → install → verify**.
Outputs land in `tools/comfy/output/<chassis>/` with stage-tagged
filenames. State lives in `tools/comfy/state/<chassis>.json`.

The orchestrator wraps the lower-level scripts (`run_workflow.py`,
`crop_canonical.py`, `transcribe_atlas.py`) so you don't have to chain
them manually. Drop down to the lower-level tools only when you need
to tune a single step.

## Layout

```
tools/comfy/
├── gostek_part_sheets/
│   ├── trooper_gostek_v1.png                # ← SHIPPING INPUT: the part sheet extract_gostek.py slices
│   └── <chassis>_gostek_manifest.txt        # reproducibility record written by extract_gostek.py
├── extract_gostek.py                         # ← SHIPPING PATH: gostek part sheet → assets/sprites/<chassis>.png
├── workflows/
│   └── soldut/
│       ├── mech_chassis_canonical_v1.json   # alt path: full Pipeline 1 (≥12 GB VRAM)
│       └── mech_chassis_canonical_8gb_v1.json  # alt path: 8 GB subset
├── skeletons/
│   ├── make_trooper_skeleton.py             # programmatic full-frame T-pose skeleton (diffusion path)
│   ├── prepare_skeleton.py                  # auto-detect a bare mech silhouette + stamp pivot dots/bone axes
│   ├── trooper_tpose_skeleton_v2.png        # generated skeleton (diffusion path)
│   └── trooper_joints.json                  # joint coords (feeds build_crop_table.py)
├── style_anchor/
│   └── trooper_anchor_v2.png                # AI-generated TRO-ink style anchor (diffusion path)
├── crop_tables/
│   └── trooper_crop.txt                     # 22-entry per-part bbox + pivot + rotate table (diffusion crop)
├── build_crop_table.py                      # regenerate <chassis>_crop.txt from <chassis>_joints.json
├── stumps/
│   └── trooper/                             # hand-drawn 32×32 stump PNGs (Pipeline 5; gitignored)
├── output/                                   # per-run candidate PNGs + extractor previews (gitignored)
├── state/                                    # asset.py orchestrator state (gitignored)
├── crop_canonical.py                         # shared infra: per-tile palette/post + PALETTES table; also slices a canonical → atlas
├── contact_sheet.py                          # composite candidate PNGs into one review grid
├── transcribe_atlas.py                       # emit C literal for g_chassis_sprites
├── run_workflow.py                           # drive the ComfyUI API (no UI clicks needed)
└── README.md                                 # ← you are here
```

## One-time setup (your manual work)

The runtime side of P15 is in. Before you can ship art, the local
ComfyUI instance needs models + custom nodes.

### 1. Install ComfyUI Manager

The most useful single custom node — every other install in this list
becomes a one-click install in the Manager UI.

```bash
cd ~/ComfyUI/custom_nodes
git clone https://github.com/Comfy-Org/ComfyUI-Manager.git
```

Restart ComfyUI; the **Manager** button appears in the menu.

### 2. Install required custom nodes

Either via Manager (→ "Install Custom Nodes" → search and install each)
or by `git clone`-ing into `~/ComfyUI/custom_nodes/`:

| Custom node | Repo | Why |
|---|---|---|
| `ComfyUI_IPAdapter_plus` | https://github.com/cubiq/ComfyUI_IPAdapter_plus | `IPAdapterUnifiedLoader` + `IPAdapterAdvanced` (style anchor). Required. |
| `comfyui_controlnet_aux` | https://github.com/Fannovel16/comfyui_controlnet_aux | `LineartStandardPreprocessor`. Already installed locally. |
| `rgthree-comfy` | https://github.com/rgthree/rgthree-comfy | Better seed/loop helpers. Quality-of-life. |
| `was-node-suite-comfyui` | https://github.com/WASasquatch/was-node-suite-comfyui | Image post + composite helpers. Quality-of-life. |
| `ComfyUI-PixelArt-Detector` | https://github.com/dimtoneff/ComfyUI-PixelArt-Detector | `PixelArtLoadPalettes` (used by Pipeline 3 — parallax). Required for P16+. |
| `ComfyUI_Comfyroll_CustomNodes` | https://github.com/Suzie1/ComfyUI_Comfyroll_CustomNodes | Color-grading + composite. Quality-of-life. |
| `ComfyUI-Custom-Scripts` | https://github.com/pythongosssss/ComfyUI-Custom-Scripts | Better LoRA/sampler pickers. Quality-of-life. |
| `ComfyUI-Fill-Nodes` | https://github.com/filliptm/ComfyUI_Fill-Nodes` | `FL Bayer Image Dithering` (alt halftone). Quality-of-life. |

### 3. Download model files

`mech_chassis_canonical_v1.json` (the full Pipeline 1) needs the four
files below; ~12.6 GB total. Place each into the matching
`<ComfyUI>/models/<dir>/` of your local install — on the original dev
box that's `~/ComfyUI`; on the current asset-gen box it's
`D:\ComfyUI`. (The 8 GB fallback workflow,
`mech_chassis_canonical_8gb_v1.json`, needs a different — smaller — set;
see its `_meta`.) All four are on HuggingFace, public, no auth:

```bash
cd <ComfyUI>/models
curl -L -o checkpoints/Illustrious-XL-v0.1.safetensors \
  https://huggingface.co/OnomaAIResearch/Illustrious-xl-early-release-v0/resolve/main/Illustrious-XL-v0.1.safetensors
curl -L -o controlnet/controlnet-union-sdxl-1.0_promax.safetensors \
  https://huggingface.co/xinsir/controlnet-union-sdxl-1.0/resolve/main/diffusion_pytorch_model_promax.safetensors
mkdir -p ipadapter
curl -L -o ipadapter/ip-adapter-plus_sdxl_vit-h.safetensors \
  https://huggingface.co/h94/IP-Adapter/resolve/main/sdxl_models/ip-adapter-plus_sdxl_vit-h.safetensors
curl -L -o vae/sdxl-vae-fp16-fix.safetensors \
  https://huggingface.co/madebyollin/sdxl-vae-fp16-fix/resolve/main/sdxl_vae.safetensors
```

| File on disk | Dir | Source repo (HF) | Notes |
|---|---|---|---|
| `Illustrious-XL-v0.1.safetensors` (~6.94 GB) | `models/checkpoints/` | `OnomaAIResearch/Illustrious-xl-early-release-v0` | Base checkpoint. SDXL-derived (`license:other` — commercial output OK per documents/m5/11-art-direction.md §License hygiene). |
| `controlnet-union-sdxl-1.0_promax.safetensors` (~2.51 GB) | `models/controlnet/` | `xinsir/controlnet-union-sdxl-1.0` — file `diffusion_pytorch_model_promax.safetensors`, renamed to a flat top-level filename so the workflow path has no `/` vs `\` platform difference | One ControlNet, all conditioning types (canny / lineart / scribble / …). apache-2.0. |
| `ip-adapter-plus_sdxl_vit-h.safetensors` (~0.85 GB) | `models/ipadapter/` (create the dir if missing) | `h94/IP-Adapter` — file `sdxl_models/ip-adapter-plus_sdxl_vit-h.safetensors` | Style-anchor conditioning. apache-2.0. |
| `clip_vision_h.safetensors` (~1.26 GB) | `models/clip_vision/` | OpenCLIP ViT-H/14 laion2B (any `clip_vision_h.safetensors` or `CLIP-ViT-H-14-laion2B-s32B-b79K.safetensors`) — the image encoder IP-Adapter PLUS SDXL requires | If you don't have it, grab `sdxl_models/image_encoder/model.safetensors` from `h94/IP-Adapter` and rename. |
| `sdxl-vae-fp16-fix.safetensors` (~0.33 GB) | `models/vae/` | `madebyollin/sdxl-vae-fp16-fix` — file `sdxl_vae.safetensors`, renamed | fp16-safe VAE. mit. |

The spec's LoRA stack (`battletech_battlemechs` @0.6 / `super_robot_diffusion_xl`
@0.4 / `lineart_sdxl_v3` @0.5 — all on Civitai, gated) is **not wired**
into the shipped workflow; the chassis output is currently
prompt + ControlNet + IP-Adapter + Pipeline 7 post only. To add it,
splice a `LoraLoader` chain between node 1 (`CheckpointLoaderSimple`)
and node 23 (`IPAdapterAdvanced`) on the MODEL line and re-point the
two `CLIPTextEncode` nodes (30, 31) at the last `LoraLoader`'s CLIP —
the workflow `_meta.description` notes this. A custom Soldut-trained
LoRA (per documents/m5/11-art-direction.md §"Escalation path 1") is
the better long-term lever once ~30+ approved sprites exist.

The workflow JSON references each file by the exact name above. If you
download something with a different name, fix the workflow's
`ckpt_name` / `control_net_name` / `ipadapter_file` / `clip_name` /
`vae_name` strings (or `lora_name` if you add LoRAs).

### 4. T-pose skeleton (the ControlNet input)

The diffusion path needs a 1024×1024 T-pose skeleton image with magenta
pivot dots. Two ways to get one:

- **`make_trooper_skeleton.py`** — programmatic full-frame mech anatomy
  (helmet+visor, panel-lined torso with power core, hip plate, shoulder
  pads, gauntlets, boots, 16 magenta pivot dots). A self-contained
  starting point: `python tools/comfy/skeletons/make_trooper_skeleton.py`.
- **`prepare_skeleton.py`** — if you already have a *bare* mech
  silhouette (no dots), this auto-detects the silhouette
  (frame-aware flood-fill so a decorative border doesn't confuse it),
  derives the 16 joint positions from a chassis profile
  (`PROFILE_TROOPER` and friends — head/neck/chest/pelvis rows, arm
  row at the vertical centre of the horizontal arm, leg centres from
  blob detection at the knee/foot rows), and stamps the magenta pivot
  dots + faint bone axes:
  `python tools/comfy/skeletons/prepare_skeleton.py INPUT.png trooper --clean --overlay --joints-out --size 1024`.
  Output goes to `<chassis>_tpose_skeleton_v2.png`; `--overlay` writes a
  debug overlay; `--joints-out` writes `<chassis>_joints.json` (which
  `build_crop_table.py` then turns into `<chassis>_crop.txt`).

Either way:

- **1024×1024**, white background, pure black ink at 2-3 px line
  weight.
- Trooper-sized mech in T-pose. **Arms drawn horizontally** in the
  canonical (the AI fills around the silhouette); the *atlas slots* are
  vertical, so the crop step (`crop_canonical.py`, `rotate` column) /
  the extractor (`extract_gostek.py`, `SLOT_FIX`) rotates the forearm
  tiles −90° at pack time. The renderer authors limb sprites vertically
  and rotates them at draw time.
- **Magenta dots** (RGB 255, 0, 255) at every joint particle:
  NECK, HEAD, CHEST, PELVIS, L_SHOULDER, R_SHOULDER,
  L_ELBOW, R_ELBOW, L_HAND, R_HAND, L_HIP, R_HIP,
  L_KNEE, R_KNEE, L_FOOT, R_FOOT. (16 dots total — `crop_canonical.py`
  doesn't extract these, but they're the geometric source of truth, and
  `build_crop_table.py` reads `<chassis>_joints.json` to derive the
  per-part crop rects from them.)
- **Light grey rectangles at parent-side overlap regions** (where the
  shoulder plate overlaps the upper-arm, hip plate overlaps the upper-
  leg, etc.). The AI honors these as "exposed end" markers per
  [`documents/m5/12-rigging-and-damage.md`](../../documents/m5/12-rigging-and-damage.md)
  §"Overlap zones".
- **Ragged silhouette edges** at the same overlap regions — wavy
  borders so the AI's continuation reads as torn metal where parent
  meets child.
- If you move joint coords, regenerate the crop table:
  `python tools/comfy/build_crop_table.py trooper` (reads
  `trooper_joints.json`, writes `trooper_crop.txt` + a
  `<stem>_flat.png` with the canonical's background flattened to white).

### 5. (Optional) Style anchor

The IP-Adapter step conditions on style, not geometry, so anything
that reads as ink-on-paper Battletech-TRO works as the anchor:

- A pencil sketch from your notebook.
- A page from a Battletech 3025/3039 TRO photographed at high
  contrast (study only — don't ship the source).
- The shipped `tools/comfy/style_anchor/trooper_anchor_v2.png`
  (itself AI-generated, best-of-4 from the prompt-only
  `style_anchor_v1.json` workflow). The canonical output inherits the
  anchor's level of detail — better anchor = better output.

The proper iteration is **anchor recursion**: run the workflow, pick
the best of 8 candidates as `<chassis>_anchor_v3.png`, re-run, pick
again as `_v4.png`, and so on until the output converges. (In practice
this is what hit the ceiling — see `TRADE_OFFS.md`. The gostek path
sidesteps it.)

## Drive the workflow

You have two paths:

### Path A — Drive via the API (preferred, scriptable)

```bash
# Start ComfyUI:
cd ~/ComfyUI && python main.py --listen 127.0.0.1 --port 8188

# In another shell, from the soldut repo:
python3 tools/comfy/run_workflow.py \
    --workflow tools/comfy/workflows/soldut/mech_chassis_canonical_v1.json \
    --skeleton tools/comfy/skeletons/trooper_tpose_skeleton_v2.png \
    --anchor   tools/comfy/style_anchor/trooper_anchor_v2.png \
    --seed 1000001 --batch 8
```

The driver:
1. POSTs the skeleton + anchor PNGs to `/upload/image`.
2. Patches the workflow's two `LoadImage` nodes to point at the uploads.
3. Submits to `/prompt` and polls `/history` until done.
4. Downloads each output into `tools/comfy/output/<prefix>_<filename>.png`.

Re-run with different seeds to explore the anchor convergence quickly:

```bash
python3 tools/comfy/run_workflow.py \
    --workflow tools/comfy/workflows/soldut/mech_chassis_canonical_v1.json \
    --skeleton tools/comfy/skeletons/trooper_tpose_skeleton_v2.png \
    --anchor   tools/comfy/style_anchor/trooper_anchor_v2.png \
    --seed 1000001 --batch 8 --prefix trooper_iter2
```

### Path B — Drive via the ComfyUI web UI

If you'd rather click through it (or you want to live-tune a node):

1. Open `http://127.0.0.1:8188` in a browser.
2. Drag `tools/comfy/workflows/soldut/mech_chassis_canonical_v1.json`
   onto the canvas. (Recent ComfyUI builds auto-convert API-format
   JSON to a graph. If yours doesn't, click **Load** and pick the
   file from the dialog.)
3. Hover over the two `LoadImage` nodes (titled "Skeleton" and
   "Style anchor"); pick your real PNGs from the dropdowns. To upload
   new ones, drag them onto the node.
4. Click **Queue Prompt**.
5. Outputs land at `~/ComfyUI/output/soldut/trooper_candidates_*.png`.
   Copy the keepers to `tools/comfy/output/`.

## Iterate until a candidate locks

Pick the best of 8 by visual inspection against the
[`documents/m5/11-art-direction.md`](../../documents/m5/11-art-direction.md)
§"Defeating the AI tells" checklist:

| AI tell to scan for | What to look at |
|---|---|
| Smeared text on plates | Should be **none** (negative prompt + post-render stencils). If you see text, increase the `text` weight in the negative. |
| Soft glow around silhouette | Bump `cfg` to 8.0 or add `glow, bloom` to the negative. |
| Inconsistent panel-line styles | Lower IP-Adapter `weight` to 0.65; the AI is drifting. Or improve the anchor. |
| Plausible-but-wrong hydraulics | Increase ControlNet `strength` from 0.92 toward 0.96; the skeleton is getting overridden. |
| Generic indie palette | The 2-color remap in `crop_canonical.py` will fix this on extraction; don't worry yet. |
| Anime-cute face on helmet | Heavy negative prompt; if it persists drop `super_robot_diffusion_xl` weight to 0.2. |

When a candidate locks, save it as
`tools/comfy/style_anchor/trooper_anchor_v2.png` (or v3, v4...) and
**bump the workflow file's `_meta.version`**.

## Crop the approved canonical into an atlas

```bash
python3 tools/comfy/crop_canonical.py \
    tools/comfy/output/trooper_<approved-filename>.png trooper \
    --palette foundry
```

The script:
1. Reads `tools/comfy/crop_tables/trooper_crop.txt` (22 entries — incl.
   the `rotate` column for the horizontal→vertical forearm/head tiles).
2. Slices each part rectangle from the canonical, applies the per-row
   `rotate`, resizes the tile to its runtime sprite size
   (`draw_w × draw_h`).
3. Pulls hand-drawn stump caps from `tools/comfy/stumps/trooper/`
   if present (otherwise uses the placeholder corner row from the
   canonical).
4. Post: alpha-key the white background (border-flood so enclosed
   white panels survive), then — unless `--no-post` — a **flat
   2-colour luminance snap** to the map palette (`#1A1612` dark +
   `#D8731A` accent for Foundry) with a nearest-neighbor half-size
   round-trip to kill AI smoothness. (No Bayer/halftone is baked in:
   the runtime `halftone_post` shader screens at framebuffer res, so
   baking it here would double-dither.)
5. Composites everything into a 1024×1024 atlas at the same dst
   positions as `src/mech_sprites.c::s_default_parts` — so no
   transcribe pass is needed.
6. Writes `assets/sprites/trooper.png` + an annotated preview at
   `tools/comfy/output/trooper_atlas_preview.png`.

(`crop_canonical.py` is also imported by `extract_gostek.py` for its
`PALETTES` table + `post_process_tile` helper — same per-tile post,
shared between the two paths.)

If the canonical has the head right-side-up (and you want the head
sprite to render correctly without re-tuning the runtime rotation
math), pass `--flip-head`.

## (Optional) Re-transcribe the parts table

The default `trooper_crop.txt`'s `dst_*` columns mirror `s_default_parts`
in `src/mech_sprites.c`, so the Trooper renders correctly out of the
box once the atlas is in place. **You only need this step if you tune
part sizes or pivots away from the defaults.**

```bash
python3 tools/comfy/transcribe_atlas.py trooper > /tmp/trooper_atlas.c
```

The script emits a C literal you paste into `src/mech_sprites.c`,
replacing the `s_default_parts` `memcpy` for `CHASSIS_TROOPER` only.
Other chassis keep `s_default_parts` until their atlases ship.

## Verify in-game

```bash
make
./soldut --host
```

The startup log lists each chassis atlas: `mech_sprites: loaded
assets/sprites/trooper.png (1024x1024) for chassis trooper` if
loaded; otherwise it falls back to capsules with an INFO line.

For a deterministic visual check: use the existing P10 shot test:

```bash
./soldut --shot tests/shots/m5_chassis_distinctness.shot
```

Open `build/shots/m5_chassis_distinctness/*.png` — Trooper should
render as a sprite (with the hand-drawn skeleton's silhouette + the
AI-filled surface, post-processed into the foundry palette); the
other four chassis still render as capsules until P16.

## Log the approval

Add a row to **`documents/art_log.md`**:

```
| trooper.png | mech_chassis_canonical_v1.json | battletech 0.6 / SRD 0.4 / lineart 0.5 | trooper_anchor_v2.png | 1000001 | v1 | YYYY-MM-DD |
```

And add the corresponding entry to **`assets/credits.txt`** under the
"SPRITES — chassis" section. The example commented row at the bottom
of `credits.txt` is the canonical format.

## Hand-drawn stump caps (Pipeline 5)

Per [`documents/m5/11-art-direction.md`](../../documents/m5/11-art-direction.md)
§"Pipeline 5", the 25 stump caps (5 per chassis × 5 chassis) are
hand-drawn — AI is bad at "the inside of a torn-off mech limb."

For Trooper, drop these five PNGs (32×32, RGBA) into
`tools/comfy/stumps/trooper/`:

- `stump_shoulder_l.png`
- `stump_shoulder_r.png`
- `stump_hip_l.png`
- `stump_hip_r.png`
- `stump_neck.png`

`crop_canonical.py` picks them up automatically the next time you
run it. They don't have to land in P15 — the placeholder corner row
in the canonical is good enough for integration; P16 or P17 can
catch up the hand-drawn pass.

When they land, log each in `documents/art_log.md` with
`Workflow = hand-drawn` and add an entry to `assets/credits.txt`
under "SPRITES — stump caps".

## Troubleshooting

**ComfyUI errors with `Failed to validate prompt for output 70`**
The model files aren't at the paths the workflow expects. Open a
ComfyUI shell + run `ls ~/ComfyUI/models/checkpoints/` etc. and
match the filenames against the workflow JSON.

**`CLIPVisionLoader` errors with `clip_vision_h.safetensors` not in list**
The full workflow uses explicit `IPAdapterModelLoader` +
`CLIPVisionLoader` nodes (not `IPAdapterUnifiedLoader`), referencing
`ip-adapter-plus_sdxl_vit-h.safetensors` and `clip_vision_h.safetensors`
by exact name. If you don't have the ViT-H image encoder, grab
`sdxl_models/image_encoder/model.safetensors` from `h94/IP-Adapter`,
drop it in `<ComfyUI>/models/clip_vision/`, rename it (e.g. to
`clip_vision_h.safetensors`), and update node 22's `clip_name` if you
named it something else.

**`ControlNetLoader` errors with `controlnet-union-...` not in list**
The full workflow expects a flat filename
`controlnet-union-sdxl-1.0_promax.safetensors` directly in
`<ComfyUI>/models/controlnet/` (no subdir — the original repo file is
`diffusion_pytorch_model_promax.safetensors` inside the
`xinsir/controlnet-union-sdxl-1.0` repo; rename it on download). If you
keep the subdir/original name, fix node 12's `control_net_name` to
match exactly what `/object_info/ControlNetLoader` lists (ComfyUI on
Windows reports `\` separators, on Linux `/` — the flat filename
sidesteps that).

**Output looks like a generic SDXL render, not Battletech-TRO**
Either the LoRAs failed to load or the IP-Adapter weight is too low.
Check `~/ComfyUI/output/` for a log file; or open the queue in the UI
and look for a red border on the LoraLoader nodes.

**Output has the silhouette wrong**
The hand-drawn skeleton's geometry isn't being honored — `strength`
on the ControlNetApplyAdvanced node is too low. Bump from 0.92 to
0.96 or 0.98.

**Output renders as capsules even though `assets/sprites/trooper.png`
exists**
Run with `LOG_LEVEL=info ./soldut --host` and look for the
`mech_sprites: loaded` line. If it says `failed to load`, the PNG
might be 0 bytes (truncated download) or in a format raylib can't
parse. Re-run `crop_canonical.py`.
