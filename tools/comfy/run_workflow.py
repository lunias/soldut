#!/usr/bin/env python3
"""
Soldut — drive a ComfyUI workflow via the API.

POSTs an API-format workflow JSON to /prompt, polls /history for
completion, and pulls the output PNGs into tools/comfy/output/.

The standalone python-shim that lets you run end-to-end without
clicking through the ComfyUI UI:

  # one-shot trooper canonical generation:
  python3 tools/comfy/run_workflow.py \\
      --workflow tools/comfy/workflows/soldut/mech_chassis_canonical_v1.json \\
      --skeleton tools/comfy/skeletons/trooper_tpose_skeleton.png \\
      --anchor   tools/comfy/style_anchor/trooper_anchor_v1.png \\
      --seed 1000001 --batch 8

It uploads the input PNGs into ComfyUI's input/ directory, swaps the
LoadImage node filenames in the workflow to point at the uploads,
optionally overrides the KSampler seed/batch, submits, polls, and
copies all generated images locally with the chassis name in the
filename so the iteration log stays sane.

Requires only Python 3 stdlib (urllib + json + uuid). No requests, no
websocket-client, no extra deps.
"""
import argparse, copy, json, os, shutil, sys, time, uuid
from pathlib import Path
from urllib import request, parse, error

REPO = Path(__file__).resolve().parents[2]


def post_json(url, payload):
    data = json.dumps(payload).encode("utf-8")
    req = request.Request(url, data=data, method="POST",
                          headers={"Content-Type": "application/json"})
    with request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())


def get_json(url):
    with request.urlopen(url, timeout=30) as r:
        return json.loads(r.read())


def get_bytes(url):
    with request.urlopen(url, timeout=60) as r:
        return r.read()


def upload_image(host, path, subfolder="soldut"):
    """POSTs an image to ComfyUI's /upload/image; returns the saved name."""
    boundary = "----soldut-" + uuid.uuid4().hex
    body = []
    def field(name, value):
        body.append(f"--{boundary}".encode())
        body.append(f'Content-Disposition: form-data; name="{name}"'.encode())
        body.append(b"")
        body.append(value if isinstance(value, bytes) else str(value).encode())
    field("subfolder", subfolder)
    field("type",      "input")
    field("overwrite", "true")
    body.append(f"--{boundary}".encode())
    body.append(
        f'Content-Disposition: form-data; name="image"; filename="{path.name}"'.encode())
    body.append(b"Content-Type: image/png")
    body.append(b"")
    body.append(path.read_bytes())
    body.append(f"--{boundary}--".encode())
    body.append(b"")
    data = b"\r\n".join(body)
    req = request.Request(f"{host}/upload/image", data=data, method="POST",
                          headers={"Content-Type":
                                   f"multipart/form-data; boundary={boundary}"})
    with request.urlopen(req, timeout=60) as r:
        out = json.loads(r.read())
    saved = out.get("name") or path.name
    sub   = out.get("subfolder", subfolder)
    return f"{sub}/{saved}" if sub else saved


def find_node_by_meta_title(workflow, title_substr):
    for node_id, node in workflow.items():
        if node_id.startswith("_"):
            continue
        meta = node.get("_meta", {})
        if title_substr.lower() in meta.get("title", "").lower():
            return node_id
    return None


def find_node_by_class(workflow, class_type, key=None):
    for node_id, node in workflow.items():
        if node_id.startswith("_"):
            continue
        if node.get("class_type") == class_type:
            if key and key not in node.get("inputs", {}):
                continue
            return node_id
    return None


def submit(host, workflow, client_id):
    url = f"{host}/prompt"
    return post_json(url, {"prompt": workflow, "client_id": client_id})


def poll_history(host, prompt_id, timeout=600, interval=2.0):
    deadline = time.time() + timeout
    last_status = None
    while time.time() < deadline:
        try:
            hist = get_json(f"{host}/history/{prompt_id}")
        except error.URLError:
            time.sleep(interval); continue
        entry = hist.get(prompt_id)
        if entry:
            status = entry.get("status", {}).get("status_str") \
                  or ("done" if entry.get("outputs") else "running")
            if status != last_status:
                print(f"  status: {status}", file=sys.stderr)
                last_status = status
            if entry.get("outputs"):
                return entry
        else:
            try:
                q = get_json(f"{host}/queue")
                running = len(q.get("queue_running", []))
                pending = len(q.get("queue_pending", []))
                msg = f"  queue: running={running} pending={pending}"
                if msg != last_status:
                    print(msg, file=sys.stderr)
                    last_status = msg
            except error.URLError:
                pass
        time.sleep(interval)
    raise RuntimeError(f"poll_history: timed out after {timeout}s")


def download_outputs(host, history_entry, dest_dir, prefix):
    """Pulls every image listed in history's outputs into dest_dir."""
    dest_dir.mkdir(parents=True, exist_ok=True)
    saved = []
    for node_id, out in history_entry.get("outputs", {}).items():
        for img in out.get("images", []):
            qs = parse.urlencode({
                "filename":  img["filename"],
                "subfolder": img.get("subfolder", ""),
                "type":      img.get("type", "output"),
            })
            url = f"{host}/view?{qs}"
            data = get_bytes(url)
            local = dest_dir / f"{prefix}_{img['filename']}"
            local.write_bytes(data)
            saved.append(local)
            print(f"  saved {local} ({len(data)} bytes)", file=sys.stderr)
    return saved


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host",      default=os.environ.get("COMFYUI_HOST",
                                                          "http://127.0.0.1:8188"))
    ap.add_argument("--workflow",  required=True, type=Path)
    ap.add_argument("--skeleton",  type=Path, default=None,
                    help="Override LoadImage[Skeleton] (typically the T-pose)")
    ap.add_argument("--anchor",    type=Path, default=None,
                    help="Override LoadImage[Style anchor]")
    ap.add_argument("--seed",      type=int, default=None,
                    help="Override KSampler seed")
    ap.add_argument("--batch",     type=int, default=None,
                    help="Override EmptyLatentImage batch_size")
    ap.add_argument("--steps",     type=int, default=None,
                    help="Override KSampler steps")
    ap.add_argument("--cfg",       type=float, default=None,
                    help="Override KSampler cfg")
    ap.add_argument("--prefix",    default="trooper",
                    help="Output filename prefix (default: trooper)")
    ap.add_argument("--out-dir",   type=Path,
                    default=REPO / "tools" / "comfy" / "output",
                    help="Where to save downloaded PNGs")
    ap.add_argument("--timeout",   type=int, default=900,
                    help="Total poll timeout seconds (default 900)")
    ap.add_argument("--dry-run",   action="store_true",
                    help="Resolve everything but skip the /prompt POST")
    args = ap.parse_args()

    if not args.workflow.is_file():
        sys.exit(f"workflow not found: {args.workflow}")

    # Test connectivity early — but skip on dry-run (lets the user inspect
    # the resolved workflow JSON without ComfyUI running).
    if not args.dry_run:
        try:
            stats = get_json(f"{args.host}/system_stats")
        except error.URLError as e:
            sys.exit(f"can't reach ComfyUI at {args.host}: {e}\n"
                     f"start it with: cd ~/ComfyUI && python main.py --listen 127.0.0.1")
        print(f"connected to ComfyUI: "
              f"{stats.get('system', {}).get('comfyui_version', '?')}", file=sys.stderr)

    workflow = json.loads(args.workflow.read_text())

    # Strip the _meta block from the workflow body before posting (the
    # /prompt endpoint accepts it but it's noise on the wire).
    clean = {k: v for k, v in workflow.items() if not k.startswith("_")}

    # Upload + rebind input images. On dry-run we just rewrite the
    # workflow's LoadImage filenames to a notional uploaded path so the
    # printed JSON shows what the live run would post.
    if args.skeleton:
        if not args.skeleton.is_file():
            sys.exit(f"skeleton not found: {args.skeleton}")
        sk = (upload_image(args.host, args.skeleton)
              if not args.dry_run
              else f"soldut/{args.skeleton.name}  (dry-run)")
        node = find_node_by_meta_title(clean, "skeleton") \
            or find_node_by_class(clean, "LoadImage")
        if node:
            clean[node]["inputs"]["image"] = sk
            print(f"skeleton: {args.skeleton} → comfy[{node}].image = {sk}", file=sys.stderr)
        else:
            print(f"warn: no skeleton LoadImage node found", file=sys.stderr)

    if args.anchor:
        if not args.anchor.is_file():
            sys.exit(f"anchor not found: {args.anchor}")
        an = (upload_image(args.host, args.anchor)
              if not args.dry_run
              else f"soldut/{args.anchor.name}  (dry-run)")
        # Anchor node = LoadImage with title containing "anchor", or the
        # OTHER LoadImage node if there are two.
        anchor_node = find_node_by_meta_title(clean, "anchor")
        if not anchor_node:
            loads = [k for k, v in clean.items() if v.get("class_type") == "LoadImage"]
            anchor_node = loads[1] if len(loads) > 1 else None
        if anchor_node:
            clean[anchor_node]["inputs"]["image"] = an
            print(f"anchor:   {args.anchor} → comfy[{anchor_node}].image = {an}", file=sys.stderr)

    # Optional KSampler / latent overrides.
    if args.seed is not None or args.steps is not None or args.cfg is not None:
        ks = find_node_by_class(clean, "KSampler")
        if ks:
            if args.seed is not None:  clean[ks]["inputs"]["seed"]  = args.seed
            if args.steps is not None: clean[ks]["inputs"]["steps"] = args.steps
            if args.cfg is not None:   clean[ks]["inputs"]["cfg"]   = args.cfg
            print(f"ksampler[{ks}]: seed={clean[ks]['inputs']['seed']} "
                  f"steps={clean[ks]['inputs']['steps']} "
                  f"cfg={clean[ks]['inputs']['cfg']}", file=sys.stderr)

    if args.batch is not None:
        em = find_node_by_class(clean, "EmptyLatentImage")
        if em:
            clean[em]["inputs"]["batch_size"] = args.batch
            print(f"latent[{em}]: batch_size={args.batch}", file=sys.stderr)

    if args.dry_run:
        print(json.dumps(clean, indent=2))
        return

    client_id = str(uuid.uuid4())
    submission = submit(args.host, clean, client_id)
    pid = submission.get("prompt_id")
    if not pid:
        sys.exit(f"submit failed: {submission}")
    print(f"prompt_id: {pid}", file=sys.stderr)

    entry = poll_history(args.host, pid, timeout=args.timeout)
    saved = download_outputs(args.host, entry, args.out_dir, args.prefix)
    if not saved:
        sys.exit("no images returned — check ComfyUI logs")
    print(f"\n{len(saved)} image(s) → {args.out_dir}")
    for p in saved:
        print(f"  {p}")


if __name__ == "__main__":
    main()
