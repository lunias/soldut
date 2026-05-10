# P16 — Remaining chassis + weapon atlas + HUD/pickup icons + parallax kits

> **P15 was revised mid-prompt — read this before starting P16.** The
> chassis-atlas pipeline pivoted from AI-diffusion-canonical (ComfyUI:
> skeleton + style anchor → SDXL + ControlNet + IP-Adapter → 17-tile
> crop) to **gostek part-sheet extraction**: `tools/comfy/extract_gostek.py`
> slices a Soldat-style part sheet (one image, all 22 body parts laid
> out flat-shaded + black-outlined + captioned in reading order)
> straight into `assets/sprites/<chassis>.png`. The diffusion path
> plateaued below the art bar (Illustrious-XL is an anime base;
> IP-Adapter style-transfer can't override the prior; the spec'd
> BattleTech LoRA is SD1.5-incompatible; detailed-illustration crops
> render as same-colour mush at the ~80 px the mech occupies on screen
> — the 17-part rigid renderer wants clean flat plates). See
> `TRADE_OFFS.md` → "Chassis art is hand-authored gostek part sheets,
> not the AI-diffusion pipeline", `tools/comfy/README.md` → "The
> shipping path — gostek part-sheet extraction", and `CURRENT_STATE.md`.
> The AI-diffusion tools stay in `tools/comfy/` as a documented
> alternative — **so the design intent in `11-art-direction.md` (the
> *look*: 2-colour print register, technical-readout flat plates, hard
> outlines, halftone screen at runtime) is unchanged; only the
> *production path* for the chassis atlases changed.** Whether ComfyUI
> stays in the loop to *produce* gostek sheets (vs. hand-draw / vector /
> trace), and what workflow does it best, is open — feel free to change
> the workflows or add a "gostek part-sheet layout" workflow if it
> gets better results.

## What this prompt does

Lands the remaining chassis atlases via the gostek path, then the
weapon atlas (Pipeline 8), HUD icons (Pipeline 6 — AI-as-sketch +
Aseprite hand-redraw), and parallax layers (Pipeline 3). Asset content
for the rendering kit lands.

Depends on P15 (Trooper part sheet extracted; `extract_gostek.py` +
`crop_canonical.py` shared infra in place).

## Required reading

1. P15's outputs: `tools/comfy/extract_gostek.py`, `tools/comfy/gostek_part_sheets/trooper_gostek_v1.png` + `trooper_gostek_manifest.txt`, `assets/sprites/trooper.png`, `documents/art_log.md`, `tools/comfy/README.md`
2. **`documents/m5/11-art-direction.md`** — the *look* canon (still applies; production-path note above)
3. `documents/m5/12-rigging-and-damage.md` §"Per-chassis bone structures" — bone lengths drive the per-chassis sprite proportions; §"Sprite anatomy" — the 22-part layout `extract_gostek.py` packs into
4. `documents/m5/12-rigging-and-damage.md` §"Per-weapon visible art" — for the weapon atlas

## Concrete tasks for Claude

### Task 1 — Per-chassis gostek part sheets

For each of Scout / Heavy / Sniper / Engineer:

- Produce `tools/comfy/gostek_part_sheets/<chassis>_gostek_v1.png` — the 22-part sheet (flat-shaded, black-outlined, captioned in reading order: `TORSO, HIP_PLATE, HEAD, SHOULDER_L/R, UPPER_ARM_L/R, FOREARM_L/R, HAND_L/R, UPPER_LEG_L/R, LOWER_LEG_L/R, FOOT_L/R, 5×STUMP_CAP`). The per-part *proportions* follow the chassis-specific bone lengths in `documents/m5/12-rigging-and-damage.md` §"Per-chassis bone structures":
  - Scout: 11/13/14/14/24/12 (smaller than Trooper)
  - Heavy: 17/18/20/20/38/16 (visibly larger; barrel chest)
  - Sniper: 13/19/17/21/30/16 (long forearms; tall stance)
  - Engineer: 14/14/16/18/32/13 (compact stocky; broader torso)
- Run `python tools/comfy/extract_gostek.py tools/comfy/gostek_part_sheets/<chassis>_gostek_v1.png <chassis> --palette foundry` → `assets/sprites/<chassis>.png`. Eyeball `tools/comfy/output/<chassis>_gostek_atlas_preview.png`; adjust `SLOT_FIX` rotations / `--no-post` / the sheet itself and re-run if a part landed wrong.
- Per-chassis posture quirks are already in `build_pose` (`src/mech.c` — Scout forward lean, Heavy locked rigid, Sniper head-down, Engineer skips right-arm aim when secondary active); the part sheet only needs the per-part shapes, not the pose.

If you want ComfyUI to *generate* the part-sheet layout (rather than hand-drawing), that's fine — but the deterministic slice (`extract_gostek.py`) is what produces the shipped atlas, and what gets logged in `art_log.md` / `credits.txt`. A "gostek part-sheet rack" workflow (one chassis's 22 parts arranged on a captioned grid, à la `weapon_rack_v1.json` in Task 3) would be a reasonable thing to add.

### Task 2 — (diffusion path only — skip unless you go that route) Per-chassis skeletons + anchors

If — and only if — you decide to use the AI-diffusion path for a chassis instead of a gostek sheet:

- Skeleton: `python tools/comfy/skeletons/prepare_skeleton.py <silhouette>.png <chassis> --clean --joints-out` (auto-stamps pivot dots + bone axes from a chassis profile — add a `PROFILE_<CHASSIS>` to `prepare_skeleton.py` for the chassis-specific row fractions), then `python tools/comfy/build_crop_table.py <chassis>` → `<chassis>_crop.txt`.
- Style anchor: reuse `trooper_anchor_v2.png` (the T-pose-with-locked-style is the IP-Adapter reference; the chassis-specific proportions come from the skeleton input). After a chassis generates, save its output as `<chassis>_anchor_v1.png` for any future re-iteration.
- Then `extract_gostek.py` doesn't apply — use `crop_canonical.py` on the approved canonical instead. Expect the same plateau P15 hit.

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
make assets-process    # runs the post-process build step (parallax/decoration; chassis sheets are pre-snapped by extract_gostek.py)
make
./soldut --host
# Spawn each chassis with each weapon. All should render as sprites.
./soldut --shot tests/shots/m5_chassis_distinctness.shot   # all 5 chassis side-by-side
```

## Close-out

1. Update `CURRENT_STATE.md`: all chassis + weapons + HUD assets in.
2. Update `TRADE_OFFS.md`:
   - **Delete** "Mech capsule renderer is the no-asset fallback" (now every chassis has a part sheet + stump caps).
   - Update "Chassis art is hand-authored gostek part sheets, not the AI-diffusion pipeline" — its "alternative path" clause stays as long as the diffusion tooling lives in `tools/comfy/`; revisit/trim per its own triggers.
3. Update `documents/art_log.md` with all approved assets (gostek rows: `extract_gostek.py` + source sheet + palette; record the sheet's own provenance).
4. Update `assets/credits.txt`.
5. Don't commit unless explicitly asked.

## Common pitfalls

- **A gostek part landed rotated/mirrored wrong** (foot upside-down, forearm sideways, head crown-down) — fix that slot's entry in `SLOT_FIX` in `extract_gostek.py` and re-run; check `output/<chassis>_gostek_atlas_preview.png`.
- **`extract_gostek.py` detected ≠ 22 shapes** — a caption merged into a part (tighten the caption↔shape gap in the sheet), two parts touched (add whitespace), or the reading-row band split a visual row (bump `band_frac` in `order_by_reading`).
- **`assets/sprites/<chassis>.png` renders as capsules anyway** — the PNG is 0 bytes / unparseable, or `g_chassis_sprites[chassis].atlas.id == 0` because the load failed. `LOG_LEVEL=info ./soldut --host` and look for the `mech_sprites: loaded …` line.
- **If you go the diffusion route instead** — per-chassis silhouette drift after chassis 3+ (the LoRA's mecha-shape bias accumulates; re-tighten ControlNet strength or train a Soldut-specific LoRA per `11-art-direction.md` §"Risk: when to escalate"); and the same plateau P15 hit (Illustrious-XL anime prior; IP-Adapter can't override it). The gostek path sidesteps both.
- **Weapon rack layout overlap**: weapons must NOT overlap in the input skeleton; the AI bleeds detail across boundaries. Leave 16+ px between weapons.
- **HUD 16×16 icons can't survive AI direct generation** — it's hand-redrawn in Aseprite. Don't try shortcuts.
- **Palette-remap requires a palette PNG** — generate before processing (parallax/decoration; chassis sheets are already snapped by `extract_gostek.py`).
- **Build step on every PNG**: long full builds. Use `make -j` and accept.
- **License**: every Civitai LoRA (diffusion path) and every externally-sourced gostek sheet has its own terms. Check before shipping.
