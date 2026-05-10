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

## Conventions

- One row per approved asset (the ones that actually ship in `assets/`).
- Failed iterations are not logged — only the locked output.
- `Workflow` references a checked-in JSON in
  `tools/comfy/workflows/soldut/`. If you rename a workflow, bump its
  `_v<n>` and add a fresh row.
- `LoRA stack` is the actual chain (`name @ weight`), in load order.
- `IP-Adapter` references the file under
  `tools/comfy/style_anchor/`. Update the row when a v2 anchor lands.
- `Seed` is the literal KSampler seed (e.g. `1000001` for Trooper).
- `Prompt-version` is the workflow's `_meta.version` field — bump it
  whenever the prompt text changes, even by a word.
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

| trooper.png (rough first pass) | mech_chassis_canonical_8gb_v1.json (T2I-Adapter Lineart SDXL substitute for ControlNet-Union promax; IP-Adapter dropped; CPU VAE — see TRADE_OFFS "ControlNet-Union promax + IP-Adapter chain doesn't fit in 8GB VRAM") | none (LoRA stack from spec doesn't apply — Battletech / SRD on Civitai gated; Lineart substitute is Flux-keyed) | (not consumed by the 8GB workflow; trooper_anchor_v2.png on disk for ≥12GB) | 1000001 | v1 | 2026-05-10 |
