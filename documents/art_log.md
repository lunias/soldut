# Soldut — Art log

Reproducibility ledger for every approved AI-assisted asset. Every row
locks in: which workflow file produced it, which LoRA stack, which
IP-Adapter anchor, which seed, which prompt version, when it was
approved. Without this, in three weeks when a regenerate comes up — or
when ComfyUI updates a node and the same workflow drifts — you can't
get the same output back.

Per [documents/m5/11-art-direction.md](m5/11-art-direction.md) §"The
mandatory iteration log", this log is **non-optional** for ship.

Pure hand-drawn assets (Pipeline 5 stump caps, Pipeline 6 HUD icons —
the Aseprite-redraw step) are logged in the same table with
`Workflow = hand-drawn` and the toolchain noted in the row.

**Gostek part sheets (the P15-revised chassis path).** Since P15 the
chassis atlases come from `tools/comfy/extract_gostek.py` slicing a
"gostek part sheet" — see `tools/comfy/README.md` → "The shipping path".
A gostek-extracted sprite has no workflow JSON / LoRA stack / IP-Adapter
anchor / seed, so those columns hold `n/a` and the `Workflow` column
notes `extract_gostek.py` + the source sheet path + the `--palette`.
The *sheet's own* provenance (who drew/generated it, from what source,
under what licence) goes in `assets/credits.txt`.

## Conventions

- One row per approved asset (the ones that actually ship in `assets/`).
- Failed iterations are not logged — only the locked output.
- `Workflow` references a checked-in JSON in
  `tools/comfy/workflows/soldut/` (diffusion path) or
  `tools/comfy/extract_gostek.py <sheet> --palette <p>` (gostek path).
  If you rename a workflow, bump its `_v<n>` and add a fresh row.
- `LoRA stack` is the actual chain (`name @ weight`), in load order;
  `n/a` for gostek/hand-drawn.
- `IP-Adapter` references the file under
  `tools/comfy/style_anchor/`; `n/a` for gostek/hand-drawn.
- `Seed` is the literal KSampler seed (e.g. `1000001` for Trooper);
  `n/a` for gostek/hand-drawn.
- `Prompt-version` is the workflow's `_meta.version` field — bump it
  whenever the prompt text changes, even by a word; `n/a` for gostek.
- `Approved` is YYYY-MM-DD; the date the row was committed.

## Approved assets

| Asset | Workflow | LoRA stack | IP-Adapter | Seed | Prompt-version | Approved |
|---|---|---|---|---|---|---|

<!--
Example rows for reference (delete when the first real row lands):

| trooper.png | mech_chassis_canonical_v1.json | battletech 0.6 / SRD 0.4 / lineart 0.5 | trooper_anchor_v2.png | 1000001 | v1 | 2026-MM-DD |
| scout.png   | mech_chassis_canonical_v1.json | battletech 0.5 / SRD 0.5 / lineart 0.6 | scout_anchor_v1.png   | 2000002 | v1 | 2026-MM-DD |
| trooper/stump_shoulder_l.png | hand-drawn (Krita Ink-2 Fineliner) | — | — | — | — | 2026-MM-DD |
-->

| trooper.png | `extract_gostek.py tools/comfy/gostek_part_sheets/trooper_gostek_v1.png trooper --palette foundry --torso-squeeze 0.75` (gostek part-sheet path; P16 added the torso/hip-plate horizontal squeeze to fix the front-facing "looks fat" read by centering a 75%-width tile in the slot with transparent padding) | n/a | n/a | n/a | n/a | 2026-05-10 |
| heavy.png | `extract_gostek.py tools/comfy/gostek_part_sheets/heavy_gostek_forged.png heavy --palette foundry` (gostek part-sheet path; `forged` variant selected over `normal` for strict 5/6/6/5 caption-order layout; no torso squeeze — Heavy is intentionally barrel-chested; 22/22 parts detected) | n/a | n/a | n/a | n/a | 2026-05-10 |
| engineer.png | `extract_gostek.py tools/comfy/gostek_part_sheets/engineer_gostek_normal.png engineer --palette foundry` (gostek part-sheet path; detector found 27 components, 22 valid + 5 stray panel-detail extras filtered after the stumps without displacing any real part) | n/a | n/a | n/a | n/a | 2026-05-10 |
| scout.png | `extract_gostek.py tools/comfy/gostek_part_sheets/scout_gostek_v1.png scout --palette foundry` (gostek part-sheet path; team-regenerated v1 sheet via Perplexity replaced the earlier blade/insect variants that the deterministic extractor couldn't process) | n/a | n/a | n/a | n/a | 2026-05-10 |
| sniper.png | `extract_gostek.py tools/comfy/gostek_part_sheets/sniper_gostek_v1.png sniper --palette foundry` (gostek part-sheet path; team-regenerated v1 sheet replaced the earlier tripod variant whose 3-legged design had 28 components, more than the extractor expected) | n/a | n/a | n/a | n/a | 2026-05-10 |
| weapons.png | `pack_weapons_atlas.py tools/comfy/raw_atlases/weapons_source.png` (14-weapon rack from Perplexity at 2816×1536, repacked to 1024×256 to match `g_weapon_sprites[]` sub-rect layout; checkerboard #C9C9C9+white background detected by tone-band filter and color-keyed to alpha=0) | Perplexity | n/a | n/a | n/a | 2026-05-10 |
| ui/hud.png | Perplexity (single prompt for the 256×256 HUD atlas — HP bar end-caps, jet bar end-caps, weapon-icon family, kill-flag glyphs, crosshair, ammo-box frame; runtime falls back to procedural bars + text-only kill-feed when missing) | Perplexity | n/a | n/a | n/a | 2026-05-10 |
| maps/foundry/parallax_far.png | Perplexity per-map prompt (orange-on-dark Foundry sky + crucible silhouette skyline at 2912×1440, 0.10x scroll layer; kept opaque — deepest layer behind the world) | Perplexity | n/a | n/a | n/a | 2026-05-10 |
| maps/foundry/parallax_mid.png | Perplexity per-map prompt → `keyout_halftone_bg.py` (mid-distance industrial silhouettes at 2912×1440, 0.40x scroll layer; dithered canvas alpha-keyed by local-dark-fraction so the foreground silhouettes stay opaque and the rest becomes transparent — necessary because the source PNG was fully opaque) | Perplexity + scripted | n/a | n/a | n/a | 2026-05-10 |
| maps/foundry/parallax_near.png | Perplexity per-map prompt → `keyout_halftone_bg.py` (close-up foundry machinery foreground at 3072×1536, 0.95x scroll layer drawn AFTER world; alpha-keying is critical here — without it the 95% scroll covers the world inside `renderer_draw_frame`) | Perplexity + scripted | n/a | n/a | n/a | 2026-05-10 |
