#version 440

// Blit an sRGB-authored UI panel (QPainter -> RGBA8) onto the lock surface.
//   HDR surface: linearise sRGB and scale to ~203 nits so white UI reads correctly.
//   SDR surface: pass the sRGB bytes straight through.
// Alpha is preserved for the alpha-blend in the pipeline.
//
// Optional rainbow: when rainbowOn, the panel's rainbow-able elements are drawn
// WHITE on the CPU side, and a rolling 45-degree band (built from the stops) is
// multiplied in here. Scrolling is just `phase` advancing -- no CPU re-render.
//
// Optional bloom: a soft halo from the blurred panel coverage, glowing in the band
// colour (rainbow) or the text's own colour. Off (strength 0) -> skipped entirely.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform U {
    float scale;
    float sdr;
    float rainbowOn;     // >0.5 -> apply the rainbow band
    float phase;         // 0..1 scroll offset
    float bandFreqX;     // band cycles per unit v_uv.x (encodes the 45deg dir + period)
    float bandFreqY;     // band cycles per unit v_uv.y
    float stopCount;     // number of valid stops (2..8)
    float bloomStrength; // glow amount (0 = off)
    float brightness;    // band luminance multiplier (>1 = HDR pop)
    float fadeAlpha;     // overlay opacity multiplier (fade-in on lock; 1 = fully shown)
    float _p2;
    float _p3;
    vec4 stops[8];       // rgba gradient stops (sRGB 0..1)
} u;

layout(binding = 1) uniform sampler2D panel;

vec3 srgbToLinear(vec3 c)
{
    vec3 lo = c / 12.92;
    vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(lo, hi, step(vec3(0.04045), c));
}

// Cyclic gradient over the stops: t wraps so stop[n-1] blends back to stop[0].
vec3 bandColor(float t)
{
    int n = int(u.stopCount + 0.5);
    if (n < 2) return vec3(1.0);
    float f = fract(t) * float(n);
    int i = int(f) % n;
    int j = (i + 1) % n;
    return mix(u.stops[i].rgb, u.stops[j].rgb, fract(f));
}

// Blurred panel sample for the glow: rgb = premultiplied blurred colour, a = coverage.
// Circular in pixels (offsets aspect-corrected via the panel size).
vec4 bloomSample(vec2 uv)
{
    const int R = 3;
    const float radius = 0.012; // uv-x units
    ivec2 sz = textureSize(panel, 0);
    vec2 stp = vec2(radius, radius * float(sz.x) / float(sz.y)) / float(R);

    // Sample the mip level whose texels are as large as the gap between taps, so each
    // tap is the AVERAGE of the area it stands for. Without this the 7x7 tap grid is
    // ~15px apart on a 4K panel and the glow steps visibly around glyph edges.
    float gapPx = max(stp.x * float(sz.x), stp.y * float(sz.y));
    float lod = max(log2(gapPx), 0.0);

    // sigma = R/2 truncates the gaussian at 11% weight -> a hard rim on the glow.
    // R/1.6 lands the last tap near 2 sigma-and-a-half: a soft, complete falloff.
    const float sigma = float(R) / 1.6;
    vec4 acc = vec4(0.0);
    float wsum = 0.0;
    for (int y = -R; y <= R; ++y) {
        for (int x = -R; x <= R; ++x) {
            float w = exp(-float(x * x + y * y) / (2.0 * sigma * sigma));
            vec4 s = textureLod(panel, uv + vec2(float(x), float(y)) * stp, lod);
            acc.rgb += s.rgb * s.a * w; // premultiplied
            acc.a += s.a * w;
            wsum += w;
        }
    }
    return acc / wsum;
}

void main()
{
    vec4 pan = texture(panel, v_uv);
    vec3 src = pan.rgb;
    vec3 bandc = vec3(1.0);
    if (u.rainbowOn > 0.5) {
        float t = dot(v_uv, vec2(u.bandFreqX, u.bandFreqY)) + u.phase;
        bandc = bandColor(t);
        src = pan.rgb * bandc; // white mask -> band; dark fill/shadow stay dark
    }
    vec3 c = (u.sdr > 0.5) ? src : srgbToLinear(src) * u.scale;
    if (u.rainbowOn > 0.5 && u.sdr < 0.5)
        c *= u.brightness; // HDR only: on SDR a >1 multiply just clips channels -> desaturates to white

    float outA = pan.a;
    if (u.bloomStrength > 0.0) {
        vec4 bs = bloomSample(v_uv);
        // Glow colour: the band (rainbow) or the blurred text colour (otherwise).
        vec3 glowCol = (u.rainbowOn > 0.5) ? bandc * bs.a : bs.rgb;
        vec3 glow = (u.sdr > 0.5) ? glowCol : srgbToLinear(glowCol) * u.scale;
        if (u.rainbowOn > 0.5 && u.sdr < 0.5)
            glow *= u.brightness;
        if (u.sdr > 0.5) {
            // SDR has no headroom: adding the glow drives channels past 1.0 and the
            // hard clip takes ALL THREE to 1.0 -> coloured text turns flat white.
            // Screen blend approaches 1.0 asymptotically instead, so the glow still
            // brightens but the hue survives. HDR keeps the additive glow (it has the
            // headroom, and that overbright bloom is the point there).
            c = vec3(1.0) - (vec3(1.0) - c) * (vec3(1.0) - clamp(glow * u.bloomStrength, 0.0, 1.0));
        } else {
            c += glow * u.bloomStrength;
        }
        outA = max(pan.a, clamp(bs.a * u.bloomStrength, 0.0, 1.0)); // make the halo visible over the bg
    }
    fragColor = vec4(c, outA * u.fadeAlpha);
}
