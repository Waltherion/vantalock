#version 440

// Blit an sRGB-authored UI panel (QPainter -> RGBA8) onto the lock surface.
//   HDR surface: linearise sRGB and scale to ~203 nits so white UI reads correctly.
//   SDR surface: pass the sRGB bytes straight through.
// Alpha is preserved for the alpha-blend in the pipeline.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

// Same 16-float UBO as the present pipeline; only scale + sdr are used here.
layout(std140, binding = 0) uniform U {
    float scale;
    float sdr;
    float _pad0;
    float _pad1;
    vec4 _rest[3];
} u;

layout(binding = 1) uniform sampler2D panel;

vec3 srgbToLinear(vec3 c)
{
    vec3 lo = c / 12.92;
    vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(lo, hi, step(vec3(0.04045), c));
}

void main()
{
    vec4 t = texture(panel, v_uv);
    vec3 c = (u.sdr > 0.5) ? t.rgb : srgbToLinear(t.rgb) * u.scale;
    fragColor = vec4(c, t.a);
}
