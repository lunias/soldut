# M5 — Art direction & asset sourcing

The aesthetic recipe. Where to source raw material, how to assemble it, what makes Soldut visually recognizable, and what discipline keeps it from feeling AI-slopped. This is the document the engineer-as-artist works from on Monday morning of M5 week 1.

The rendering pipeline that consumes these assets is in [08-rendering.md](08-rendering.md); the per-part anatomy + dismemberment + damage feedback is in [12-rigging-and-damage.md](12-rigging-and-damage.md); the audio analog is in [09-audio.md](09-audio.md). This document is upstream of all three: it picks the *visual identity*, lists the sources to pull from, and specifies the **ComfyUI workflow** that produces the assets.

The design canon already pins the high-level intent ([00-vision.md](../00-vision.md) §"The aesthetic"):

> Futuristic, not cyberpunk. Clean panel lines, exposed pistons, hard edges. Think *Ghost in the Shell* + *Patlabor* + *Battletech* tabletop, less *Cyberpunk 2077* neon overload. Polygons, visibly. Mech bodies are flat-shaded plates with hand-drawn line work. No textures on the mechs themselves — color and seam lines tell the silhouette. Backgrounds are painted, with parallax. Blood is bright. Sound is mechanical.

What this document adds: a *recognizable* execution of that intent — produced **AI-assisted with ComfyUI** but disciplined against the AI-generated feel that makes most 2024–2026 indie games look identical. The core argument: the post-process pipeline, the locked palette, and the hand-drawn skeleton inputs that drive ControlNet do most of the visual work. AI fills in the interior; the *aesthetic* is authored.

## The identity statement

> **Industrial Service Manual.** Soldut looks like a 1985 Battletech technical readout that fell through a portal into a 1925 constructivist machine-shop pamphlet. Mechs are flat-shaded plates with single-weight black linework, two-color flat fills, factory stencils, and visible hydraulic seams; backgrounds are scanned acrylic-paint parallax over a halftone post-process; HUD is VG5000 + Steps Mono on Atkinson body, kill feed shows weapon icons in a 16×16 sprite atlas, blood is the same three colors everywhere it appears.

Memorize it. Every asset that lands in `assets/` answers to this statement.

## Why this and not the easier paths

| Direction | Why we reject it |
|---|---|
| **First-thing-out-of-the-prompt AI generation** | Looks like every other indie shipped 2024–2026; default AI lacks a *consistent* hand and produces telltale artifacts (misaligned panel lines, melted text, generic glow). The whole point of this doc is the *discipline* that makes AI output not look this way. |
| **Generic pixel-art platformer** | Saturated. Every Kenney pack starts here. Indistinguishable. |
| **Synthwave / cyberpunk neon** | The design canon explicitly forbids this. Soldut is *industrial*, not glow-stick. |
| **3D rendered to 2D ("HD-2D")** | Out of budget; produces a smooth, "rendered" surface that fights the polygon-and-line aesthetic. |
| **Cuphead-style hand animation** | Out of budget by 50×. Their pipeline animated 50,000 cells. We have one engineer for 4–6 weeks. |
| **Pure hand-drawn at scale** | Realistic for one chassis, painful for five plus 8 maps' worth of decoration. The earlier draft of this doc proposed this; we revised to AI-assisted because the asset count is too high for the timeline. |

What we adopt: **AI-assisted generation locked behind hand-drawn skeletons (ControlNet-lineart) + a single approved style anchor (IP-Adapter) + a locked LoRA stack + a deterministic post-process pipeline** that crushes everything down into the two-color halftone-print register. The AI provides the *interior linework and surface*; the *silhouette*, *palette*, *post-processing*, and *placement* are authored.

The line we hold: AI never makes a *creative* decision. It fills in geometry we've already specified. The geometry comes from a hand-drawn input image. The style comes from a single approved anchor we accept once and reuse for the whole chassis. The post crushes ambiguity to flat fills + halftone. The engineer makes every decision; ComfyUI is the brush.

## The five hard constraints

Lock these on day 1. Every asset that follows them automatically feels coherent — and crucially, **these are exactly what defeat AI's tells**. A pure "I prompted SDXL" output drifts in palette, gradients its shadows, generates plausible-but-wrong panel lines, and dissolves into the indistinguishable 2024+ AI-game look. The five constraints below break each of those failures:

| AI tell | Constraint that defeats it |
|---|---|
| Drifts palette across a set | (1) Two-color print, locked palette |
| Smooth-everywhere rendering | (3) Halftone-only fills, run as a screen post |
| Generic vector-clean lines | (2) Hand-drawn linework at 4× resolution |
| Plausible-but-wrong panel geometry | ControlNet-lineart from your hand-drawn skeleton |
| Inconsistent style across parts | IP-Adapter from a single approved anchor |
| Missing/melted text | (4) Stencils rendered post-AI from a real font |
| Per-victim color variance | (5) Three-color blood/oil palette baked into the FX header |

Constraints 2–5 below are aesthetic; the ControlNet + IP-Adapter discipline is in §"The ComfyUI workflow" — the same constraints all the way down.

### 1. Two-color print, per map

Each of the 8 maps gets a 2-color palette: one **dark** (used for all linework and mech-shadow) plus one **accent** (the map's "signature" color). Backgrounds, decorations, and tile fills come from the accent + grayscale; never from a third hue. The result: Foundry reads as orange, Reactor as green, Citadel as muted blue. A player can identify the map from a thumbnail at 64×64.

Map-color assignments (starting; tune after first paints):

| Map | Dark | Accent | Reads as |
|---|---|---|---|
| Foundry | `#1A1612` (warm black) | `#D8731A` (orange) | "smelter floor" |
| Slipstream | `#0F1416` (cold black) | `#3FB6C2` (cyan) | "service tunnel" |
| Concourse | `#19131A` (purple-black) | `#E0B838` (sodium yellow) | "atrium under sodium light" |
| Reactor | `#0E1812` (forest-black) | `#4FA67F` (mint-green) | "active core" |
| Catwalk | `#15121E` (night-black) | `#C2436F` (rose) | "exterior dusk" |
| Aurora | `#1A1A20` (gunmetal) | `#9FA8DC` (overcast lavender) | "open sky" |
| Crossfire | `#1F1A14` (dark amber) | `#B57A39` (umber) | "evening foundry" |
| Citadel | `#0E1419` (deep blue) | `#7BA0C2` (slate cyan) | "fortress dawn" |

Codified in `src/palette.h`:

```c
typedef struct { uint8_t r, g, b; } RGB8;
typedef struct {
    const char *name;
    RGB8 dark;
    RGB8 accent;
} MapPalette;
extern const MapPalette g_map_palettes[MAP_COUNT];
```

Each map's MapDef references its palette; the renderer's tile-atlas tinting and parallax color-grading run through this struct.

**Why this is high-impact**: most indie 2D games drift into the full RGB cube. Locking 2 colors per map is what makes early-90s SNES screenshots still recognizable today. We're stealing that signal.

### 2. All linework is generated at 4× resolution and downsampled with hard nearest-point

Mech parts, decoration sprites, HUD glyphs — every line passes through ComfyUI at 4× the final pixel size, then is downsampled with **point/nearest-neighbor** resampling (not bicubic). Bicubic re-introduces the AI-soft-edge feel; nearest-point preserves the per-pixel hardness that reads as authored.

The 4× authoring is also the size at which the **hand-drawn skeleton** that drives ControlNet-lineart lives. You draw the part outline at 1024×1024 working resolution; the AI fills its interior; the post-process downsamples to 256×256 final. The downsample-jitter on a 1px line is the "this was made by a person" signal that pure AI output lacks (because pure AI output is generated and rendered at the same resolution — no jitter, no grain).

**Never bicubic-downsample a mech sprite.** This is the single most common mistake in AI-art pipelines: the bicubic resampler smooths everything and the result feels machine-rendered. The ImageMagick `-filter point` in the post-process pipeline is load-bearing.

Vector-traced decoration is fine for *background* sprites (the Inkscape pipeline traces public-domain HAER blueprints — see "Pipeline 4" below). Those read as "engineering drawings," and engineering drawings are supposed to be sharp.

### 3. Halftone fill, never gradient

Soldut uses no gradients. Anywhere shadow falls — on a mech plate, in a parallax silhouette, on the HUD bar — it's expressed as a halftone screen. Krita's [Screentone fill layer](https://docs.krita.org/en/reference_manual/layers_and_masks/fill_layer_generators/screentone.html) handles this for assets; a fragment-shader halftone post-process handles the rest.

**The halftone post shader.** [06-rendering-audio.md](../06-rendering-audio.md) §"Shaders" already specifies a `post_fx` fragment shader. M5 expands it to do a Bayer-matrix dither plus a halftone screen at ~30% screen density. The shader runs on the full framebuffer after `EndMode2D`, before HUD. It's ~40 lines of GLSL — see "The halftone post shader" below for the implementation.

The shader doing the work means individual assets don't have to be perfect. Rough fills, slightly inconsistent colors, jagged edges — all of it gets unified by the screen pass. **This is the "Obra Dinn lesson"** ([Surma's Ditherpunk](https://surma.dev/things/ditherpunk/) is the canonical reference): shipping a strong post-process means the asset budget per pixel drops.

### 4. Diegetic factory stencils — rendered by the engine, not the AI

Every mech part has a small numerical or alphanumeric label like `K-7`, `42-A`, `M-12-RE`. **AI cannot render text.** It produces gibberish that reads as immediately AI on inspection. So we don't ask it to.

Instead: AI generates the part *without text*. The engine draws the stencil **post-render**, on top of the part sprite, in a real TTF font (**VG5000** — Velvetyne, OFL). Each chassis has a small `chassis_stencils.h` table that says "draw stencil `K-7` at sprite-local coords (12, 24) on the L-upper-arm part." 2-3 stencils per chassis × 5 chassis = ~12 stencils total. ~20 minutes one-time work.

The stencil colors come from the map palette's dark color. The font is real, the placement is authored, the result reads as a real factory marking. Patlabor + Battletech reference is preserved.

```c
// src/mech_stencils.h — small table per chassis
typedef struct {
    MechSpriteId part;
    int          x, y;           // sprite-local
    int          font_pt;        // typically 6–10
    const char  *text;
} MechStencil;

extern const MechStencil g_trooper_stencils[];   // [] = {{MSP_TORSO, 24, 30, 8, "K-7"}, …};
extern const MechStencil g_scout_stencils[];
// ... per chassis
```

The renderer's `draw_mech` walks the stencil table after drawing each sprite and overlays the text via `DrawTextEx`. ~30 LOC.

This is also a **layered defense against AI tells**: even if a chassis sprite slips through with vague AI smoothness, the visible factory text re-establishes that this was authored.

### 5. Three-color blood/oil/spark palette

Particles, decals, gibs — same three values everywhere they appear:

```c
// src/fx_palette.h — header constants
#define BLOOD_RED      0xD8232AFF  // bright red core
#define BLOOD_ORANGE   0xF2A516FF  // hydraulic orange
#define BLOOD_BLACK    0x0A0908FF  // oil-black for decals
```

No tinting. No per-victim color variation. A mech leaks the same colors regardless of chassis or armor. This was the design-canon promise ("Blood is bright"); the constraint is what makes it consistent.

Trade-off: **doesn't differentiate which mech bled** in a chaotic fight. Acceptable — the kill feed already attributes kills.

## Reference aesthetics — three sources to literally study

Per the research, three references the engineer studies (not traces, *studies*):

### A. Battletech 3025/3026/3039 Technical Readouts (FASA, 1986–1989)

What we steal: **single-weight contour line + selective panel hatching + visible plate seams + factory call-outs**. Drawn as "draftsman's elevations," not paintings. No rendered shading.

Where to look (study only, copyright is FASA's):
- [Sarna.net — Category: Technical Readouts and Recognition Guides](https://www.sarna.net/wiki/Category:Technical_Readouts_and_Recognition_Guides)
- [Art of Battletech (Tumblr)](https://artofbattletech.tumblr.com)

The signature artists to look up by name: **Duane Loose**, **Steve Venters**, **Doug Chaffee**, **Mike Jackson**. Their pen work in the 3025/3026 generation is the technique we're emulating — *not* the 3050+ digital-painting era.

### B. Patlabor + Ghost in the Shell mecha design sheets

What we steal: **how to make industrial vehicles look like characters**. Visible hydraulics and pistons, exposed mounts, factory stencils, side-by-side production-spec diagrams.

Where to look:
- [Mechabay — Yutaka Izubuchi](https://mechabay.com/izubuchi-yutaka)
- [Schaft Enterprises — Patlabor artbook list](http://www.patlabor.info/artbooklist2.htm)

Specifically: Patlabor production design sheets show construction-equipment-as-character. The Ingram and Pyracid models from the films are the right register — armored but *visibly machined*.

### C. Mid-century technical illustration + Soviet constructivism

What we steal: **flat-color, oblique-projection, labeling-as-art**. This is the unique-aesthetic axis that nobody copies. Predates and is orthogonal to "synthwave indie."

Where to look (much of it public-domain; can ship in derivative form):
- [Wikimedia Commons — Constructivism](https://commons.wikimedia.org/wiki/Category:Constructivism) — Rodchenko, Lissitzky, Klutsis posters
- [Wikimedia Commons — Soviet propaganda posters](https://commons.wikimedia.org/wiki/Category:Propaganda_posters_of_the_Soviet_Union)
- [Library of Congress HABS/HAER](https://www.loc.gov/collections/historic-american-buildings-landscapes-and-engineering-records/) — measured drawings of historical American factories, steel mills, generators
- [Internet Archive — Pulp Magazine Archive](https://archive.org/details/pulpmagazinearchive) — Frank R. Paul's *Amazing Stories* covers (1926–1929 are unambiguously PD)

The HAER collection is *the* trace source for our parallax decorations. Steel-mill cross-sections, factory boiler diagrams, valve assemblies — all measured, all PD, all directly traceable.

## Asset source registry

Every URL we'll pull from, by category. License column is the *minimum* license in the set; verify per file.

### Visual reference (study, do not redistribute)

| Source | URL | Use |
|---|---|---|
| Battletech TROs index | https://www.sarna.net/wiki/Category:Technical_Readouts_and_Recognition_Guides | Mech pose + linework register |
| Art of Battletech | https://artofbattletech.tumblr.com | Curated TRO plates |
| Mechabay (Izubuchi) | https://mechabay.com/izubuchi-yutaka | Patlabor + GitS designs |
| Schaft Enterprises | http://www.patlabor.info/artbooklist2.htm | Patlabor artbook bibliography |
| The Korshak Collection (Frank R. Paul) | https://www.korshakcollection.com/frank-r-paul | High-res Paul cover scans |

### Public-domain / CC0 raw material (ship in derivative form)

| Source | URL | License | Best for |
|---|---|---|---|
| Library of Congress HABS/HAER | https://www.loc.gov/collections/historic-american-buildings-landscapes-and-engineering-records/ | PD (US Gov) | Industrial machinery measured drawings — trace fuel for decorations |
| Wikimedia Commons — Constructivism | https://commons.wikimedia.org/wiki/Category:Constructivism | PD (mostly pre-1929) | HUD chrome backplates, map title cards |
| Wikimedia Commons — Soviet posters | https://commons.wikimedia.org/wiki/Category:Propaganda_posters_of_the_Soviet_Union | PD | Same |
| NASA Image and Video Library | https://images.nasa.gov/ | PD (US Gov) | Spacecraft schematics, blueprint scans |
| Smithsonian Open Access | https://www.si.edu/openaccess | CC0 | 5.1M items including patent models |
| Smithsonian 3D CC0 | https://3d.si.edu/cc0 | CC0 | 3D scans of historical machinery (silhouette source) |
| Internet Archive Pulps | https://archive.org/details/pulpmagazinearchive | PD pre-1929 | Frank R. Paul SF covers — parallax sky elements |
| Stanford Pulp Magazines Project | https://www.pulpmags.org/ | Verifies PD status | Scholarly verification before pulling |
| PICRYL | https://picryl.com/ | PD aggregator | Search-engine across LoC/NARA/Smithsonian |
| PICRYL — Patent drawings | https://picryl.com/topics/patent+drawings | PD | Pre-1929 patent illustrations |
| Stanford Copyright Renewal Database | https://exhibits.stanford.edu/copyrightrenewals | Verify renewal status | Check 1929–1963 works are non-renewed |
| Polyhaven — textures | https://polyhaven.com/textures | CC0 | Concrete, rust, metal — for parallax base painting only |

### Game-asset packs (selective; mostly for HUD, decals, particles)

| Source | URL | License | Use |
|---|---|---|---|
| Kenney — sci-fi sounds | https://kenney.nl/assets/sci-fi-sounds | CC0 | Plasma/energy weapon SFX |
| Kenney — impact sounds | https://kenney.nl/assets/impact-sounds | CC0 | Explosions, hit SFX |
| Kenney — interface sounds | https://kenney.nl/assets/interface-sounds | CC0 | UI clicks/hovers |
| Kenney — assets browser | https://kenney.nl/assets | CC0 | Avoid the visual sci-fi packs (too cartoony for Soldut) |
| OpenGameArt — CC0 filter | https://opengameart.org/art-search-advanced?field_art_licenses_tid%5B%5D=4 | CC0 | Particles, projectile sprites, decals |
| Sonniss GameAudioGDC | https://sonniss.com/gameaudiogdc/ | Royalty-free, no attribution | Slug-gun, explosion, footstep, debris audio |

**Honest reality check:** the mech sprites themselves are hand-drawn, not pulled from a pack. No CC0 indie pack matches the Battletech-TRO/Patlabor register at the right resolution. The packs above source decals, particles, audio — *not* mechs.

### Audio sources

| Source | URL | License | Best for |
|---|---|---|---|
| Sonniss GameAudioGDC | https://gdc.sonniss.com/ | Royalty-free | Bulk of slug-gun, explosion, footstep, debris |
| Kenney audio packs | https://kenney.nl/assets/category:Audio | CC0 | Plasma weapons, impacts, UI |
| Freesound — CC0 filter | https://freesound.org/search/?f=license%3A%22Creative+Commons+0%22 | CC0 | Hydraulics (RutgerMuller, parabolix), servo motors (Artninja, tlwmdbt) |
| NASA audio | https://www.nasa.gov/audio-and-ringtones/ | PD | Industrial sci-fi room tone |
| Incompetech (Kevin MacLeod) | https://incompetech.com/music/royalty-free/music.html | CC-BY 4.0 | Music — industrial / electronica |
| Nihilore | http://www.nihilore.com/synthwave | CC-BY 4.0 | Music — synthwave / industrial / cinematic |
| FreePD | https://freepd.com/ | PD (CC0-equivalent) | Music — small public-domain catalog |

**Avoid:** [BBC Sound Effects](https://sound-effects.bbcrewind.co.uk/) — RemArc license is **non-commercial only** (per their [licensing page](https://sound-effects.bbcrewind.co.uk/licensing)). Skip unless you license commercially.

### Fonts (all OFL or PD; ship in `assets/fonts/`)

| Font | URL | License | Role |
|---|---|---|---|
| Atkinson Hyperlegible | https://www.brailleinstitute.org/freefont/ | OFL | Body — already vendored at M5 (per [08-rendering.md](08-rendering.md)) |
| VG5000 | https://velvetyne.fr/fonts/vg5000/ | OFL | Display — map titles, kill feed flag chips, factory stencils on mech plates |
| Steps Mono | https://velvetyne.fr/fonts/steps-mono/ | OFL | HUD numerics — ammo, timer, score |
| Departure Mono | https://departuremono.com/ | OFL | Backup HUD-numeric (alt to Steps Mono if Steps reads too wide) |
| Public Sans | https://github.com/uswds/public-sans | OFL | Backup body (alt to Atkinson if accessibility tone feels wrong) |
| Velvetyne foundry | https://velvetyne.fr/ | OFL | Browse for one-offs; their whole catalog is OFL |

Three fonts ship in `assets/fonts/`: Atkinson Hyperlegible (body), VG5000 (display), Steps Mono (numeric). ~600 KB combined.

### Tools

| Tool | URL | License | Role |
|---|---|---|---|
| Wacom Intuos S | $80 USD hardware | n/a | Tablet for hand-drawing |
| Krita | https://krita.org/ | GPL/free | Mech sprites, parallax painting, halftone filter |
| Inkscape | https://inkscape.org/ | GPL/free | Trace-bitmap pipeline for HAER blueprints |
| Aseprite | https://aseprite.org/ | $15 commercial | HUD atlas, pickup atlas, particle atlas (pixel-perfect) |
| ImageMagick | https://imagemagick.org/ | Apache-2 | Build-step halftone + jitter passes |
| rTexPacker | https://raylibtech.itch.io/rtexpacker | Free | Atlas packing |
| GIMP | https://www.gimp.org/ | GPL/free | Backup raster editor (curve correction on scans) |

## The ComfyUI workflow

ComfyUI is the load-bearing tool for M5 art. We assume it's installed locally with the standard custom-node pack (manager, controlnet_aux, IPAdapter_plus, rgthree, was-node-suite, comfyroll, fill-nodes, pixelart-detector, custom-scripts). Workflows live in `tools/comfy/workflows/soldut/` as `.json` files committed to the repo.

The recipe is **canonical-T-pose generation, then crop per part** — *not* per-part generation in isolation. This is the single most important workflow choice. Generating each of 12 parts as separate prompts produces panel-line discontinuities at every joint that you'll fight for days. Generating one full mech and cropping it produces 12 parts that look like they belong to the same machine, because the diffusion model thought of them that way.

### Models (download once, store under `~/.local/share/ComfyUI/models/`)

| File | Where | License | Role |
|---|---|---|---|
| `illustriousXL_v20.safetensors` (6.4 GB) | https://civitai.com/models/795765/illustrious-xl | OpenRAIL-M (commercial OK) | Base checkpoint — handles the technical-illustration register |
| `battletech_battlemechs.safetensors` | https://civitai.com/models/136009/battletech-battlemechs | check model card | Mecha-shape LoRA (weight 0.5–0.7) — *the* reference for our register |
| `super_robot_diffusion_xl.safetensors` | https://civitai.com/models/124747 | check model card | Generic mecha LoRA (weight 0.3–0.5) — variety supplement |
| `lineart_sdxl_v3.safetensors` | https://civitai.com/models/539031 | OFL-equivalent | Lineart-style LoRA (weight 0.5–1.0) — kicks output toward black-line-on-flat |
| `soviet_constructivist_poster.safetensors` | https://civitai.com/models/695475 | check model card | Constructivist style LoRA — for parallax layers (weight 0.7) |
| `controlnet-union-sdxl-1.0` (2.5 GB) | https://huggingface.co/xinsir/controlnet-union-sdxl-1.0 | OpenRAIL | One ControlNet that does canny / lineart / scribble / depth. Saves disk and VRAM. |
| `ip-adapter-plus_sdxl_vit-h.safetensors` | https://huggingface.co/h94/IP-Adapter | MIT | Style-anchor reference conditioning |
| `model.safetensors` (CLIPVision encoder) | https://huggingface.co/h94/IP-Adapter (image_encoder) | MIT | Required by IP-Adapter Plus |
| `sdxl-vae-fp16-fix.safetensors` | https://huggingface.co/madebyollin/sdxl-vae-fp16-fix | MIT | VAE that doesn't saturate-blow at fp16 |

For the **parallax** workflow only, `flux2-klein-fp8` (~7 GB) at https://huggingface.co/black-forest-labs/FLUX.2-klein produces wider compositions cleanly. Skip if VRAM is tight; Illustrious-XL handles parallax fine at 1024×512.

### Workflow file layout

```
tools/comfy/workflows/soldut/
├── mech_chassis_canonical.json   # Pipeline 1 — full T-pose mech
├── damage_variant.json            # Pipeline 2 — img2img to damaged variant
├── parallax_layer.json            # Pipeline 3 — wide background layer
├── decoration_trace.json          # Pipeline 4 — input is a HAER blueprint
├── hud_icon_sketch.json           # Pipeline 5 — AI as sketch reference for hand-drawn icon
└── style_anchor/
    ├── trooper_anchor.png         # The first approved image for each chassis
    ├── scout_anchor.png
    └── ...
```

A workflow's filename is `_v<n>` whenever the prompt or LoRA stack changes. Older versions stay because v3 might regress against v2.

### Pipeline 1 — Mech chassis canonical T-pose (the load-bearing one)

Goal: one 1024×1024 image of a full mech in T-pose with the locked style. The 12 part sprites are **cropped from this** image deterministically.

```
[CheckpointLoaderSimple] illustriousXL_v20.safetensors
   │
   ├──► [LoraLoader] battletech_battlemechs.safetensors  weight 0.6
   │      └──► [LoraLoader] super_robot_diffusion_xl.safetensors  weight 0.4
   │             └──► [LoraLoader] lineart_sdxl_v3.safetensors  weight 0.5
   │                    └──► MODEL_OUT
   │
   └──► CLIP_OUT, VAE_OUT (use sdxl-vae-fp16-fix)

[LoadImage] tools/comfy/skeletons/trooper_tpose.png
                    (1024×1024 hand-drawn skeleton: silhouette of the mech in T-pose,
                     pure black on white, with magenta dots at every pivot point)
   │
   └──► [LineartStandardPreprocessor]
          └──► [ControlNetApplyAdvanced]
                 controlnet: controlnet-union-sdxl-1.0
                 strength: 0.92   start: 0.0   end: 0.95

[LoadImage] tools/comfy/workflows/soldut/style_anchor/trooper_anchor.png
                    (the SINGLE approved style reference — for chassis 1 this might
                     be a pencil sketch you drew; for chassis 2+ it's the canonical
                     T-pose output from chassis 1, or your chosen part of it)
   │
   └──► [IPAdapterUnifiedLoader]  preset: PLUS
          └──► [IPAdapterAdvanced]  weight 0.75   start 0.0   end 0.85

[CLIPTextEncode positive]:
  "masterpiece, technical illustration, mechanical_parts, no_humans,
   industrial_design, line_art, flat_color, monochrome_with_red_accent,
   battlemech, full_body, t_pose, hydraulic_pistons, panel_lines,
   exposed_machinery, factory_built, hand_drawn_ink, halftone_screentone,
   white_background, no_text, no_watermark"

[CLIPTextEncode negative]:
  "photo, photorealistic, 3d render, smooth shading, gradient, soft lighting,
   ambient_occlusion, painterly, blurry, jpeg_artifacts, text, watermark,
   signature, anime_face, character, multiple objects, busy background,
   antialiased_lines, glow, bloom, depth_of_field, character_eyes, human_face"

[KSampler]
  steps: 30   cfg: 7.5   sampler: dpmpp_2m_sde   scheduler: karras
  seed: <CHASSIS_SEED>   denoise: 1.0
  (CHASSIS_SEED is locked per-chassis: TROOPER=1000001, SCOUT=2000002, …
   batch_size: 8 to produce 8 candidates per generation)

[VAEDecode]
   │
   └──► [SaveImage] candidates_<chassis>_<seed>_*.png
```

Generate 8 candidates (different sub-seeds within the chassis seed). Pick the best one as `style_anchor/<chassis>_anchor.png` (overwrites the input from previous iteration if better). For chassis 1 (Trooper) this iteration takes the longest — usually 20–40 candidates over a day before a usable anchor lands. For chassis 2+, the IP-Adapter + the previous anchor + locked seed converge in ~8 candidates per chassis.

After picking, **deterministic crop** to the 12 part sub-rects. The hand-drawn skeleton's coordinates dictate which pixels go to which sprite. The cropping is done by an `imagemagick` step (or a tiny ComfyUI workflow `crop_canonical.json`) that takes the canonical PNG + a per-chassis crop table and outputs `assets/sprites/<chassis>.png` (the atlas) plus the `g_chassis_sprites[chassis]` table entry.

The hand-drawn skeleton is the most important input you produce. It's authored once per chassis (5 skeletons, ~2 hours each = 10 hours total), and it specifies:
- The silhouette of every part
- Where each pivot lives (magenta dots in the source image; the cropper extracts pixel coords for the `MechSpritePart.pivot` field)
- Which pixel ranges are "overlap zones" (drawn in light grey in the source, used as mask for the per-part cropping; see [12-rigging-and-damage.md](12-rigging-and-damage.md) §"Overlap zones")
- Where the "exposed end" should be ragged (drawn with a wavy edge in the source, AI honors the silhouette)

### Pipeline 2 — Damage variants (img2img)

Goal: produce a "damaged" or "heavily damaged" variant of an existing part sprite without changing the silhouette. **At v1 we don't ship damage variants — see [12-rigging-and-damage.md](12-rigging-and-damage.md) §"Layer 2 — Persistent damage decals on the part" for the runtime decal system that obviates this.** This pipeline is documented as the M6 polish path.

```
[LoadImage] assets/sprites/trooper.png
   │
   └──► [VAEEncode] (latent)
          │
          └──► [KSampler] (img2img mode; denoise 0.30; same seed as canonical)
                 with prompt-add: "battle damaged, scorched plates, dented armor,
                                   exposed wiring, hydraulic leaks, panel scratches"
                 ControlNet-canny on the source so silhouette is preserved
                    │
                    └──► [VAEDecode] → [Image Composite Masked]  silhouette mask
                           │
                           └──► [SaveImage] trooper_damaged.png
```

Mask the silhouette (using the same alpha channel from the canonical) so the silhouette stays identical between damage states; only the surface changes. This is critical for the bone-attachment math — pivot pixels must not move.

### Pipeline 3 — Parallax layer

Goal: a 1024×512 parallax layer (mid or near distance) for a map.

```
[CheckpointLoaderSimple] flux2-klein-fp8.safetensors    (or illustriousXL if VRAM-limited)
   │
   └──► [LoraLoader] soviet_constructivist_poster.safetensors  weight 0.7

[LoadImage] tools/comfy/skeletons/<map>_silhouette.png
                    (hand-drawn: rough outline of factory rooftops, pipes,
                     reactor towers, etc. — silhouette only, no detail)
   │
   └──► [ScribblePreprocessor]
          └──► [ControlNetApplyAdvanced]  strength 0.85

[CLIPTextEncode +]:
  "industrial silhouette, factory skyline, smokestacks, refinery,
   constructivist propaganda poster, two-color print, silkscreen,
   hard edges, flat fill, high contrast, <map_dark> and <map_accent>,
   no atmospheric perspective, no people, no vehicles in foreground,
   no text"

[CLIPTextEncode -]:
  "photograph, gradient, soft, glow, fog, lens flare, anime, character,
   3d, depth blur, smooth, painterly, color noise"

[KSampler]  steps 40   cfg 6.5   dpmpp_3m_sde   karras   denoise 1.0
   │
   └──► [VAEDecode]
          │
          └──► [UltimateSDUpscale] to 2048×1024, denoise 0.25
                 │
                 └──► [PixelArtLoadPalettes] palette: <map>_2color.png
                        (the 2-color hard-quantize step — non-negotiable)
                        │
                        └──► [SaveImage] parallax_<map>_<distance>.png
```

The `<map>_2color.png` is a 2-pixel image holding the map's dark + accent colors (per the per-map palette table in §"Two-color print, per map"). `PixelArtLoadPalettes` from `ComfyUI-PixelArt-Detector` does the hard remap.

3 layers per map × 8 maps = 24 parallax PNGs. ~1 day per map; share kits across map pairs to halve. Realistic budget: **5–6 days for all parallax**.

### Pipeline 4 — HAER decoration trace (the AI-free pipeline that survives)

Goal: a parallax decoration like "vintage steel-mill boiler" at 256×128. **No AI in this pipeline** — AI is bad at clean technical-drawing register, the trace pipeline excels, and shipping some assets that are *certifiably* not AI-generated diversifies the look (and the legal posture).

1. **Source**: [LoC HABS/HAER](https://www.loc.gov/collections/historic-american-buildings-landscapes-and-engineering-records/), search "Bethlehem Steel" or similar, download the TIFF at "Higher Resolution."
2. **GIMP**: Curves → push pure black to RGB(0,0,0) and pure white to RGB(255,255,255). Export as PNG.
3. **Inkscape**: File → Import. Path → Trace Bitmap → Single Scan, "Brightness Cutoff." Path → Simplify (Ctrl-L). Fill = map dark color. Stroke = none.
4. **Krita**: Import SVG, rasterize at 256×128, add halftone fill on shaded regions.
5. **Export**: PNG into `assets/sprites/decorations/`.

10 decoration sprites/day. 80 sprites across all maps = ~8 days. About half the decoration art uses this pipeline; the other half goes through ComfyUI Pipeline 3 with a constructivist LoRA at very high weight (essentially "decoration mode" of the parallax pipeline).

### Pipeline 5 — Hand-drawn stump caps (5 per chassis × 5 chassis = 25 sprites)

Per [12-rigging-and-damage.md](12-rigging-and-damage.md) §"Stump cap art is hand-drawn", AI is bad at "the inside of a torn-off mech limb" — minimal training data for the subject. We hand-draw all stump caps in Krita.

1. **Reference**: search HAER for cross-sections of pipes, valves, industrial conduits. The cross-section iconography is what we're emulating.
2. **Krita**: 128×128 working canvas (= 32×32 final), Ink-2 Fineliner.
3. **Draw**: torn metal silhouette + 3–5 cross-sections of cables/hydraulic lines + a fluid drip from the lowest point. ~25 minutes per cap.
4. **Add the map's dark + accent colors** (the stumps tint per chassis, not per map — chassis sprites use the *first map they appear on* as a default tint).
5. **Downsample to 32×32** with `-filter point`.
6. **Pack** into the chassis atlas.

5 stumps × 5 chassis × 25 min ≈ 10 hours total. One-time cost.

### Pipeline 6 — HUD icons (AI as sketch reference, hand-redraw at 16×16)

Goal: 16×16 weapon icons, kill-feed flag glyphs, pickup-category icons. AI generates *reference sketches* at 256×256; the engineer redraws each at 16×16 in Aseprite. AI is the brainstorm, not the deliverable.

1. **Generate**: Pipeline 1's setup, but at 256×256 with `EmptyLatentImage` `batch_size 8`. Prompt "single icon, weapon glyph, autocannon, white background, thick black ink line, no shading, centered, flat 2d, stencil, monochrome, military catalogue."
2. **Pick**: best of 8 references.
3. **Aseprite**: trace the silhouette at 16×16 pixel-by-pixel using the AI reference as a backdrop layer.
4. **Pack**: into the HUD atlas.

8 weapons + 5 pickup categories + 5 kill-feed flags = 18 icons. ~2 days at 6 icons/day.

### Pipeline 8 — Weapon atlas (canonical sheet, then crop per weapon)

Goal: a single `assets/sprites/weapons.png` atlas containing all 14 weapons (8 primaries + 6 secondaries), each visibly distinct per [12-rigging-and-damage.md](12-rigging-and-damage.md) §"Per-weapon visible art".

The same crop-from-canonical pattern as Pipeline 1, applied to weapons. Generate one image of all 14 weapons laid out on a "weapon rack" (or "armorer's catalog page") with shared style, then deterministic-crop each weapon to its sub-rect.

```
[CheckpointLoaderSimple] illustriousXL_v20.safetensors
   │
   └──► [LoraLoader] lineart_sdxl_v3.safetensors  weight 0.7
          └──► [LoraLoader] battletech_battlemechs.safetensors  weight 0.3
                 (low weight; biases toward "military catalogue page" aesthetic)

[LoadImage] tools/comfy/skeletons/weapon_rack_layout.png
                    (1024×1024 hand-drawn skeleton: 14 weapon outlines arranged in
                     a 4×4 grid minus 2 cells, each weapon drawn at its target
                     sprite size; magenta dots at grip + foregrip + muzzle for
                     each weapon)
   │
   └──► [LineartStandardPreprocessor]
          └──► [ControlNetApplyAdvanced]  strength 0.95   start 0.0   end 0.92

[LoadImage] tools/comfy/workflows/soldut/style_anchor/weapon_anchor.png
                    (the approved style — typically the chassis canonical T-pose
                     image, since weapons should match chassis surface aesthetic)
   │
   └──► [IPAdapterUnifiedLoader]  preset PLUS
          └──► [IPAdapterAdvanced]  weight 0.65   start 0.0   end 0.8

[CLIPTextEncode +]:
  "armorer catalogue page, weapon arrangement, technical illustration,
   line art, single weight ink, monochrome with red accent, factory stencils,
   panel lines, hydraulic lines, exposed mechanism, no human figure,
   no character, white background, no text, military catalogue"

[CLIPTextEncode -]:
  "photo, 3d render, smooth shading, gradient, glow, anime character, person,
   hand, holding, gloss, bloom, watermark, text, signature"

[KSampler]  steps 30   cfg 7.5   dpmpp_2m_sde   karras
            seed: 5000001 (locked across all weapon-atlas regenerations)
            denoise: 1.0   batch_size: 6 candidates
   │
   └──► [VAEDecode] → [SaveImage] weapon_rack_candidates_*.png
```

Pick the best candidate. Run an ImageMagick crop pass with the per-weapon bounding-box table:

```bash
# tools/build_weapon_atlas.sh — invoked once on weapon_rack_approved.png
magick weapon_rack_approved.png -crop 56x14+8+8     -strip   pulse_rifle.png
magick weapon_rack_approved.png -crop 48x16+72+8    -strip   plasma_smg.png
magick weapon_rack_approved.png -crop 60x24+136+8   -strip   riot_cannon.png
# ... (one line per weapon)
# Then re-pack with rTexPacker into assets/sprites/weapons.png
```

The pivot positions (grip / foregrip / muzzle) come from the magenta dots in the **input skeleton** (extract via a tiny ImageMagick `-channel R -threshold 95% -average` pass). Pivot pixel coords transcribe into `g_weapon_sprites` table at one-time art-bake.

**Realistic budget**: 1 day of iteration to lock the weapon rack output, ~1 day to slice + transcribe + tune the atlas. ~16 hours total.

Two-handed weapons need their foregrip position to fall on a sprite pixel that lines up with where a two-handed-grip mech hand should go (per [12-rigging-and-damage.md](12-rigging-and-damage.md) §"Foregrip positions per two-handed weapon"). Check by overlaying the foregrip dot and the grip dot on a working chassis sprite during the iteration loop — both hands should sit on visible weapon mounting points.

### Asset budget update — weapon atlas

| Asset | Size | Notes |
|---|---|---|
| Weapons atlas | 1024×512 RGBA8 | ~512 KB on disk, ~2 MB in RAM. Single binding for all weapon draws across all mechs. |
| Muzzle flash atlas | 256×256 RGBA8 | Per-weapon muzzle flash sprites (5 variants — small, medium, large, plasma, energy). Already in particle atlas. |

Total impact on the texture budget: +2 MB GPU-side, +512 KB on disk. Inside [10-performance-budget.md](../10-performance-budget.md) cap.

### Pipeline 7 — ImageMagick build-step (the consistency layer)

Every PNG that lands in `assets/` runs through a Makefile target. **This is non-negotiable** — it's the layer that erases ~80% of remaining AI tells:

```bash
# Makefile addition
ASSET_PNG := $(shell find assets -name '*.png' -not -path '*/raw/*' -not -path '*/proc/*')
ASSET_PROCESSED := $(ASSET_PNG:.png=.proc.png)

# Per-asset post: ordered-dither halftone, palette-remap, paper-noise multiply,
# point-filter downsample (CRITICAL — bicubic re-introduces AI smoothness).
%.proc.png: %.png
    magick $< \
        -ordered-dither h6x6a \
        -remap $(PALETTE) \
        \( assets/raw/paper_noise.png -alpha set -channel A -evaluate set 25% +channel \) \
        -compose multiply -composite \
        -filter point -resize 50% \
        $@

assets-process: $(ASSET_PROCESSED)
```

Three consequential moves in the post:
1. `-ordered-dither h6x6a` — Bayer-style halftone screen at the asset level. Pairs with the in-shader halftone post (§"The halftone post-process shader") for double-locked screen-print look.
2. `-remap palette.png` — hard-quantize to the map's 2-color palette. AI's subtle palette drift dies here.
3. `-filter point -resize 50%` — nearest-point downsample. **Never bicubic.** Bicubic smooths edges and re-introduces the AI-renderly feel.

Plus the per-asset palette PNG generation:
```bash
# tools/build_palettes.sh — run once, regenerated when src/palette.h changes
magick -size 2x1 xc:'#1A1612' xc:'#D8731A' assets/raw/palette_foundry.png
magick -size 2x1 xc:'#0F1416' xc:'#3FB6C2' assets/raw/palette_slipstream.png
# ... per map
```

The script runs as a build step; the per-asset PNG (under `assets/raw/`) is the source of truth, the `.proc.png` is what ships. The ImageMagick post is what makes 100 sprites from disparate ComfyUI runs look like they shipped from the same press.

## Defeating the AI tells — checklist

Every shipped asset gets evaluated against this checklist. Anything that fails goes back through Pipeline 1 with a corrective prompt or back to Aseprite for cleanup.

| AI tell | Visible artifact | Defeat |
|---|---|---|
| Smeared text | Gibberish on stencils | Generate without text; render stencils post-AI from real VG5000 (constraint #4). |
| Soft glow on every edge | Gentle gradient where you want a hard ink line | KSampler `cfg ≥ 7.5` + post `posterize 4` + halftone shader. The Pipeline 7 post is the safety net. |
| Inconsistent style across a set | Shoulder pad on Mech-A reads different from Mech-B | Locked seed + IP-Adapter anchor + LoRA stack at the same weights for every chassis. The canonical-T-pose pipeline produces 12 cohesive parts in one pass. |
| Plausible-but-wrong geometry | Hydraulic mounts go nowhere; foot doesn't connect | ControlNet-lineart strength 0.92 over a hand-drawn skeleton. The skeleton dictates geometry; the AI fills surface only. |
| Smooth-everywhere surfaces | Painterly metal where you want flat-fill | `ImageQuantize` to ≤4 colors with `dither none` + the ImageMagick palette-remap. |
| Generic indie palette | Slightly purple-tinted neutral grays | `PixelArtLoadPalettes` in ComfyUI + `magick -remap palette.png` in build step. |
| Plate seams that don't tile | Adjacent parts have mismatched panel-line styles | Crop from one canonical T-pose, never generate parts independently. |
| Negative-space hallucinations | AI invents detail in alpha regions | Pure white background prompt + alpha-key in post (`magick -fuzz 5% -transparent white`). |
| The "AI face" tell | Anime-leaning cute face on the helmet visor | Negative prompt: `anime_face, character_eyes, human_face, character`. Heavy weight. |
| Default lighting (always 3/4 from upper-left) | Every mech looks lit the same way | Negative prompt: `soft lighting, ambient occlusion, painterly`. KSampler `cfg 7.5+`. |

The ImageMagick post-process pipeline (Pipeline 7) is the safety net that catches anything that escapes prompt control. **Run the post on every asset, every time, no exceptions.**

## The mandatory iteration log

ComfyUI generation is non-deterministic across model updates and silently regresses. Every approved asset gets a row in `documents/art_log.md`:

```
| Asset | Workflow | LoRA stack | IP-Adapter | Seed | Prompt-version | Approved |
|---|---|---|---|---|---|---|
| trooper.png | mech_chassis_canonical_v3.json | battletech 0.6 / SRD 0.4 / lineart 0.5 | trooper_anchor_v2.png | 1000001 | v3 | 2026-05-12 |
| scout.png | mech_chassis_canonical_v3.json | battletech 0.5 / SRD 0.5 / lineart 0.6 | scout_anchor_v1.png | 2000002 | v3 | 2026-05-13 |
```

Without this log, in 3 weeks when you need to add a 6th chassis or a damage variant, you can't reproduce the look. The log is a hard requirement, not a polish item.

## The halftone post-process shader

`assets/shaders/halftone_post.fs.glsl` — the single load-bearing fragment shader for the aesthetic.

```glsl
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2  resolution;
uniform float halftone_density;   // 0..1; we ship at 0.30
out vec4 finalColor;

// 8x8 Bayer matrix, tilted halftone-style.
const float bayer8[64] = float[](
     0.0/64.,  32.0/64.,   8.0/64.,  40.0/64.,   2.0/64.,  34.0/64.,  10.0/64.,  42.0/64.,
    48.0/64.,  16.0/64.,  56.0/64.,  24.0/64.,  50.0/64.,  18.0/64.,  58.0/64.,  26.0/64.,
    12.0/64.,  44.0/64.,   4.0/64.,  36.0/64.,  14.0/64.,  46.0/64.,   6.0/64.,  38.0/64.,
    60.0/64.,  28.0/64.,  52.0/64.,  20.0/64.,  62.0/64.,  30.0/64.,  54.0/64.,  22.0/64.,
     3.0/64.,  35.0/64.,  11.0/64.,  43.0/64.,   1.0/64.,  33.0/64.,   9.0/64.,  41.0/64.,
    51.0/64.,  19.0/64.,  59.0/64.,  27.0/64.,  49.0/64.,  17.0/64.,  57.0/64.,  25.0/64.,
    15.0/64.,  47.0/64.,   7.0/64.,  39.0/64.,  13.0/64.,  45.0/64.,   5.0/64.,  37.0/64.,
    63.0/64.,  31.0/64.,  55.0/64.,  23.0/64.,  61.0/64.,  29.0/64.,  53.0/64.,  21.0/64.
);

void main() {
    vec4 col = texture(texture0, fragTexCoord);
    // Convert to luminance + value.
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Pixel-grid coords for the Bayer index.
    ivec2 ip = ivec2(gl_FragCoord.xy) % 8;
    float thresh = bayer8[ip.y * 8 + ip.x];
    // Apply halftone: pixels brighter than threshold * (1 - density)
    // pass through saturated; darker pixels darken to 60%.
    float t = step(thresh * (1.0 - halftone_density), lum);
    vec3 dark  = col.rgb * 0.6;
    vec3 light = col.rgb;
    finalColor = vec4(mix(dark, light, t), col.a);
}
```

Wired into the existing `post_fx` shader slot in `src/render.c`. Performance: 1 texture sample, 2 multiplies, 1 step, 1 mix per pixel = ~0.5 ms at 1920×1080 on integrated GPUs. Inside budget.

The shader's `halftone_density` is exposed as a config slider (in M6). Default 0.30 produces the look. Fully off (0.0) returns to vanilla flat shading.

References for the shader implementation:
- [Surma — Ditherpunk article](https://surma.dev/things/ditherpunk/) — canonical writeup
- [Daniel Ilett — Obra Dinn dithering tutorial](https://danielilett.com/2020-02-26-tut3-9-obra-dithering/) — fragment shader walkthrough

## Per-map kit assets

Each map ships with a directory bundle:

```
assets/maps/<short>/
├── <short>.lvl
├── thumb.png             # 256×144 vote-picker thumbnail
├── parallax_far.png      # 1024×512 sky / distant
├── parallax_mid.png      # 1024×512 mid silhouettes
├── parallax_near.png     # 1024×512 foreground hatching
├── tiles.png             # 256×256 — the kit's tile atlas (8×8 grid of 32×32)
└── decorations/
    ├── pipe_horizontal.png    # 64×16
    ├── steam_plume.png        # 32×96
    ├── valve_assembly.png     # 64×64
    └── ...
```

8 maps × ~1 MB each (PNG-compressed) = 8 MB of map-specific art. Plus shared assets (mech atlases × 5 = ~10 MB compressed; HUD atlas; pickup atlas; particle atlas) = ~25 MB total of art. Inside the [10-performance-budget.md](../10-performance-budget.md) cap.

## License hygiene for AI-assisted assets

The legal landscape as of May 2026:

- **Pure AI output is not copyrightable in the US.** SCOTUS denied cert in *Thaler v. Perlmutter* on March 2, 2026; the DC Circuit's rule stands. AI-generated assets can ship commercially but can't be registered for copyright on their own.
- **AI-assisted output with substantial human creative direction CAN be registered**, with the AI portion disclaimed. The 2023 *Zarya of the Dawn* registration is the precedent. Our pipeline (hand-drawn skeletons driving ControlNet, palette-remap, halftone post, manual cleanup, integration) provides the human-direction story.
- **Steam disclosure (Jan 17, 2026 update)**: pre-generated AI content needs a generic disclosure on the store page; live-generated does not apply to us; dev tools are exempt. Our disclosure: "Some 2D background and concept art assets generated with the assistance of Stable Diffusion via ComfyUI, with substantial post-generation editing, palette mapping, and integration by the developer."
- **Model licensing**: Illustrious-XL inherits SDXL's OpenRAIL-M (commercial output OK). Flux.2-Klein commercial usage requires a paid tier. Civitai LoRAs vary — check each model card before shipping.

**`assets/credits.txt` format** — every shipped asset has a row:

```
foundry/parallax_mid.png
  source: ComfyUI workflow parallax_layer_v3.json
  base model: Illustrious-XL v2.0 (OpenRAIL-M)
  loras: soviet_constructivist_poster.safetensors @ 0.7
  ip_adapter: foundry_anchor_v2.png
  seed: 1234567
  workflow_hash: abc123de…
  modification: 4× downsample, 2-color palette remap, halftone post
  human_direction: hand-drawn silhouette skeleton, palette authored, post-processed

foundry/decorations/boiler_horizontal.png
  source: HAER plate HAER PA-103-12 (Bethlehem Steel Plant)
  license: PD (US Government work)
  modification: traced via Inkscape Bitmap Trace, recolored to Foundry palette,
                halftone fill on shaded regions
  human_direction: trace edits, palette match, sprite cropping

trooper/stump_shoulder_l.png
  source: hand-drawn in Krita, no AI
  license: original Soldut, CC0 release
```

Build-time script (`tools/credits/check.sh`) walks `assets/` and fails CI if any file lacks a credits row. The script also greps for any `.workflow.json` referenced and refuses to build if the workflow file isn't checked in (so the asset is reproducible).

**In-game About screen**: lists CC-BY models (Battletech LoRA, Soviet Constructivist LoRA), HAER public-domain attributions, and the Steam-required generic AI disclosure. Length: ~half a screen.

**What we refuse to disclose as "AI-free"**: anywhere on the store page, in marketing, or in `credits.txt` — even informally. Don't claim "100% hand-drawn" anywhere; that's the only way a hybrid pipeline turns from boring into a lawsuit. Stick with "AI-assisted with substantial post-generation editing" as the standard disclosure phrase.

## Risk: when to escalate

The recipe assumes one engineer running ComfyUI on a local GPU (12+ GB VRAM). Realistic outcome: 4 weeks of asset work produces ~85% of the visual target. The remaining 15% is the slip-budget item — likely:

- One chassis (probably Sniper or Engineer — the asymmetric ones) where the ComfyUI output never quite locks the silhouette.
- One map's parallax that doesn't read as belonging to the same world as the others.
- The HUD's full-art pass, where 16×16 icons are tedious to redraw cleanly.

**Escalation path 1 — train a Soldut-specific LoRA.** Around mid-M5, after 30–40 approved sprites exist, train a custom LoRA on those approved outputs using `kohya_ss/sd-scripts`. ~4–6 hour training run on a 12GB GPU. The Soldut-LoRA then locks consistency across the rest of the chassis and makes the canonical-T-pose pipeline more reliable. This is the "scale to 5+ chassis without drift" move.

**Escalation path 2 — commission a working illustrator.** A $300–$800 commission for one chassis + one parallax kit. Brief: the five hard constraints, the palette table, the VG5000 reference, the Battletech TRO 3025 register. Specifically *not* "draw it however you want" — the constraints are the brief. The illustrator produces source PNGs that go through the same Pipeline 7 post-process so they integrate visually with the AI-assisted assets.

**Escalation path 3 — relax the constraints.** If the bake-test reveals players don't care about the visual fidelity (because the gameplay is good and the halftone post is doing the unification), accept slightly looser AI output as final. The kill-criterion is "a blind playtester can identify it as AI" — if they can't, we're shipping.

We don't escalate at week 1. We re-evaluate at week 4 against the bake-test result.

## What we refuse

- **First-thing-out-of-the-prompt AI output as ship asset.** Every shipped sprite passes through ControlNet-locked geometry, IP-Adapter style anchor, and the Pipeline 7 post. "I generated this in one shot, looks fine" doesn't ship.
- **Stock 3D mech models flattened to 2D.** The flatness has to be authored, not rendered.
- **Synthwave neon palettes**, even one map of "this one's the cyberpunk map." The two-color palette discipline rules it out.
- **Kawaii / chibi / anime-cute deformation.** The mechs are industrial vehicles, not characters. Negative prompt is load-bearing here.
- **Pre-rendered sprite-sheets from someone else's game.** License hygiene + visual-recognition risk.
- **AI-generated text on assets.** AI cannot render text legibly. Stencils are rendered by the engine post-AI (constraint #4).
- **Crediting the game as "100% hand-drawn"** anywhere, ever. The disclosure is "AI-assisted with substantial post-generation editing." Don't tell the lie.

## Done when

- All 8 maps have palette + parallax + tile atlas + decorations shipped under `assets/maps/<short>/`.
- All 5 chassis have 1024² atlases under `assets/sprites/<chassis>.png` with the per-part rigging spec from [12-rigging-and-damage.md](12-rigging-and-damage.md).
- All 25 stump caps (5 per chassis) are hand-drawn and atlased.
- HUD atlas, pickup atlas, particle atlas exist with line-art icons.
- Atkinson Hyperlegible + VG5000 + Steps Mono ship in `assets/fonts/`.
- `assets/shaders/halftone_post.fs.glsl` is wired into the post_fx slot; the screen post produces the halftone look.
- `assets/credits.txt` lists every shipped asset with source + license + workflow hash where AI-assisted.
- `tools/credits/check.sh` passes.
- The mandatory `documents/art_log.md` has a row for every approved asset (workflow file, LoRA stack, seed, IP-Adapter anchor, prompt version).
- All ComfyUI workflow JSONs are checked in to `tools/comfy/workflows/soldut/`.
- A blind playtester sees the game and **cannot identify a specific AI generator's signature** in the visuals. (The kill-criterion of the entire art direction.)

## Trade-offs to log

- **No animated mech sprites.** Every chassis sprite is static; the bone system articulates. No frame-by-frame animation anywhere. (Cuphead's lesson: budget reality.)
- **AI generation is the production hot path** with the four-part defeat-the-tells discipline (ControlNet skeleton + IP-Adapter anchor + LoRA stack + Pipeline 7 post). The earlier "no AI" stance is rejected; the discipline replaces it.
- **Canonical-T-pose-then-crop** instead of per-part generation. Single biggest workflow choice. Per-part generation produces inconsistent panel-line styles at every joint; canonical-then-crop produces parts that look like they belong to the same machine.
- **Stump caps are hand-drawn**, not AI-generated. AI's training data for "torn-off interior of a mech limb" is sparse; the result looks worse than 30 minutes of Krita. ~10 hours total for 25 stumps, paid once.
- **HUD icons are AI-as-sketch + Aseprite-as-deliverable** at 16×16. AI-direct generation at 16×16 produces unreadable mush. ~2 days for 18 icons.
- **Damage variants are NOT shipped sprites at v1.** [12-rigging-and-damage.md](12-rigging-and-damage.md) §"Layer 2" specifies decal-overlay damage instead, which obviates the need for per-state sprite variants. Saves ~30 hours of art. Pipeline 2 (img2img damage variants) documented for M6 polish only.
- **Three chassis canonical T-poses, not five.** Trooper / Heavy / Sniper are authored fully; Scout shares its silhouette skeleton with Trooper at smaller scale; Engineer shares with Trooper at the same scale with shoulder/jet differences. Reduces ComfyUI iteration time by ~40%; the silhouette-pair maps look related, which we accept.
- **Halftone post is a fragment shader + per-asset ImageMagick post.** Two layers of halftone discipline; means the shader is *required* — turn it off and the AI tells leak through.
- **VG5000 stencils are engine-rendered**, not baked into sprite. Means we can re-render them at higher resolution if we ever upgrade the asset pipeline; means stencils never look AI-melted.
- **Per-asset workflow JSON, IP-Adapter anchor, and seed must be reproducible.** The art-log discipline is non-optional. Without it, regenerating an asset with a current ComfyUI version diverges silently from the original.
- **Map palettes are compiled into `palette.h`.** A modder who wants Foundry-but-blue has to recompile. (M6 stretch: read `palette.cfg`.)
- **Three of eight parallax kits are shared across map pairs** with palette tints. Reduces parallax workload from 24 to ~16 unique generations.
