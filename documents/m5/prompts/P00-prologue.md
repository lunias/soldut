# P00 — Prologue (orientation; not an implementation prompt)

This is the first prompt to run when starting M5 work. It does not implement features; it confirms Claude has the right context model before P01 onward begins.

Run this once. After it, run P01.

## What to paste into Claude Code

```
We are starting M5 of the Soldut project — the "Maps & content" milestone. M5 is broken down into 14 sub-documents under documents/m5/. The work is sequenced as 18 implementation prompts in documents/m5/prompts/, which I will run one at a time over multiple sessions.

This is the orientation pass. Do NOT write any code yet. Your job in this session:

1. Read these in order:
   - CLAUDE.md (project conventions)
   - documents/README.md (canon docs index)
   - documents/00-vision.md (the pitch + design pillars)
   - documents/01-philosophy.md (rules of the codebase)
   - documents/09-codebase-architecture.md (module layout + dependency graph)
   - CURRENT_STATE.md (what the M4 build actually does)
   - TRADE_OFFS.md (deliberate compromises with revisit triggers)
   - documents/m5/README.md (M5 sub-doc index)
   - documents/m5/00-overview.md (M5 strategic view + dependency graph + risks)
   - documents/m5/13-controls-and-residuals.md (especially the trade-off sweep section)
   - documents/m5/prompts/README.md (how the prompts are sequenced)

2. Skim the file paths under src/ via `ls src/` and read CLAUDE.md's "Architecture in a paragraph" carefully.

3. Report back:
   - What state the build is currently in (M4 — describe what works).
   - The top 3 risks for M5 per the overview doc.
   - Which trade-off entries M5 is supposed to resolve (delete on land) versus which it explicitly keeps.
   - Any inconsistencies, ambiguities, or contradictions between the docs and the code that you noticed during the read pass.

4. Do NOT open any subdoc beyond what's listed above. P01 onward will tell you exactly which subdoc to read for each implementation pass.

5. Do NOT write code. Do NOT run the build. Do NOT modify any file. This is read-only orientation.

When you've reported your findings, the session ends. I'll start P01 in a new session.
```

## What good looks like at the end of this session

Claude has read the listed docs and can explain:

- The M4 build runs end-to-end (title → browser → lobby → match → summary), with capsule mechs, 3 hardcoded code-built maps, no audio, no pickups, no CTF.
- M5 ships 8 maps as `.lvl` files, a level editor, pickup system, CTF, grapple, audio, and AI-assisted art via ComfyUI.
- The dependency graph: format → editor + collision-polygons → pickups + grapple + CTF + map-sharing → maps content. Rendering and audio run in parallel.
- The trade-off sweep in [13-controls-and-residuals.md](../13-controls-and-residuals.md) — 12 entries resolved by other M5 work, 4 picked up at M5, 11 explicitly deferred.

If Claude reports something that contradicts the docs, **fix the docs before running P01**. The docs are the source of truth for everything that follows.

## What NOT to do in this session

- No code changes.
- No build runs.
- No "let me dive into the actual implementation."
- No reading every file under `src/` or every M5 sub-doc — the listed reading is sufficient.
- No new task lists or TODO files.

## After this session

Run P01 in a new Claude Code session.
