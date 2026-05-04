# P16 — ComfyUI: remaining chassis + weapon atlas + HUD/pickup icons + parallax kits

## What this prompt does

Replicates the P15 pipeline for the remaining 4 chassis (Scout, Heavy, Sniper, Engineer), then runs Pipeline 8 for the weapon atlas, Pipeline 6 for HUD icons (AI-as-sketch + Aseprite hand-redraw), and Pipeline 3 for parallax layers. Asset content for the rendering kit lands.

Same shape as P15 — Claude prepares infrastructure, you drive ComfyUI.

Depends on P15 (Trooper anchor + crop pipeline locked).

## Required reading

1. P15's outputs: `tools/comfy/workflows/soldut/mech_chassis_canonical_v1.json`, `style_anchor/trooper_anchor_v2.png`, `documents/art_log.md`
2. **`documents/m5/11-art-direction.md`** — the whole doc again
3. `documents/m5/12-rigging-and-damage.md` §"Per-chassis bone structures" — bone lengths drive the per-chassis sprite proportions
4. `documents/m5/12-rigging-and-damage.md` §"Per-weapon visible art" — for the weapon atlas

## Concrete tasks for Claude

### Task 1 — Per-chassis skeletons

For each of Scout / Heavy / Sniper / Engineer:

- Hand-draw `tools/comfy/skeletons/<chassis>_tpose_skeleton.png` per the chassis-specific bone lengths in `documents/m5/12-rigging-and-damage.md` §"Per-chassis bone structures":
  - Scout: 11/13/14/14/24/12 (smaller than Trooper)
  - Heavy: 17/18/20/20/38/16 (visibly larger; barrel chest)
  - Sniper: 13/19/17/21/30/16 (long forearms; tall stance)
  - Engineer: 14/14/16/18/32/13 (compact stocky; broader torso)

- Per-chassis crop table at `tools/comfy/crop_tables/<chassis>_crop.txt` reflecting the chassis-specific bone-length proportions.

- Per-chassis posture quirks documented in the README (forward lean for Scout, locked rigid for Heavy, hunched for Sniper, etc.) — reminders for the AI prompt phrasing.

### Task 2 — Per-chassis style anchors (or shared)

Use the Trooper anchor for chassis 2-5 (the T-pose-with-locked-style is the IP-Adapter reference; the chassis-specific bone proportions come from the skeleton input). After chassis 2 generates, save its output as `<chassis>_anchor_v1.png` for any future re-iteration.

### Task 3 — Weapon atlas pipeline

Write `tools/comfy/workflows/soldut/weapon_rack_v1.json` per `documents/m5/11-art-direction.md` §"Pipeline 8 — Weapon atlas":

- Same model + LoRA stack base
- ControlNet input: `tools/comfy/skeletons/weapon_rack_layout.png` — a 1024×1024 hand-drawn weapon-rack layout with each of 14 weapons in its own pose at its target sprite size, magenta dots at grip + foregrip + muzzle for each
- Style anchor: chassis canonical T-pose (so weapons match chassis surface aesthetic)
- Prompt: "armorer catalogue page, weapon arrangement..." per the doc
- Crop table: per-weapon sub-rect + pivot extraction per `documents/m5/12-rigging-and-damage.md` §"Per-weapon visible sizes"

You drive ComfyUI to generate the rack image. Then `tools/comfy/crop_weapons.sh weapon_rack_approved.png` slices into `assets/sprites/weapons.png` and emits `g_weapon_sprites[]` table for `src/weapon_sprites.c`.

### Task 4 — HUD icon pipeline

Per `documents/m5/11-art-direction.md` §"Pipeline 6 — HUD icons":

For each of 18 HUD icons (8 weapon icons + 5 pickup category + 5 kill-feed flags):

- ComfyUI generates 8 candidates at 256×256 with the icon-specific prompt.
- You pick the best.
- **You hand-redraw at 16×16 in Aseprite** using the AI reference as a backdrop. The redraw is what ships.

Pack into `assets/ui/hud.png` (256×256 atlas).

This is the most-time-per-asset; budget ~2 days for 18 icons.

### Task 5 — Parallax pipeline

Per `documents/m5/11-art-direction.md` §"Pipeline 3 — Parallax layer":

For each map (8 maps × 3 layers = 24, but reused across map pairs reduces to ~16 unique):

- Hand-draw silhouette skeleton at `tools/comfy/skeletons/<map>_<layer>_silhouette.png`.
- ComfyUI workflow `tools/comfy/workflows/soldut/parallax_layer_v1.json` with constructivist LoRA + per-map palette.
- Generate, pick, post-process via ImageMagick (palette-remap to map's 2 colors, halftone, paper-noise).
- Output to `assets/maps/<short>/parallax_far.png`, `parallax_mid.png`, `parallax_near.png`.

### Task 6 — ImageMagick post-process build step

Per `documents/m5/11-art-direction.md` §"Pipeline 7":

Add to top-level `Makefile`:

```makefile
ASSET_PNG := $(shell find assets -name '*.png' -not -path '*/raw/*' -not -path '*/proc/*')
ASSET_PROCESSED := $(ASSET_PNG:.png=.proc.png)

%.proc.png: %.png
    magick $< -ordered-dither h6x6a -remap $(PALETTE) \
        \( assets/raw/paper_noise.png -alpha set -channel A -evaluate set 25% +channel \) \
        -compose multiply -composite \
        -filter point -resize 50% \
        $@

assets-process: $(ASSET_PROCESSED)
```

`tools/build_palettes.sh` generates per-map palette PNGs from `src/palette.h` (the palette table from `documents/m5/11-art-direction.md` §"Two-color print, per map").

### Task 7 — Hand-drawn stump caps (if not done in P15)

5 per chassis × 5 chassis = 25 stump caps. Hand-drawn, no AI. ~10 hours total.

Per `documents/m5/12-rigging-and-damage.md` §"Stump cap art is hand-drawn".

### Task 8 — Decoration sprites

A mix of HAER trace pipeline (per `documents/m5/11-art-direction.md` §"Pipeline 4") and ComfyUI Pipeline 3 with constructivist LoRA at high weight. ~80 decoration sprites total across all maps.

This is bulky time-wise; it can spread into P17 if needed.

## Done when

- 5 chassis atlases at `assets/sprites/<chassis>.png`.
- Weapon atlas at `assets/sprites/weapons.png`.
- HUD atlas at `assets/ui/hud.png`.
- 8 maps × 3 parallax layers (with kit-share) at `assets/maps/<short>/parallax_*.png`.
- 25 stump cap sprites integrated into the chassis atlases.
- ImageMagick build step processes everything; `assets/raw/` holds source PNGs and `.proc.png` is what ships.
- `documents/art_log.md` has rows for every approved asset.
- `assets/credits.txt` lists every asset with source + license.
- `make && ./soldut`: all chassis render as sprites; weapons render as sprites; HUD has icons.

## Out of scope

- Decoration sprites if too many to fit — defer some to P17/P18.
- Maps content — P17/P18.
- Audio assets — that's a separate workflow with Sonniss + Freesound (not ComfyUI).

## How to verify

```bash
make assets-process    # runs the post-process build step
make
./soldut --host
# Spawn each chassis with each weapon. All should render as sprites.
```

## Close-out

1. Update `CURRENT_STATE.md`: all chassis + weapons + HUD assets in.
2. Update `TRADE_OFFS.md`:
   - **Delete** "Mechs rendered as raw capsules" (now everything has art).
3. Update `documents/art_log.md` with all approved assets.
4. Update `assets/credits.txt`.
5. Don't commit unless explicitly asked.

## Common pitfalls

- **Per-chassis silhouette drift after chassis 3+**: the LoRA's mecha-shape bias accumulates; chassis 5 might drift toward "Trooper-like." Re-tighten ControlNet strength or train a Soldut-specific LoRA on the approved chassis 1-3 (per `documents/m5/11-art-direction.md` §"Risk: when to escalate").
- **Weapon rack layout overlap**: weapons must NOT overlap in the input skeleton; the AI bleeds detail across boundaries. Leave 16+ px between weapons.
- **HUD 16×16 icons can't survive AI direct generation** — it's hand-redrawn in Aseprite. Don't try shortcuts.
- **Palette-remap requires a palette PNG** — generate before processing.
- **Build step on every PNG**: long full builds. Use `make -j` and accept.
- **License**: every Civitai LoRA has its own terms. Check before shipping. The doc lists the licenses but verify per file.
