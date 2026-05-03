# Reference — Sources

Every URL cited across these documents, plus a few worth keeping for follow-up. Grouped by topic.

## Soldat (the spiritual predecessor)

- Soldat homepage — https://www.soldat.pl/en/
- Soldat source (open-sourced 2020, MIT) — https://github.com/Soldat/soldat
  - `shared/Constants.pas` — physics & gameplay constants
  - `shared/Parts.pas` — particle/Verlet/constraint system (the gostek body)
  - `shared/PolyMap.pas` — map polygon collision
  - `shared/Anims.pas` — animation system (`.poa` files)
  - `shared/Weapons.pas` (and `weapons.ini` per distribution) — weapon stat table
  - `shared/MapFile.pas` — `.PMS` map format (we replace with our own)
- Soldat wiki — https://wiki.soldat.pl/

## raylib + stb

- raylib — https://www.raylib.com / https://github.com/raysan5/raylib
- raylib source structure (relevant headers in our local clone):
  - `src/raylib.h`, `src/raymath.h`, `src/rlgl.h`
  - `src/rcore.c`, `src/rshapes.c`, `src/rtextures.c`, `src/rtext.c`, `src/raudio.c`
  - `src/external/` — bundled stb_image, stb_truetype, stb_vorbis, stb_perlin, stb_rect_pack, stb_image_resize2, stb_image_write, miniaudio, glad, glfw, par_shapes
- raygui (single-header IMGUI) — https://github.com/raysan5/raygui
- stb single-header libs — https://github.com/nothings/stb
- stb_ds.h docs — https://github.com/nothings/stb/blob/master/docs/stb_ds.md
- Sean Barrett — https://nothings.org / "Single-File Public-Domain Libraries" Handmade Con 2014 talk
- miniaudio (used inside raylib) — https://miniaud.io

## Networking

- Glenn Fiedler — *Gaffer on Games* — https://gafferongames.com
  - *What Every Programmer Needs To Know About Game Networking* — https://gafferongames.com/post/what_every_programmer_needs_to_know_about_game_networking/
  - *Networking for Game Programmers* (series)
  - *Snapshot Interpolation* — https://gafferongames.com/post/snapshot_interpolation/
  - *Snapshot Compression* — https://gafferongames.com/post/snapshot_compression/
  - *State Synchronization* — https://gafferongames.com/post/state_synchronization/
  - *Reliable Ordered Messages* — https://gafferongames.com/post/reliable_ordered_messages/
  - *UDP vs. TCP* — https://gafferongames.com/post/udp_vs_tcp/
  - *Deterministic Lockstep* — https://gafferongames.com/post/deterministic_lockstep/
  - *Fix Your Timestep!* — https://gafferongames.com/post/fix_your_timestep/
- Yahn Bernier (Valve) — *Latency Compensating Methods in Client/Server In-game Protocol Design and Optimization* — https://developer.valvesoftware.com/wiki/Latency_Compensating_Methods_in_Client/Server_In-game_Protocol_Design_and_Optimization
- Valve — *Source Multiplayer Networking* — https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking
- Tim Ford & Dan Reed (Blizzard) — *'Overwatch' Gameplay Architecture and Netcode* — GDC 2017 — https://www.gdcvault.com/play/1024001/-Overwatch-Gameplay-Architecture-and
- Frohnmayer & Gift — *The Tribes Engine Networking Model* — https://www.gamedevs.org/uploads/tribes-networking-model.pdf
- Quake 3 networking writeup (Fabien Sanglard) — https://fabiensanglard.net/quake3/network.php
- Riot Games — *Peeking Behind the Curtains of Valorant's Netcode* — https://technology.riotgames.com/news/peeking-valorants-netcode
- Gabriel Gambetta — *Fast-Paced Multiplayer* (4-part series) — https://www.gabrielgambetta.com/client-server-game-architecture.html
- ENet — http://enet.bespin.org / https://github.com/lsalzman/enet (and active fork: https://github.com/zpl-c/enet)
- Valve GameNetworkingSockets — https://github.com/ValveSoftware/GameNetworkingSockets
- netcode.io / reliable.io / yojimbo — https://github.com/networkprotocol

## Physics (ragdoll, constraints, Verlet)

- Thomas Jakobsen — *Advanced Character Physics* — GDC 2001 (the Hitman ragdoll paper). Search: "Jakobsen Advanced Character Physics PDF"
- Müller, Heidelberger, Hennix, Ratcliff — *Position Based Dynamics* — JVCIR 2007
- Macklin, Müller, Chentanez — *XPBD: Position-Based Simulation of Compliant Constrained Dynamics* — Motion in Games 2016
- Erin Catto / Box2D — https://box2d.org
  - *Iterative Dynamics with Temporal Coherence* — GDC 2005
  - *Fast and Simple Physics using Sequential Impulses* — GDC 2006 — https://box2d.org/files/ErinCatto_SequentialImpulses_GDC2006.pdf
  - *Modeling and Solving Constraints* — GDC 2009 — https://box2d.org/files/ErinCatto_ModelingAndSolvingConstraints_GDC2009.pdf
  - *Soft Constraints* — GDC 2011
  - *Understanding Constraints* — GDC 2014 — https://box2d.org/files/ErinCatto_UnderstandingConstraints_GDC2014.pdf
- Box2D source — https://github.com/erincatto/box2d
- Chipmunk2D — https://chipmunk-physics.net/

## Cross-platform build / distribution

- zig — https://ziglang.org
- *Cross-compile a C/C++ Project with Zig* — https://zig.news/kristoff/cross-compile-a-c-c-project-with-zig-3599
- Apple — *Notarizing macOS Software Before Distribution* — https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution
- Apple — *Signing Mac Software with Developer ID* — https://developer.apple.com/developer-id/
- mingw-w64 — https://www.mingw-w64.org/

## Level design

- *The Level Design Book* — https://book.leveldesignbook.com
- *The Language of Arena FPS Level Design* — https://www.plusforward.net/post/21433/The-Language-of-Arena-FPS-Level-Design/
- War Robots Universe — *Top-down shooter level design: how map design supports game mechanics* — https://medium.com/my-games-company/top-down-shooter-level-design-how-map-design-supports-game-mechanics-6ae39fdd095d

## Philosophy / craft

- Casey Muratori — Handmade Hero — https://handmadehero.org
- Casey Muratori — *Compression-Oriented Programming* — https://hero.handmade.network/forums/code-discussion/t/87-compression_oriented_programming
- Casey Muratori — *Semantic Compression* — https://caseymuratori.com/blog_0015
- Casey Muratori — *Complexity and Granularity* — https://caseymuratori.com/blog_0016
- Handmade Network — https://handmade.network
- Jonathan Blow — Jai language presentations & demos (e.g., YouTube *Jai Demo and Design Explanation*)
- Jonathan Blow — talks on game development workflow (e.g., *Programming Aesthetics Learnt From Making Inflatable Dolls*)
- *JaiPrimer* (Jai overview) — https://github.com/BSVino/JaiPrimer

## C / engineering

- C11 standard — ISO/IEC 9899:2011
- *Modern C* by Jens Gustedt — open: https://gustedt.gitlabpages.inria.fr/modern-c/

## Misc

- Soldat default UDP port: **23073** (we adopt for homage)
- Public-domain image / sound resources we may use during prototyping:
  - Kenney.nl — https://kenney.nl
  - OpenGameArt — https://opengameart.org
  - Freesound — https://freesound.org

---

This list will grow. When we cite a new URL in a doc, it goes here too.
