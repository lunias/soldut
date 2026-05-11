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

| trooper.png | `extract_gostek.py tools/comfy/gostek_part_sheets/trooper_gostek_v1.png trooper --palette foundry` (gostek part-sheet path — slices the 22 captioned flat-plate parts into the s_default_parts atlas; per-part rotate/flip + resize + border-flood white-key + Foundry 2-colour snap; no Bayer baked in) | n/a | n/a | n/a | n/a | 2026-05-10 |
