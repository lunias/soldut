#version 330

/*
 * M5 P13 — Halftone post-process pass.
 *
 * Bayer 8x8 ordered dither + a flat-shaded "screen tint" that crushes
 * everything between black and the source color into a two-step screen.
 * Pairs with the per-asset ImageMagick halftone (Pipeline 7 in
 * documents/m5/11-art-direction.md) so individual asset roughness gets
 * unified screen-print under a single shader pass — the canonical
 * "Industrial Service Manual" look.
 *
 * M6 P02 adds a second pass folded into the same shader: a per-fragment
 * heat-shimmer that perturbs the source-texture UV near up to 16 active
 * jet-plume "hot zones." The zones are screen-space (xy, radius,
 * intensity) and the noise is a cheap two-octave sin/cos at fragment-
 * derived frequencies advanced by `jet_time`. Zones with w<=0 are
 * skipped — pad unused slots with zero and the loop costs nothing
 * visible. The CPU side pushes the uniform array only when at least
 * one mech has MECH_JET_ACTIVE set.
 *
 * M6 P09 adds three more layers, each behind its own zero-init
 * short-circuit so default maps cost zero extra shader work:
 *   - `fog_density` + `fog_color`        — global fog tint.
 *   - `fog_zones[16]` + `fog_zone_count` — AMBI_FOG volumes, peer of
 *     the existing jet_hot_zones[16]. Each zone is
 *     (center_screen_x, center_screen_y, radius_px, density).
 *   - `vignette_strength`                — radial corner darkening.
 *
 * Run after EndMode2D, before HUD, on the full framebuffer:
 *   BeginShaderMode(g_halftone_post);
 *     DrawTextureRec(post_target.texture, ..., WHITE);
 *   EndShaderMode();
 *
 * Uniforms (set per-frame in renderer_draw_frame):
 *   resolution        — backbuffer pixel size (texelFetch / fragCoord scale)
 *   halftone_density  — 0..1; ship at 0.30 per the spec. 0 = pass-through.
 *   jet_hot_zones[16] — (sx, sy, radius_px, intensity); w==0 → skip
 *   jet_time          — monotonic seconds for the shimmer noise phase
 *   fog_density       — global fog density [0..1]; 0 = off
 *   fog_color         — global + zone fog tint (RGB)
 *   fog_zones[16]     — (cx_screen, cy_screen, radius_px, density)
 *   fog_zone_count    — actual filled count
 *   vignette_strength — radial corner darkening [0..1]; 0 = off
 *   atmos_time        — monotonic seconds for atmosphere effects
 *
 * References: Surma's Ditherpunk, Daniel Ilett's Obra Dinn writeup. The
 * 8x8 Bayer matrix below is the classic ordered dither pattern; threshold
 * the Bayer cell against the source luminance to decide which pixels
 * "burn through" and which darken. Shimmer reference: Linden Reid's
 * heat-distortion shader tutorial. Fog reference: raylib's
 * shaders_fog_rendering example (3D ported to 2D screen-space).
 */

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4      colDiffuse;
uniform vec2      resolution;
uniform float     halftone_density;

#define JET_HOT_ZONE_MAX 16
uniform vec4  jet_hot_zones[JET_HOT_ZONE_MAX];
uniform float jet_time;
/* M6 P02-perf — actual filled count for the loop bound. Most ticks
 * have 0-4 active zones; iterating to MAX every fragment at 4K is
 * 133M wasted op/frame just for the `z.w <= 0.001` check. Uniform-
 * bounded loop lets the driver shrink the iteration count to what's
 * actually needed. */
uniform int   jet_hot_zone_count;

/* M6 P09 — atmospherics uniforms. All zero-default so a pre-P09 map
 * (LvlMeta atmosphere fields = 0) costs nothing in this shader. */
#define FOG_ZONE_MAX 16
uniform float fog_density        = 0.0;
uniform vec3  fog_color          = vec3(1.0, 1.0, 1.0);
uniform vec4  fog_zones[FOG_ZONE_MAX];
uniform int   fog_zone_count     = 0;
uniform float vignette_strength  = 0.0;
uniform float atmos_time         = 0.0;

/* M6 P09 (post-user-feedback) — weather scene tinting. Each is a
 * 0..1 fraction matching g_atmosphere.snow_accum / .rain_wetness
 * respectively, advanced by atmosphere_advance_accumulators on every
 * peer (deterministic across multiplayer).
 *
 *   snow_intensity — cool tint + shadow lift + sparkle on bright
 *                    pixels (the accumulated snow pile + the snow
 *                    flake particles together read as a cohesive
 *                    snowscape under this pass).
 *   rain_intensity — slight darken + cool blue cast (wet surfaces
 *                    appear darker + more reflective).
 *
 * Both behind <= 0.001 short-circuits so default maps cost nothing. */
uniform float snow_intensity     = 0.0;
uniform float rain_intensity     = 0.0;

out vec4 finalColor;

/* 8x8 Bayer matrix, normalized to [0, 1). Standard ordered-dither layout
 * (van Wezel / Limb 1969). Indices wrap modulo 8 — every 8x8 block in
 * screen space gets the same threshold pattern, which is what gives the
 * print-screen look. */
const float bayer8[64] = float[](
     0.0/64.0,  32.0/64.0,   8.0/64.0,  40.0/64.0,   2.0/64.0,  34.0/64.0,  10.0/64.0,  42.0/64.0,
    48.0/64.0,  16.0/64.0,  56.0/64.0,  24.0/64.0,  50.0/64.0,  18.0/64.0,  58.0/64.0,  26.0/64.0,
    12.0/64.0,  44.0/64.0,   4.0/64.0,  36.0/64.0,  14.0/64.0,  46.0/64.0,   6.0/64.0,  38.0/64.0,
    60.0/64.0,  28.0/64.0,  52.0/64.0,  20.0/64.0,  62.0/64.0,  30.0/64.0,  54.0/64.0,  22.0/64.0,
     3.0/64.0,  35.0/64.0,  11.0/64.0,  43.0/64.0,   1.0/64.0,  33.0/64.0,   9.0/64.0,  41.0/64.0,
    51.0/64.0,  19.0/64.0,  59.0/64.0,  27.0/64.0,  49.0/64.0,  17.0/64.0,  57.0/64.0,  25.0/64.0,
    15.0/64.0,  47.0/64.0,   7.0/64.0,  39.0/64.0,  13.0/64.0,  45.0/64.0,   5.0/64.0,  37.0/64.0,
    63.0/64.0,  31.0/64.0,  55.0/64.0,  23.0/64.0,  61.0/64.0,  29.0/64.0,  53.0/64.0,  21.0/64.0
);

/* Heat-shimmer UV displacement. Iterates the 16-slot hot-zone uniform
 * array and accumulates a sin/cos noise scaled by an exp-falloff weight
 * around each zone. Zones with intensity <= 0 short-circuit so the
 * empty common-case is one comparison per slot. */
vec2 shimmer_offset(vec2 frag_px) {
    vec2 offs = vec2(0.0);
    /* Loop is dynamically-bounded on `jet_hot_zone_count`. Most
     * drivers compile this efficiently when the bound is a uniform
     * — when no zones are active, the loop body doesn't execute
     * at all (vs the old MAX-bounded version which always paid the
     * "is this slot empty?" check). The MAX_* clamp is defensive
     * against a malformed uniform push. */
    int n = jet_hot_zone_count;
    if (n > JET_HOT_ZONE_MAX) n = JET_HOT_ZONE_MAX;
    for (int i = 0; i < n; ++i) {
        vec4 z = jet_hot_zones[i];
        if (z.w <= 0.001) continue;
        vec2 d = frag_px - z.xy;
        float r2 = dot(d, d);
        float r_sq = z.z * z.z;
        if (r_sq < 1.0) continue;
        float falloff = exp(-r2 / r_sq);
        if (falloff < 0.01) continue;
        /* Two octaves — gives a more organic shimmer than one. */
        float n1 = sin(frag_px.x * 0.08 + jet_time * 3.0)
                 + cos(frag_px.y * 0.10 + jet_time * 2.7);
        float n2 = sin(frag_px.x * 0.21 - jet_time * 4.1)
                 + cos(frag_px.y * 0.19 + jet_time * 3.5);
        offs += vec2(n1, n2) * falloff * z.w * 0.6;
    }
    return offs;
}

/* M6 P09 — Per-zone fog accumulator. Returns the per-fragment fog
 * density contribution (0..1) summed across all active fog zones.
 * Falloff is exp(-r²/zone_radius²), so the zone bleeds softly at the
 * rim. */
float zone_fog_density(vec2 frag_px) {
    if (fog_zone_count <= 0) return 0.0;
    float total = 0.0;
    int n = fog_zone_count;
    if (n > FOG_ZONE_MAX) n = FOG_ZONE_MAX;
    for (int i = 0; i < n; ++i) {
        vec4 z = fog_zones[i];
        if (z.w <= 0.001) continue;
        vec2 d = frag_px - z.xy;
        float r2 = dot(d, d);
        float r_sq = z.z * z.z;
        if (r_sq < 1.0) continue;
        float falloff = exp(-r2 / r_sq);
        if (falloff < 0.01) continue;
        total += falloff * z.w;
    }
    return clamp(total, 0.0, 1.0);
}

void main() {
    vec2 frag_px = fragTexCoord * resolution;
    vec2 shimmer = shimmer_offset(frag_px);
    vec2 uv      = (frag_px + shimmer) / resolution;
    vec4 src     = texture(texture0, uv) * colDiffuse * fragColor;

    vec3 color = src.rgb;

    /* M6 P09 — Global fog mix. exp() falloff would be more physical
     * but a straight mix() reads better at low densities (which is
     * the whole point of a per-map atmosphere knob). Short-circuited
     * on zero density so default maps stay byte-identical. */
    if (fog_density > 0.001) {
        color = mix(color, fog_color, fog_density);
    }
    /* M6 P09 — Per-zone fog accumulation. Each AMBI_FOG zone bleeds
     * with exp falloff around its center; the per-fragment density
     * sums (capped at 1) and tints toward fog_color. */
    float zd = zone_fog_density(frag_px);
    if (zd > 0.001) {
        color = mix(color, fog_color, zd);
    }

    /* Halftone pass. Density 0 = pass-through (still applies shimmer
     * + fog above). */
    if (halftone_density > 0.001) {
        /* Pick this pixel's Bayer threshold from the 8x8 cell. gl_FragCoord
         * is in pixel-space (post-viewport-transform) so modulo-8 gives a
         * stable pattern across resize. */
        ivec2 ip = ivec2(int(gl_FragCoord.x), int(gl_FragCoord.y));
        int   bi = (ip.y & 7) * 8 + (ip.x & 7);
        float th = bayer8[bi];

        /* Convert source RGB → luminance (ITU-R BT.601). Pixels brighter
         * than the threshold pass through saturated; darker ones drop to
         * 60% — same shape as documents/m5/11-art-direction.md §"shader". */
        float lum = dot(color, vec3(0.299, 0.587, 0.114));
        float t   = step(th * (1.0 - halftone_density), lum);
        vec3  dark  = color * 0.6;
        vec3  light = color;
        color = mix(dark, light, t);
    }

    /* M6 P09 — SNOW scene treatment. Three blended effects, all
     * scaled by snow_intensity (= g_atmosphere.snow_accum):
     *   1. Cool blue-white tint over the whole frame (snowy days
     *      look slightly desaturated + blue-shifted).
     *   2. Shadow lift — snow surfaces reflect a lot of light, so
     *      the contrast between darks and lights compresses.
     *   3. Sparkle on bright pixels — a sin/cos noise gated above
     *      a luminance threshold drops glittering specs on the
     *      snow pile, animated by atmos_time. Self-skips when no
     *      bright pixels are on screen so a dark mid-scene doesn't
     *      pay for sparkle. */
    if (snow_intensity > 0.001) {
        /* Tint: blend toward a cool slightly-cyan grey-white. */
        vec3 snow_tint = vec3(0.92, 0.96, 1.04);
        color = mix(color, color * snow_tint, snow_intensity * 0.6);
        /* Shadow lift: add a small constant scaled by snow_intensity. */
        color += vec3(0.04, 0.05, 0.06) * snow_intensity;
        color = clamp(color, 0.0, 1.0);
        /* Sparkle on bright pixels (the accumulated pile). */
        float lum = dot(color, vec3(0.299, 0.587, 0.114));
        if (lum > 0.65) {
            vec2 ip2 = frag_px * 0.35;
            float s = sin(ip2.x + atmos_time * 4.5) *
                      cos(ip2.y - atmos_time * 3.7);
            s = s * s * s;                /* sharpen peaks */
            float spark_gate = (lum - 0.65) * 2.0;
            if (s > 0.55) {
                color += vec3(0.35, 0.38, 0.42) *
                         snow_intensity * spark_gate;
            }
        }
    }

    /* M6 P09 — RAIN scene treatment. Wet surfaces look darker
     * (water absorbs light, then specularly reflects bright
     * highlights). At full rain we slightly darken + cool the
     * frame; a per-fragment noise adds a faint scatter texture
     * that reads as falling rain haze. */
    if (rain_intensity > 0.001) {
        vec3 rain_tint = vec3(0.85, 0.90, 1.00);
        color = mix(color, color * rain_tint, rain_intensity * 0.5);
        /* Faint scatter for the rain haze look. */
        float n = sin(frag_px.x * 0.5 + atmos_time * 12.0) *
                  cos(frag_px.y * 0.7 - atmos_time * 9.0);
        color += vec3(n) * 0.015 * rain_intensity;
        color = clamp(color, 0.0, 1.0);
    }

    /* M6 P09 — Vignette. Standard `pow(distance_from_center, 2) *
     * strength` radial darken. Strength 0 = pass-through. */
    if (vignette_strength > 0.001) {
        vec2  uv_centered = fragTexCoord - vec2(0.5);
        float d = length(uv_centered) * 1.414;   /* normalise corner = 1 */
        float vig = 1.0 - vignette_strength * d * d;
        if (vig < 0.0) vig = 0.0;
        color *= vig;
    }

    finalColor = vec4(color, src.a);
}
