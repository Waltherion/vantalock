#version 440

// Blit an sRGB-authored UI panel (QPainter -> RGBA8) onto the lock surface.
//   HDR surface: linearise sRGB and scale to ~203 nits so white UI reads correctly.
//   SDR surface: pass the sRGB bytes straight through.
// Alpha is preserved for the alpha-blend in the pipeline.
//
// Optional rainbow: when rainbowOn, the panel's rainbow-able elements are drawn
// WHITE on the CPU side, and a rolling 45-degree band (built from the stops) is
// multiplied in here -- white -> band colour, dark fill/shadow stay dark. Scrolling
// is just `phase` advancing, so no CPU re-render or texture re-upload per frame.

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
    float bloomStrength; // glow amount (added in a later step; 0 = off)
    float brightness;    // band luminance multiplier (>1 = HDR pop)
    float _p1;
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

void main()
{
    vec4 pan = texture(panel, v_uv);
    vec3 src = pan.rgb;
    if (u.rainbowOn > 0.5) {
        float t = dot(v_uv, vec2(u.bandFreqX, u.bandFreqY)) + u.phase;
        src = pan.rgb * bandColor(t); // white mask -> band; dark fill/shadow stay dark
    }
    vec3 c = (u.sdr > 0.5) ? src : srgbToLinear(src) * u.scale;
    if (u.rainbowOn > 0.5)
        c *= u.brightness; // push the band into HDR brightness so vivid colours pop
    fragColor = vec4(c, pan.a);
}
