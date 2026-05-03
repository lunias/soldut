# 00 — Vision

## The pitch

A 32-player 2D side-scrolling multiplayer shooter where you pilot a **polygon-bodied mech** built out of plates connected by bones. You run, jet, slide, shoot, and die in arenas that reward exploration as much as combat. When you die, your mech *comes apart*: limbs detach, plates pinwheel, blood arcs across the level. Other players keep fighting around your wreck.

This is **Soldat** in a futuristic shell, with the ragdoll physics turned up and the aesthetic dragged forward thirty years.

## What it feels like to play

- You hold W to jet upward, A/D to drift, space to dash. Your mech's torso aims at the cursor; your legs swing as you run. Recoil punches your hands back, which ripples through your skeleton, which makes the next shot feel earned.
- You spawn, sprint to a side passage, find a Barrett-equivalent rail-cannon under a vent. You jet up to a rooftop, snipe a player crossing a gap, watch their leg fly off and the rest of them tumble.
- A grenade sails over your head. You backflip out of a doorway. Sparks, blood, decals, smoke. The map is loud and dirty within thirty seconds of the round starting.
- A round is **5–10 minutes**. The lobby fills before the next.

The texture of the play is not "tactical" and it is not "casual." It is **frenetic, loose, kinetic, and fair**. Every death is your fault and every kill is your reward.

## Design pillars

These are the load-bearing commitments. Every gameplay decision should be checked against them. Anything that violates a pillar is wrong even if it sounds fun.

1. **Movement is the protagonist.** The mech feels alive in your hands at all times. Jets, slides, rolls, and momentum carry between actions. Unforgiving gravity, generous air control. If movement isn't fun without combat, combat won't save us.

2. **Hits have weight.** A bullet doesn't just decrement a number. It rocks the target's skeleton via impulse, leaves a decal, splashes blood, makes a *sound* with low end. The visual and audible feedback of every shot is a system we build, not a coat of polish.

3. **Mechs come apart.** Ragdolls aren't a death animation, they are physics. Limbs detach when shot enough. Killshots launch bodies. Dead mechs become geometry the living step over.

4. **Maps reward both running and gunning.** Exploration finds equipment, heals, and shortcuts. Combat happens in the chokes the layout funnels you into. Hiding indefinitely is impossible; running through everything is suicidal. The map *makes you* meet other players.

5. **The host is the server.** No publisher infrastructure, no matchmaking service, no required login. A player types an IP and a port; another player hosts. The network layer is good enough that 32 players over a home connection is not a fantasy.

6. **It runs on a laptop.** Sixty frames per second on integrated graphics with 32 players in heavy combat. Memory budget under 256 MB resident. We do this by writing **less code, of higher quality, with explicit data layouts** — not by leaning on a black-box engine.

7. **C is the language, raylib is the foundation, stb fills the gaps.** No layers we can't read. No build system we can't reproduce on a fresh laptop in fifteen minutes. No dependencies that drag in twelve transitive libraries.

## What we are NOT

- **Not Battle Royale.** Round-based arena play, fixed teams or FFA, fast respawn. The lobby is the persistent thing; the match is the disposable thing.
- **Not a cinematic single-player game.** No story campaign at launch. A bot mode and an offline practice arena are stretch goals.
- **Not a competitive esport.** We balance for fun and feel. We don't build anti-cheat past what an authoritative server gives us for free. Servers are run by players, moderation is social.
- **Not free-to-play.** No microtransactions, no battle passes, no loot boxes. If we sell the game, we sell *the game*.
- **Not a Soldat clone with a new skin.** We respect Soldat's mechanics and copy what made it work. We are not afraid to throw things out: no parachute (mechs have jets), no team-color polygons (mechs read by silhouette), no .pas-file map format (we own our content pipeline).

## The aesthetic

- **Futuristic, not cyberpunk.** Clean panel lines, exposed pistons, hard edges. Think *Ghost in the Shell* + *Patlabor* + *Battletech* tabletop, less *Cyberpunk 2077* neon overload.
- **Polygons, visibly.** Mech bodies are flat-shaded plates with hand-drawn line work. No textures on the mechs themselves — color and seam lines tell the silhouette. Backgrounds are painted, with parallax.
- **Blood is bright.** Hydraulic fluid, plasma, oil — call it what you want, but it splashes red and orange and pools. Mechs leak. Wreckage smolders.
- **Sound is mechanical.** Servo whines. Hydraulic hisses. Concrete thuds when a leg severs. No orchestral score in match — pulse-driven low ambient with cues that punch up on big events.

## The "soul" we are preserving from Soldat

The original Soldat (Michał Marcinkowski, 2002, Pascal) is one of the few multiplayer shooters whose *feel* has aged well. What made it iconic, and what we copy:

- **Verlet-stick gostek.** The stick-figure body that ragdolls naturally because it never stopped being a ragdoll. Soldat's `Parts.pas` runs Verlet integration at all times — alive players are just ragdolls being puppeted by the animation system. We do the same. (See [03-physics-and-mechs.md](03-physics-and-mechs.md).)
- **Players don't collide with each other.** Bullets do. This single decision is most of why Soldat feels fast and fair. We keep it.
- **Weapon distinctness.** Soldat's 14 primaries each *felt different* — Desert Eagles vs Ruger vs Barrett vs Spas vs M79 — without a stat being objectively best. We copy the discipline: every weapon is a different *play style*, not a different *DPS number*.
- **Movement-tied accuracy** ("bink" and "self-bink" in Soldat). Shooting while jetting is less accurate than shooting from a planted stance. Movement choice and weapon choice interact. We keep this.
- **Maps are skill containers.** Soldat's `ctf_Ash`, `Voland`, `Kampf` etc. were small, dense, and rewarded learning. We commit to that scale, not 200×200 sprawls.

What we drop:

- The polygon-soup map authoring (we use tile + polygon hybrids — see [07-level-design.md](07-level-design.md)).
- The `.pas` codebase (we are in C).
- The team-vs-team-only mindset (we ship FFA + team modes).
- The no-mech-collision applied to *corpses* (we want corpses to be physical obstacles).
- The parachute (mechs jet).

## The success criterion

A new player downloads the executable, double-clicks it, joins a server, and within sixty seconds laughs out loud at something that just happened. They invite a friend that night.

That is the bar. Everything in these documents is in service of that minute.
