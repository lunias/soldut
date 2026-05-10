#!/usr/bin/env python3
"""
Soldut — single-CLI asset orchestrator.

  python3 tools/comfy/asset.py <chassis> [options]

Drives the per-chassis P15 pipeline end-to-end: skeleton → style anchor
→ canonical (8 candidates) → contact sheet → pick → crop + post-process
→ install to assets/sprites/ → smoke-test in-game.

Each stage is independently re-runnable. Outputs land in
tools/comfy/output/<chassis>/ with stage-tagged filenames so you
always know which asset you're looking at. State is persisted in
tools/comfy/state/<chassis>.json so re-running picks up where it left
off.

------------------------------------------------------------------
Examples
------------------------------------------------------------------

  # Run the whole pipeline for the Trooper. Auto-picks the best
  # canonical candidate (default heuristic). Installs the atlas
  # and runs the in-game shot test.
  python3 tools/comfy/asset.py trooper

  # Re-run just the canonical stage (e.g. you tweaked the prompt).
  python3 tools/comfy/asset.py trooper --regenerate canonical

  # Generate, but don't auto-pick — instead show the contact sheet
  # path and quit. You inspect it, then pick by index.
  python3 tools/comfy/asset.py trooper --review
  python3 tools/comfy/asset.py trooper --pick 3

  # Same chassis, fresh skeleton + anchor + canonical (full reset).
  python3 tools/comfy/asset.py trooper --regenerate all

  # Skip the in-game smoke test (faster iteration loop).
  python3 tools/comfy/asset.py trooper --no-test

  # Override the seed (useful for finding a specific candidate).
  python3 tools/comfy/asset.py trooper --seed 1000042

------------------------------------------------------------------
"""
import argparse, hashlib, json, os, shutil, subprocess, sys, time
from datetime import datetime
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOLS = REPO / "tools" / "comfy"
WORKFLOWS = TOOLS / "workflows" / "soldut"
STATE_DIR = TOOLS / "state"
OUT_BASE = TOOLS / "output"

# Per-chassis defaults (matches the workflow JSON's _meta.chassis_seed).
CHASSIS = {
    "trooper":  {"seed": 1000001, "anchor_seed": 7700001, "stump_seed": 7800001, "palette": "foundry",   "blurb": "balanced baseline"},
    "scout":    {"seed": 2000002, "anchor_seed": 7700002, "stump_seed": 7800002, "palette": "slipstream","blurb": "small + lean"},
    "heavy":    {"seed": 3000003, "anchor_seed": 7700003, "stump_seed": 7800003, "palette": "crossfire", "blurb": "+18% size, broad shoulders"},
    "sniper":   {"seed": 4000004, "anchor_seed": 7700004, "stump_seed": 7800004, "palette": "citadel",   "blurb": "long forearm + shin"},
    "engineer": {"seed": 5000005, "anchor_seed": 7700005, "stump_seed": 7800005, "palette": "reactor",   "blurb": "compact, tool-arm"},
}

STAGES = ["skeleton", "anchor", "canonical", "stumps", "crop", "install", "verify"]


# ----- pretty -----
def banner(stage, chassis, msg=""):
    bar = "─" * 70
    print(f"\n┌{bar}\n│ [{chassis.upper():>9}] {stage.upper():>10}  {msg}\n└{bar}", flush=True)


def info(msg):  print(f"  · {msg}", flush=True)
def warn(msg):  print(f"  ! {msg}", flush=True)
def err(msg):   print(f"  X {msg}", flush=True)


# ----- state -----
def load_state(chassis):
    p = STATE_DIR / f"{chassis}.json"
    if p.exists():
        return json.loads(p.read_text())
    return {"chassis": chassis, "history": []}


def save_state(chassis, state):
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    state["updated"] = datetime.now().isoformat(timespec="seconds")
    (STATE_DIR / f"{chassis}.json").write_text(json.dumps(state, indent=2))


def out_dir(chassis):
    d = OUT_BASE / chassis
    d.mkdir(parents=True, exist_ok=True)
    return d


# ----- comfy wait -----
def wait_for_comfy(host="http://127.0.0.1:8188", timeout=180):
    """Block until ComfyUI's /system_stats responds. Returns True if up."""
    import urllib.request, urllib.error
    deadline = time.time() + timeout
    last_err = None
    waited = False
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(f"{host}/system_stats", timeout=5) as r:
                d = json.loads(r.read())
                ver = d.get("system", {}).get("comfyui_version", "?")
                if waited:
                    print()
                info(f"comfyui up at {host} (v{ver})")
                return True
        except (urllib.error.URLError, ConnectionError, OSError) as e:
            last_err = e
            if not waited:
                sys.stdout.write("  · waiting for ComfyUI ")
                waited = True
            sys.stdout.write(".")
            sys.stdout.flush()
            time.sleep(2)
    if waited:
        print()
    err(f"comfyui not responding at {host} after {timeout}s ({last_err})")
    return False


# ----- stages -----
def stage_skeleton(chassis, state, args):
    """Programmatic skeleton — geometry contract; not AI-generated.
    The skeleton's joint positions ARE the crop table; if the AI
    decided where joints lived, the cropper would slice through hips.
    """
    skel = TOOLS / "skeletons" / f"{chassis}_tpose_skeleton.png"
    if skel.exists() and not args.regenerate_set & {"skeleton", "all"}:
        info(f"skeleton already on disk: {skel}")
        state["skeleton"] = str(skel)
        return True
    # For chassis other than trooper, we don't have a per-chassis Python
    # generator yet. v1 uses the trooper skeleton verbatim (per
    # documents/m5/11-art-direction.md §"Trade-offs to log" — three
    # chassis canonical T-poses, not five).
    if chassis != "trooper":
        warn(f"no per-chassis skeleton for {chassis}; reusing trooper's")
        trooper_skel = TOOLS / "skeletons" / "trooper_tpose_skeleton.png"
        if not trooper_skel.exists():
            err("no trooper skeleton either; run with chassis=trooper first")
            return False
        shutil.copy(trooper_skel, skel)
        state["skeleton"] = str(skel)
        return True
    # Generate the trooper one.
    info("generating skeleton (programmatic, deterministic)")
    rc = subprocess.run([sys.executable, str(TOOLS / "skeletons" / "make_placeholder_skeleton.py")],
                        cwd=str(REPO))
    if rc.returncode != 0:
        err("skeleton generator failed")
        return False
    state["skeleton"] = str(skel)
    return True


def _run_workflow_subprocess(label, args_list):
    """Wrap run_workflow.py invocation with stage-tagged output capture."""
    cmd = [sys.executable, str(TOOLS / "run_workflow.py")] + args_list
    info(f"$ {' '.join(cmd)}")
    return subprocess.run(cmd, cwd=str(REPO))


def stage_anchor(chassis, state, args):
    """AI-generate the IP-Adapter style reference.

    Replacement for the doc's 'pencil sketch' — uses style_anchor_v1.json
    (no ControlNet, no IP-Adapter, lineart LoRA at 0.7). Picks first
    candidate as v1 anchor; user can re-run for variety.
    """
    ch = CHASSIS[chassis]
    anchor = TOOLS / "style_anchor" / f"{chassis}_anchor_v1.png"
    if anchor.exists() and not args.regenerate_set & {"anchor", "all"}:
        info(f"anchor already on disk: {anchor}")
        state["anchor"] = str(anchor)
        return True
    if not wait_for_comfy(args.host):
        return False
    info(f"generating style anchor (seed {ch['anchor_seed']}, batch 4)")
    out_d = out_dir(chassis)
    # The run_workflow's --prefix maps to the "tag in saved filenames"
    rc = _run_workflow_subprocess(
        f"anchor[{chassis}]",
        ["--workflow", str(WORKFLOWS / "style_anchor_v1.json"),
         "--seed",  str(ch["anchor_seed"]),
         "--batch", "4",
         "--prefix", f"{chassis}_anchor",
         "--out-dir", str(out_d),
         "--host", args.host,
         "--timeout", "600"])
    if rc.returncode != 0:
        err("anchor generation failed")
        return False
    # Auto-pick first non-empty result; downsample to 512x512.
    cands = sorted(out_d.glob(f"{chassis}_anchor_*.png"))
    if not cands:
        err("no anchor candidates produced")
        return False
    info(f"got {len(cands)} anchor candidate(s); using {cands[0].name} as v1 anchor")
    # Resize to 512x512 with PIL (IP-Adapter expects ~512x512).
    from PIL import Image
    a = Image.open(cands[0]).convert("RGB")
    if a.size != (512, 512):
        a = a.resize((512, 512), Image.LANCZOS)
    a.save(anchor, format="PNG", optimize=True)
    state["anchor"] = str(anchor)
    state["anchor_source"] = str(cands[0])
    return True


def stage_canonical(chassis, state, args):
    """Run the canonical 8-candidate generation, save contact sheet."""
    ch = CHASSIS[chassis]
    out_d = out_dir(chassis)
    cand_glob_base = f"{chassis}_canonical"
    seed = args.seed if args.seed is not None else ch["seed"]
    state["seed"] = seed
    # Wipe old candidates if --regenerate canonical or all
    if args.regenerate_set & {"canonical", "all"}:
        for p in out_d.glob(f"{cand_glob_base}_*.png"):
            p.unlink()
        sheet = out_d / f"{chassis}_contact_sheet.png"
        if sheet.exists():
            sheet.unlink()
    # Skip if already have candidates and not regenerating
    existing = sorted(out_d.glob(f"{cand_glob_base}_*.png"))
    if existing and not args.regenerate_set & {"canonical", "all"}:
        info(f"{len(existing)} canonical candidate(s) already on disk")
        state["candidates"] = [str(p) for p in existing]
        _build_contact_sheet(chassis, state)
        return True
    if not wait_for_comfy(args.host):
        return False
    info(f"generating canonical 1024x1024 batch={args.batch} seed={seed}")
    info("(this is the slow step — ~3-5 min on RTX 2080 8GB with --lowvram)")
    rc = _run_workflow_subprocess(
        f"canonical[{chassis}]",
        ["--workflow", str(WORKFLOWS / "mech_chassis_canonical_v1.json"),
         "--skeleton", state["skeleton"],
         "--anchor",   state["anchor"],
         "--seed",     str(seed),
         "--batch",    str(args.batch),
         "--prefix",   cand_glob_base,
         "--out-dir",  str(out_d),
         "--host",     args.host,
         "--timeout",  "1500"])
    if rc.returncode != 0:
        err("canonical generation failed")
        return False
    cands = sorted(out_d.glob(f"{cand_glob_base}_*.png"))
    if not cands:
        err("no canonical candidates produced")
        return False
    info(f"{len(cands)} canonical candidate(s) saved")
    state["candidates"] = [str(p) for p in cands]
    _build_contact_sheet(chassis, state)
    return True


def _build_contact_sheet(chassis, state):
    """Write a 4-col contact sheet (256x256 cells) of all candidates,
    labeled with their index. Lets the user pick at a glance."""
    from PIL import Image, ImageDraw, ImageFont
    cands = [Path(p) for p in state.get("candidates", [])]
    if not cands:
        return
    cols, cell = 4, 256
    rows = (len(cands) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cell + 2 * (cols + 1),
                              rows * cell + 2 * (rows + 1) + 24),
                      (24, 22, 20))
    draw = ImageDraw.Draw(sheet)
    draw.text((8, 6), f"{chassis} canonical candidates — pick with --pick N (1..{len(cands)})",
              fill=(220, 220, 220))
    for i, p in enumerate(cands):
        col, row = i % cols, i // cols
        x = 2 + col * (cell + 2)
        y = 24 + 2 + row * (cell + 2)
        try:
            im = Image.open(p).convert("RGB").resize((cell, cell), Image.LANCZOS)
            sheet.paste(im, (x, y))
            draw.rectangle((x, y, x + cell - 1, y + cell - 1), outline=(255, 0, 255))
            draw.rectangle((x, y, x + 36, y + 18), fill=(255, 0, 255))
            draw.text((x + 4, y + 2), f"{i+1}", fill=(0, 0, 0))
        except Exception as e:
            warn(f"contact sheet: skipped {p.name} ({e})")
    sheet_path = OUT_BASE / chassis / f"{chassis}_contact_sheet.png"
    sheet.save(sheet_path, format="PNG", optimize=True)
    info(f"contact sheet → {sheet_path}")
    state["contact_sheet"] = str(sheet_path)


def stage_stumps(chassis, state, args):
    """Optional AI-generated stump caps (5 per chassis).

    The art-direction doc says these should be hand-drawn (AI is bad
    at the subject); the user explicitly asked us to AI-generate them
    anyway. Each is generated at 256x256 and downsampled with point
    filter to 32x32. If the user doesn't want them yet, this stage
    no-ops — the canonical placeholder corner-row pixels are good
    enough for integration testing.
    """
    if args.no_stumps:
        info("--no-stumps set; skipping AI-generated stump caps")
        return True
    ch = CHASSIS[chassis]
    stumps_dir = TOOLS / "stumps" / chassis
    stumps_dir.mkdir(parents=True, exist_ok=True)
    names = ["stump_shoulder_l", "stump_shoulder_r",
             "stump_hip_l", "stump_hip_r", "stump_neck"]
    if not args.regenerate_set & {"stumps", "all"} \
       and all((stumps_dir / f"{n}.png").exists() for n in names):
        info(f"all 5 stump caps already on disk: {stumps_dir}")
        state["stumps"] = {n: str(stumps_dir / f"{n}.png") for n in names}
        return True
    if not wait_for_comfy(args.host):
        return False
    info("generating stump caps (5 × 256x256 → downsample 8x → 32x32)")
    out_d = out_dir(chassis) / "stumps"
    out_d.mkdir(parents=True, exist_ok=True)
    from PIL import Image
    state.setdefault("stumps", {})
    for i, name in enumerate(names):
        seed = ch["stump_seed"] + i
        rc = _run_workflow_subprocess(
            f"stump[{name}]",
            ["--workflow", str(WORKFLOWS / "stump_cap_v1.json"),
             "--seed",     str(seed),
             "--batch",    "1",
             "--prefix",   name,
             "--out-dir",  str(out_d),
             "--host",     args.host,
             "--timeout",  "300"])
        if rc.returncode != 0:
            warn(f"stump '{name}' failed; continuing with placeholder fallback")
            continue
        produced = sorted(out_d.glob(f"{name}_*.png"))
        if not produced:
            continue
        # Downsample 8x with NEAREST to kill bicubic AI smoothness.
        src = Image.open(produced[0]).convert("RGBA")
        small = src.resize((32, 32), Image.NEAREST)
        dst = stumps_dir / f"{name}.png"
        small.save(dst, format="PNG", optimize=True)
        state["stumps"][name] = str(dst)
        info(f"  {name} → {dst}")
    return True


def stage_crop(chassis, state, args):
    """Pick a candidate, then run crop_canonical.py."""
    cands = [Path(p) for p in state.get("candidates", [])]
    if not cands:
        err("no canonical candidates to crop")
        return False
    pick_idx = args.pick - 1 if args.pick else None
    if pick_idx is None:
        if args.review:
            info(f"--review: contact sheet at {state.get('contact_sheet')}")
            info("re-run with --pick N (1..{}) to commit".format(len(cands)))
            return False
        # Auto-pick: heuristic = the candidate whose pixel-stddev
        # diversity is closest to the median (skips near-blank frames).
        pick_idx = _auto_pick(cands)
        info(f"auto-picked candidate {pick_idx + 1}/{len(cands)}: {cands[pick_idx].name}")
        info("    (override with --pick N)")
    if not (0 <= pick_idx < len(cands)):
        err(f"pick {pick_idx+1} out of range (1..{len(cands)})")
        return False
    chosen = cands[pick_idx]
    state["approved_candidate"] = str(chosen)
    state["approved_index"] = pick_idx + 1
    palette = args.palette or CHASSIS[chassis]["palette"]
    info(f"cropping {chosen.name} into atlas (palette={palette})")
    rc = subprocess.run(
        [sys.executable, str(TOOLS / "crop_canonical.py"),
         str(chosen), chassis, "--palette", palette],
        cwd=str(REPO))
    if rc.returncode != 0:
        err("crop_canonical failed")
        return False
    return True


def _auto_pick(cands):
    """Heuristic: skip near-blank frames; prefer the one closest to
    the median pixel-stddev (avoids both the blandest and the noisiest
    output). Cheap; deterministic."""
    from PIL import Image
    import statistics
    scores = []
    for p in cands:
        im = Image.open(p).convert("L")
        # Sample 4096 pixels (8x8 grid each cell averaged) for stddev
        small = im.resize((64, 64), Image.LANCZOS)
        px = list(small.getdata())
        sd = statistics.pstdev(px)
        scores.append(sd)
    median = statistics.median(scores)
    best_i, best_d = 0, float("inf")
    for i, s in enumerate(scores):
        if s < 10:  # near-blank; reject
            continue
        d = abs(s - median)
        if d < best_d:
            best_d, best_i = d, i
    return best_i


def stage_install(chassis, state, args):
    """Verify the atlas exists at assets/sprites/<chassis>.png."""
    atlas = REPO / "assets" / "sprites" / f"{chassis}.png"
    if not atlas.exists():
        err(f"atlas not produced: {atlas}")
        return False
    sz = atlas.stat().st_size / 1024
    info(f"atlas installed: {atlas} ({sz:.0f} KB)")
    state["atlas"] = str(atlas)
    state["installed_at"] = datetime.now().isoformat(timespec="seconds")
    return True


def stage_verify(chassis, state, args):
    """Build the binary, run the chassis distinctness shot test, list outputs."""
    if args.no_test:
        info("--no-test set; skipping in-game smoke test")
        return True
    info("rebuilding ./soldut")
    rc = subprocess.run(["make"], cwd=str(REPO), capture_output=True, text=True)
    if rc.returncode != 0:
        err("make failed:")
        sys.stderr.write(rc.stdout[-2000:] + "\n" + rc.stderr[-2000:])
        return False
    shot = REPO / "tests" / "shots" / "m5_chassis_distinctness.shot"
    if not shot.is_file():
        warn(f"shot test not found: {shot}; skipping")
        return True
    info(f"running ./soldut --shot {shot.relative_to(REPO)}")
    rc = subprocess.run([str(REPO / "soldut"), "--shot", str(shot)],
                        cwd=str(REPO), timeout=120)
    if rc.returncode != 0:
        err(f"shot test exited {rc.returncode}")
        return False
    out = REPO / "build" / "shots" / "m5_chassis_distinctness"
    if not out.is_dir():
        out = REPO / "build" / "shots"
    info(f"shot output → {out}")
    return True


# ----- main -----
def parse_args():
    ap = argparse.ArgumentParser(
        description="Soldut single-CLI asset orchestrator (P15 / P16).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Examples")[1] if "Examples" in __doc__ else "")
    ap.add_argument("chassis", choices=list(CHASSIS), nargs="?",
                    help="Which chassis atlas to produce.")
    ap.add_argument("--regenerate", default="",
                    help=f"Comma-separated stages to force re-run "
                         f"({','.join(STAGES)}; or 'all').")
    ap.add_argument("--pick",   type=int, default=None,
                    help="Pick canonical candidate N (1..batch). Skips auto-pick.")
    ap.add_argument("--review", action="store_true",
                    help="Build contact sheet + quit; user picks via --pick on next run.")
    ap.add_argument("--seed",   type=int, default=None, help="Override KSampler seed.")
    ap.add_argument("--batch",  type=int, default=4,    help="Canonical batch size (default 4 — bump for variety, drop for speed).")
    ap.add_argument("--palette", default=None,          help="Override per-chassis palette.")
    ap.add_argument("--host",   default="http://127.0.0.1:8188", help="ComfyUI URL.")
    ap.add_argument("--no-test", action="store_true",   help="Skip in-game smoke test.")
    ap.add_argument("--no-stumps", action="store_true",
                    help="Skip the (slow, optional) stump-cap generation pass.")
    ap.add_argument("--list",   action="store_true",
                    help="List approved + pending chassis assets and quit.")
    return ap.parse_args()


def cmd_list():
    print(f"{'chassis':<10s}  {'atlas':<30s}  {'cands':>5s}  {'approved':<19s}  notes")
    for c in CHASSIS:
        atlas = REPO / "assets" / "sprites" / f"{c}.png"
        st = STATE_DIR / f"{c}.json"
        cnt = "—"
        approved = "—"
        if st.exists():
            d = json.loads(st.read_text())
            cnt = str(len(d.get("candidates", []))) or "0"
            approved = d.get("installed_at", "—")
        atlas_s = "✓ " + str(atlas.relative_to(REPO)) if atlas.exists() else "(no atlas)"
        print(f"{c:<10s}  {atlas_s:<30s}  {cnt:>5s}  {approved:<19s}  {CHASSIS[c]['blurb']}")


def main():
    args = parse_args()
    if args.list:
        cmd_list()
        return 0
    if not args.chassis:
        err("chassis is required (or use --list)")
        return 2
    args.regenerate_set = set(args.regenerate.split(",")) if args.regenerate else set()
    if args.regenerate_set - set(STAGES + ["all", ""]):
        err(f"unknown stage(s): {args.regenerate_set - set(STAGES + ['all'])}")
        return 1

    chassis = args.chassis
    state = load_state(chassis)
    state["history"].append({
        "ts":  datetime.now().isoformat(timespec="seconds"),
        "cmd": " ".join(sys.argv),
    })

    stages = [
        ("skeleton",  stage_skeleton),
        ("anchor",    stage_anchor),
        ("canonical", stage_canonical),
        ("stumps",    stage_stumps),
        ("crop",      stage_crop),
        ("install",   stage_install),
        ("verify",    stage_verify),
    ]
    for name, fn in stages:
        banner(name, chassis, "")
        try:
            ok = fn(chassis, state, args)
        except KeyboardInterrupt:
            err("interrupted")
            save_state(chassis, state)
            return 130
        save_state(chassis, state)
        if not ok:
            err(f"stage '{name}' did not complete")
            print(f"\n  state preserved at {STATE_DIR / (chassis + '.json')}")
            print(f"  to resume: python3 tools/comfy/asset.py {chassis}")
            return 1
    save_state(chassis, state)
    print(f"\n  ✓ {chassis} pipeline complete")
    print(f"  atlas:    {state.get('atlas','?')}")
    print(f"  picked:   candidate {state.get('approved_index','?')} "
          f"({Path(state.get('approved_candidate','?')).name})")
    print(f"  state:    {STATE_DIR / (chassis + '.json')}")
    print(f"\n  to iterate, e.g.:")
    print(f"    python3 tools/comfy/asset.py {chassis} --regenerate canonical --pick 5")
    print(f"    python3 tools/comfy/asset.py {chassis} --regenerate all")
    print(f"    python3 tools/comfy/asset.py {chassis} --review   # to see contact sheet")
    return 0


if __name__ == "__main__":
    sys.exit(main())
