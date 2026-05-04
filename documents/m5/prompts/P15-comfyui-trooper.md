# P15 — ComfyUI setup + first chassis (Trooper) generation

## What this prompt does

This is mostly a **you-driven** prompt — the model can't run ComfyUI's GUI. Claude's job here is to:

1. Set up the workflow JSONs and skeleton inputs.
2. Write the build scripts that crop the canonical T-pose into the per-part atlas + transcribe pivots into `g_chassis_sprites[CHASSIS_TROOPER]`.
3. Write the iteration log structure.
4. Tell **you** what to do interactively in ComfyUI.

After this prompt, you have a working Trooper atlas at `assets/sprites/trooper.png` plus the locked workflow + style anchor for chassis 2-5 (P16).

Depends on P10 (atlas runtime expects `assets/sprites/<chassis>.png`).

## Required reading

1. `CLAUDE.md`
2. **`documents/m5/11-art-direction.md`** — the whole doc, especially §"The ComfyUI workflow", §"Pipeline 1 — Mech chassis canonical T-pose", §"Defeating the AI tells", §"The mandatory iteration log", §"License hygiene"
3. `documents/m5/12-rigging-and-damage.md` §"Per-chassis bone structures and silhouettes" — what the Trooper sprite has to look like

## Background

ComfyUI is installed locally on your machine (the user has stated this). Claude can't drive it; this prompt has Claude prepare the surrounding infrastructure (workflow files, skeleton inputs, crop tooling, atlas wire-up) while telling **you** the steps to take in ComfyUI itself.

After P15, the same workflow generates 4 more chassis in P16.

## Concrete tasks for Claude

### Task 1 — Set up `tools/comfy/` directory

Create:

```
tools/comfy/
├── workflows/
│   └── soldut/
│       └── mech_chassis_canonical_v1.json   # ComfyUI workflow
├── skeletons/
│   └── trooper_tpose_skeleton.png            # placeholder hand-drawn T-pose
├── style_anchor/
│   └── trooper_anchor_v1.png                 # placeholder; you pick the real one
├── crop_tables/
│   └── trooper_crop.txt                      # per-part bbox + pivot
└── README.md                                 # how to drive the workflow
```

### Task 2 — Workflow JSON skeleton

Write `tools/comfy/workflows/soldut/mech_chassis_canonical_v1.json` per the spec at `documents/m5/11-art-direction.md` §"Pipeline 1 — Mech chassis canonical T-pose":

- CheckpointLoaderSimple → illustriousXL_v20.safetensors
- LoraLoader chain: battletech_battlemechs (0.6), super_robot_diffusion_xl (0.4), lineart_sdxl_v3 (0.5)
- LoadImage → skeletons/trooper_tpose_skeleton.png → LineartStandardPreprocessor → ControlNetApplyAdvanced (strength 0.92)
- LoadImage → style_anchor/trooper_anchor_v1.png → IPAdapterUnifiedLoader → IPAdapterAdvanced (weight 0.75)
- CLIPTextEncode positive: per the doc
- CLIPTextEncode negative: per the doc
- KSampler (steps 30, cfg 7.5, dpmpp_2m_sde, karras, seed=1000001, batch_size=8)
- VAEDecode → SaveImage to `tools/comfy/output/trooper_candidates_*.png`

You'll write this as raw ComfyUI workflow JSON — describe each node + its inputs + the `links` connections. Use the standard ComfyUI workflow JSON format (an array of nodes plus a links array).

If you can't infer the exact JSON shape, use ComfyUI's documented format from https://github.com/comfyanonymous/ComfyUI as reference and produce the best approximation. The user will verify by loading it in ComfyUI's UI.

### Task 3 — Skeleton placeholder

Write `tools/comfy/skeletons/trooper_tpose_skeleton.png` as a placeholder. Since Claude can't draw, use ImageMagick to produce a 1024×1024 image with:

- White background
- Black ovals + lines forming a stick-figure T-pose mech silhouette
- Magenta dots at every pivot point (per `documents/m5/12-rigging-and-damage.md` §"Per-weapon visible art" — note that for chassis it's per-bone pivot)

Document in `tools/comfy/README.md` that the user **must replace this placeholder** with a hand-drawn skeleton (see Pipeline 1 in 11-art-direction.md). The placeholder is to ensure the workflow runs at all.

### Task 4 — Crop table format

Write `tools/comfy/crop_tables/trooper_crop.txt` (text format — Claude can edit later as the chassis bone lengths get tuned):

```
# part_id   src_x  src_y  src_w  src_h  pivot_x  pivot_y  draw_w  draw_h
torso       80     220    56     72     28       36       56      72
head        100    160    40     40     20       20       40      40
hip_plate   72     320    64     36     32       18       64      36
shoulder_l  60     180    56     40     28       20       56      40
shoulder_r  ...
arm_upper_l 40     400    32     80     16       10       32      80
... (22 entries total — 12 visible parts + 5 stump caps + 5 helper)
```

Coordinates are sized to match a Trooper canonical T-pose generated at 1024×1024 (the part rectangles fit within that). Tune per the bone-length distinctness table.

### Task 5 — Crop tool

Write `tools/comfy/crop_canonical.sh` that takes a canonical PNG + a crop table + an output directory, slices each part, and packs them into `assets/sprites/<chassis>.png` via rTexPacker.

Or, simpler if rTexPacker isn't reliably automatable: use ImageMagick to crop each part into a separate file, then concatenate them into a single 1024×1024 atlas with `magick montage`.

### Task 6 — Atlas table generator

Write `tools/comfy/transcribe_atlas.sh` (or a small C helper `tools/comfy/transcribe_atlas.c`) that reads the crop table + the packed atlas's actual sub-rect positions and emits a `g_chassis_sprites[CHASSIS_TROOPER].parts[]` literal that's pasted into `src/mech_sprites.c`.

### Task 7 — `documents/art_log.md`

Initialize the art log per `documents/m5/11-art-direction.md` §"The mandatory iteration log":

```markdown
# Soldut art log

| Asset | Workflow | LoRA stack | IP-Adapter | Seed | Prompt-version | Approved |
|---|---|---|---|---|---|---|
```

Empty for now; you fill rows as you approve assets.

### Task 8 — `assets/credits.txt`

Initialize per `documents/m5/11-art-direction.md` §"License hygiene":

```
# Soldut credits — assets

ART (AI-assisted via ComfyUI)
Some 2D background and concept assets generated with the assistance of
Stable Diffusion XL (Illustrious-XL fine-tune, OpenRAIL-M license) via
ComfyUI, with substantial post-generation editing, palette mapping,
compositing, and integration by the developer.

FONTS
Atkinson Hyperlegible — Braille Institute — OFL
VG5000 — Velvetyne — OFL
Steps Mono — Velvetyne — OFL

ASSETS REGISTRY
(populated as assets land)
```

### Task 9 — `tools/comfy/README.md` — your interactive instructions

Write step-by-step instructions for the user driving ComfyUI:

1. Install custom nodes (per `documents/m5/11-art-direction.md` §"Custom nodes worth installing").
2. Download model files (per the doc's table of URLs).
3. Hand-draw a real `trooper_tpose_skeleton.png` to replace the placeholder. Specs: 1024×1024, pure black on white, magenta dot at each pivot, ragged silhouette at each parent-side overlap region for "exposed end" markers.
4. Pick or hand-draw the first style anchor as `trooper_anchor_v1.png` (a pencil sketch of the desired surface aesthetic).
5. Open the workflow JSON in ComfyUI.
6. Generate 8 candidates. Inspect.
7. Iterate prompt + LoRA weights until a candidate locks. Save it as `style_anchor/trooper_anchor_v2.png`.
8. Re-run the workflow with the v2 anchor for an approval pass.
9. Run `tools/comfy/crop_canonical.sh trooper_canonical_approved.png trooper_crop.txt assets/sprites/`.
10. Run `tools/comfy/transcribe_atlas.sh > /tmp/trooper_atlas.c` and paste the contents into `src/mech_sprites.c`.
11. `make && ./soldut --host` and verify the Trooper renders as a sprite, not a capsule.
12. Add a row to `documents/art_log.md`.
13. Add an entry to `assets/credits.txt`.

### Task 10 — Hand-drawn stump caps (5)

Hand-drawn (no AI), per `documents/m5/11-art-direction.md` §"Pipeline 5 — Hand-drawn stump caps" + `documents/m5/12-rigging-and-damage.md` §"Stump cap art is hand-drawn":

5 sprites at 32×32: stump_shoulder_l, stump_shoulder_r, stump_hip_l, stump_hip_r, stump_neck.

Krita pipeline per the doc. ~30 min each = ~2.5 hours of your time.

These don't have to land in P15 if your time is short — they can wait for P16 or even P17. But they're hand-drawn, not AI; document the source as such.

## Done when

- `tools/comfy/` directory is set up with workflow JSON + crop tools + README.
- Placeholder skeleton is in place so the workflow at least loads in ComfyUI.
- `documents/art_log.md` and `assets/credits.txt` are initialized.
- **You** (user) drive ComfyUI and produce `assets/sprites/trooper.png` + the corresponding `g_chassis_sprites[CHASSIS_TROOPER]` table entry.
- `make && ./soldut`: a Trooper renders as a sprite.

## Out of scope

- Other chassis — P16.
- Weapons atlas — P16.
- HUD / pickup atlases — P16.
- Parallax kits — P16.
- Maps content — P17/P18.

## How to verify

```bash
make
./soldut --host
# Spawn a Trooper. See sprite, not capsule.
```

## Close-out

1. Update `CURRENT_STATE.md`: Trooper atlas in; chassis pipeline locked.
2. Update `TRADE_OFFS.md` — no entries to delete (capsule trade-off resolves once all chassis ship in P16).
3. Add a row to `documents/art_log.md` with the approved Trooper's seed + workflow version.
4. Add a row to `assets/credits.txt` for the Trooper atlas.
5. Don't commit unless explicitly asked.

## Common pitfalls

- **ComfyUI workflow JSON format**: it's verbose and node-id-driven. Easier to author by saving from ComfyUI's UI than to hand-write. If hand-written, validate by loading in the UI.
- **The placeholder skeleton is ugly** — don't ship the placeholder atlas. The user MUST replace before approval.
- **Style anchor recursion**: chassis 2-5 use the Trooper anchor. Don't lose it.
- **Crop coordinates depend on the canonical T-pose layout** — if the user generates a layout that doesn't match the crop table, parts will overlap or have wrong content. The skeleton image is the contract: skeleton silhouette + crop table = matching positions.
- **AI-cannot-render-text**: stencils on the chassis are added post-AI by the renderer (P10 has the `chassis_stencils` table). Don't expect stencils to come from the AI output; they're applied at draw time.
