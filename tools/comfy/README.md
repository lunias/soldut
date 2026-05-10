# tools/comfy/ — Soldut ComfyUI asset pipeline

The ComfyUI side of the M5 art pipeline. P15 lands the runtime + scripts
+ the Trooper-specific workflow; the goal is one approved
`assets/sprites/trooper.png` atlas. P16 reuses the same pipeline for the
four remaining chassis + the weapon atlas + HUD icons.

Full design rationale: [`documents/m5/11-art-direction.md`](../../documents/m5/11-art-direction.md).
ComfyUI install: `~/ComfyUI` (reused; not reinstalled).

## TL;DR — one command

After the one-time setup below, the entire pipeline is one command:

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
├── workflows/
│   └── soldut/
│       └── mech_chassis_canonical_v1.json   # The locked Pipeline 1 workflow
├── skeletons/
│   ├── trooper_tpose_skeleton.png           # PLACEHOLDER — replace with hand-drawn
│   └── make_placeholder_skeleton.py          # Regenerator for the placeholder
├── style_anchor/
│   ├── trooper_anchor_v1.png                # PLACEHOLDER — replace with pencil sketch
│   └── make_placeholder_anchor.py
├── crop_tables/
│   └── trooper_crop.txt                     # 22-entry per-part bbox + pivot table
├── stumps/
│   └── trooper/                             # Drop hand-drawn 32×32 stump PNGs here (Pipeline 5)
├── output/                                   # Where run_workflow.py drops downloaded PNGs
├── crop_canonical.py                         # Slice canonical → atlas + Pipeline 7 post
├── transcribe_atlas.py                       # Emit C literal for g_chassis_sprites
├── run_workflow.py                           # Drive the API (no UI clicks needed)
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

Place each into the matching `~/ComfyUI/models/<dir>/`. Don't symlink
through `~/.local/share/ComfyUI/`; the install at `~/ComfyUI` reads
from its own subtree.

| File | Place under | Source | Notes |
|---|---|---|---|
| `illustriousXL_v20.safetensors` (~6.4 GB) | `models/checkpoints/` | https://civitai.com/models/795765/illustrious-xl | Base. Required. OpenRAIL-M. |
| `battletech_battlemechs.safetensors` | `models/loras/` | https://civitai.com/models/136009/battletech-battlemechs | Mecha-shape LoRA. Required for chassis. Check model card for license. |
| `super_robot_diffusion_xl.safetensors` | `models/loras/` | https://civitai.com/models/124747 | Optional variety; doc weights it at 0.4. |
| `lineart_sdxl_v3.safetensors` | `models/loras/` | https://civitai.com/models/539031 | Lineart bias. Strongly recommended. |
| `controlnet-union-sdxl-1.0/diffusion_pytorch_model_promax.safetensors` (~2.5 GB) | `models/controlnet/controlnet-union-sdxl-1.0/` | https://huggingface.co/xinsir/controlnet-union-sdxl-1.0 | Required. One ControlNet for canny / lineart / scribble. |
| `ip-adapter-plus_sdxl_vit-h.safetensors` | `models/ipadapter/` (create the dir) | https://huggingface.co/h94/IP-Adapter | Required. Style-anchor conditioning. |
| `model.safetensors` (image_encoder) | `models/clip_vision/` (rename to `CLIP-ViT-H-14-laion2B-s32B-b79K.safetensors`) | https://huggingface.co/h94/IP-Adapter (image_encoder folder) | Required by IPAdapterUnifiedLoader. |
| `sdxl-vae-fp16-fix.safetensors` | `models/vae/` | https://huggingface.co/madebyollin/sdxl-vae-fp16-fix | Required. fp16-safe VAE. |

The workflow JSON references each by the exact filename above. If you
download something with a different name, fix the workflow's
`ckpt_name` / `lora_name` / `control_net_name` / `vae_name` strings.

### 4. Hand-draw a real T-pose skeleton

The placeholder at `tools/comfy/skeletons/trooper_tpose_skeleton.png`
exists so the workflow loads. **It is not the input you ship from.**

Replace it with a hand-drawn 1024×1024 PNG. Specs (per
[`documents/m5/11-art-direction.md`](../../documents/m5/11-art-direction.md)
§"Pipeline 1"):

- **1024×1024**, white background, pure black ink at 2-3 px line
  weight.
- Trooper-sized mech in T-pose. **Arms hang vertically** (not
  horizontal) — the renderer authors limb sprites vertically and
  rotates them at draw time, so the canonical needs them in their
  rest position.
- **Magenta dots** (RGB 255, 0, 255) at every joint particle:
  NECK, HEAD, CHEST, PELVIS, L_SHOULDER, R_SHOULDER,
  L_ELBOW, R_ELBOW, L_HAND, R_HAND, L_HIP, R_HIP,
  L_KNEE, R_KNEE, L_FOOT, R_FOOT. (16 dots total — `crop_canonical.py`
  doesn't extract these but they're the geometric source of truth.)
- **Light grey rectangles at parent-side overlap regions** (where the
  shoulder plate overlaps the upper-arm, hip plate overlaps the upper-
  leg, etc.). The AI honors these as "exposed end" markers per
  [`documents/m5/12-rigging-and-damage.md`](../../documents/m5/12-rigging-and-damage.md)
  §"Overlap zones".
- **Ragged silhouette edges** at the same overlap regions — wavy
  borders so the AI's continuation reads as torn metal where parent
  meets child.
- Match the joint coords in
  `tools/comfy/skeletons/make_placeholder_skeleton.py`. If you change
  them, also update `tools/comfy/crop_tables/trooper_crop.txt`.

The placeholder regenerator (`make_placeholder_skeleton.py`) is a
recoverable starting point if you delete the PNG by accident.

### 5. (Optional) Hand-draw a style anchor

The IP-Adapter step conditions on style, not geometry, so anything
that reads as ink-on-paper Battletech-TRO works as the v1 anchor:

- A pencil sketch from your notebook.
- A page from a Battletech 3025/3039 TRO photographed at high
  contrast (study only — don't ship the source).
- The placeholder anchor at
  `tools/comfy/style_anchor/trooper_anchor_v1.png` (a crude rectangle
  silhouette + halftone hatching). It works, but the canonical output
  inherits its level of detail — better anchor = better output.

The proper iteration is **anchor recursion**: run the workflow with the
v1 anchor, pick the best of 8 candidates as `trooper_anchor_v2.png`,
re-run, pick again as `_v3.png`, and so on until the output converges.

## Drive the workflow

You have two paths:

### Path A — Drive via the API (preferred, scriptable)

```bash
# Start ComfyUI:
cd ~/ComfyUI && python main.py --listen 127.0.0.1 --port 8188

# In another shell, from the soldut repo:
python3 tools/comfy/run_workflow.py \
    --workflow tools/comfy/workflows/soldut/mech_chassis_canonical_v1.json \
    --skeleton tools/comfy/skeletons/trooper_tpose_skeleton.png \
    --anchor   tools/comfy/style_anchor/trooper_anchor_v1.png \
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
    --skeleton tools/comfy/skeletons/trooper_tpose_skeleton.png \
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
1. Reads `tools/comfy/crop_tables/trooper_crop.txt` (22 entries).
2. Slices each part rectangle from the canonical.
3. Pulls hand-drawn stump caps from `tools/comfy/stumps/trooper/`
   if present (otherwise uses the placeholder corner row from the
   canonical).
4. Applies Pipeline 7 post: alpha-key the white background, run a
   Bayer-8 halftone screen, remap to the foundry 2-color palette
   (`#1A1612` dark + `#D8731A` accent), nearest-neighbor
   round-trip downsample to kill any AI smoothness.
5. Composites everything into a 1024×1024 atlas at the same dst
   positions as `src/mech_sprites.c::s_default_parts` — so no
   transcribe pass is needed.
6. Writes `assets/sprites/trooper.png` + an annotated preview at
   `tools/comfy/output/trooper_atlas_preview.png`.

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

**`IPAdapterUnifiedLoader` errors with `clip_vision not found`**
Move `model.safetensors` from IP-Adapter's `image_encoder/` directory
into `~/ComfyUI/models/clip_vision/` and rename to
`CLIP-ViT-H-14-laion2B-s32B-b79K.safetensors` (or whatever the node
expects — check the IPAdapter_plus README).

**`controlnet-union` errors with `unable to load control net`**
The Union ControlNet ships in a subdirectory; the path in the
workflow is
`controlnet-union-sdxl-1.0/diffusion_pytorch_model_promax.safetensors`.
Check `~/ComfyUI/models/controlnet/controlnet-union-sdxl-1.0/` for the
file. If yours is named differently, fix the workflow's
`control_net_name`.

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
