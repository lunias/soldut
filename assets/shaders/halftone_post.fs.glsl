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
 * Run after EndMode2D, before HUD, on the full framebuffer:
 *   BeginShaderMode(g_halftone_post);
 *     DrawTextureRec(post_target.texture, ..., WHITE);
 *   EndShaderMode();
 *
 * Uniforms (set once per frame in renderer_draw_frame):
 *   resolution        — backbuffer pixel size (texelFetch / fragCoord scale)
 *   halftone_density  — 0..1; ship at 0.30 per the spec. 0 = pass-through.
 *
 * References: Surma's Ditherpunk, Daniel Ilett's Obra Dinn writeup. The
 * 8x8 Bayer matrix below is the classic ordered dither pattern; threshold
 * the Bayer cell against the source luminance to decide which pixels
 * "burn through" and which darken.
 */

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4      colDiffuse;
uniform vec2      resolution;
uniform float     halftone_density;

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

void main() {
    vec4 src = texture(texture0, fragTexCoord) * colDiffuse * fragColor;

    /* Density 0 = pass-through. Useful for A/B and for screenshot diffs. */
    if (halftone_density <= 0.001) {
        finalColor = src;
        return;
    }

    /* Pick this pixel's Bayer threshold from the 8x8 cell. gl_FragCoord
     * is in pixel-space (post-viewport-transform) so modulo-8 gives a
     * stable pattern across resize. */
    ivec2 ip = ivec2(int(gl_FragCoord.x), int(gl_FragCoord.y));
    int   bi = (ip.y & 7) * 8 + (ip.x & 7);
    float th = bayer8[bi];

    /* Convert source RGB → luminance (ITU-R BT.601). Pixels brighter
     * than the threshold pass through saturated; darker ones drop to
     * 60% — same shape as documents/m5/11-art-direction.md §"shader". */
    float lum = dot(src.rgb, vec3(0.299, 0.587, 0.114));
    float t   = step(th * (1.0 - halftone_density), lum);
    vec3  dark  = src.rgb * 0.6;
    vec3  light = src.rgb;

    finalColor = vec4(mix(dark, light, t), src.a);
}
