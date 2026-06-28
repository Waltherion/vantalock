#version 440

// VantaLock — present one HDR/SDR image onto a lock surface.
// Adapted from vantaviewer's image.frag (same UBO + colour pipeline) so the
// proven HDR path is reused verbatim. window-uv (v_uv, 0..1 across the output)
// maps to image-uv:
//     iuv = (v_uv - 0.5) * uvScale + 0.5 + uvOffset
// Out-of-range iuv is letterbox -> the configured background (true black on HDR).
//
// Output depends on the surface mode (u.sdr):
//   HDR: linear scRGB * scale (203/80) for the Windows-scRGB surface.
//   SDR: HDR images tone-mapped (Reinhard), SDR rolled off, then sRGB-encoded.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform U {
    float scale;     // 203/80 for the Windows-scRGB surface
    float sdr;       // >0.5 -> SDR output (tone-map + sRGB encode)
    float imageHdr;  // >0.5 -> the image is true HDR (needs range compression on SDR)
    float rot;       // rotation quadrant: 0/1/2/3 == 0/90/180/270 clockwise
    vec2 uvScale;    // window-uv -> display-uv scale (fit/zoom)
    vec2 uvOffset;   // window-uv -> display-uv offset (pan)
    float primaries; // 0 = BT.709, 1 = BT.2020, 2 = Display-P3 (convert to BT.709)
    float exposure;  // linear multiplier (2^EV); 1.0 = no change
    float dim;       // dim multiplier in linear (1.0 = none); for lock dimming
    float blur;      // gaussian blur radius in texture-uv units (0 = sharp)
    vec4 bgColor;    // letterbox background: linear rgb + alpha
    vec4 rounding;   // xy = corner radius in window-uv (per axis), z>0.5 = enabled
} u;

layout(binding = 1) uniform sampler2D tex;

// Background blur. The lock screen is rendered infrequently, so tap count is cheap.
// The radius is in texture-uv units; the *style* is carried in u.rounding.w (free on
// the background, where corner rounding is disabled):
//   0 frosted  - sparse-tap gaussian (the original "ridged glass" look), R=6
//   1 gaussian - dense-tap gaussian (smooth), R=10
//   2 box      - equal-weight taps (harder blur), R=6
//   3 pixelate - snap to a coarse grid sized by the radius
//   4 none     - sharp
vec3 sampleBlurred(vec2 uv)
{
    if (u.blur <= 0.0)
        return texture(tex, uv).rgb;
    int bt = int(u.rounding.w + 0.5);

    if (bt == 4)
        return texture(tex, uv).rgb;

    if (bt == 3) { // pixelate
        vec2 cells = max(vec2(2.0), vec2(1.0) / vec2(u.blur));
        return texture(tex, (floor(uv * cells) + 0.5) / cells).rgb;
    }

    if (bt == 1) { // dense gaussian (smooth)
        const int R = 10;
        const float sigma = float(R) * 0.5;
        const float step = u.blur / float(R);
        vec3 sum = vec3(0.0);
        float wsum = 0.0;
        for (int y = -R; y <= R; ++y) {
            for (int x = -R; x <= R; ++x) {
                float w = exp(-float(x * x + y * y) / (2.0 * sigma * sigma));
                sum += texture(tex, uv + vec2(float(x), float(y)) * step).rgb * w;
                wsum += w;
            }
        }
        return sum / wsum;
    }

    // frosted (0, default - original look) or box (2): sparse R=6
    const int R = 6;
    const float sigma = float(R) * 0.5;
    const float step = u.blur / float(R);
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int y = -R; y <= R; ++y) {
        for (int x = -R; x <= R; ++x) {
            float w = (bt == 2) ? 1.0 : exp(-float(x * x + y * y) / (2.0 * sigma * sigma));
            sum += texture(tex, uv + vec2(float(x), float(y)) * step).rgb * w;
            wsum += w;
        }
    }
    return sum / wsum;
}

// Linear BT.2020 -> linear BT.709 primaries.
vec3 bt2020ToBt709(vec3 c)
{
    return vec3(
         1.660491 * c.r - 0.587641 * c.g - 0.072850 * c.b,
        -0.124550 * c.r + 1.132900 * c.g - 0.008349 * c.b,
        -0.018151 * c.r - 0.100579 * c.g + 1.118730 * c.b);
}

// Linear Display-P3 (D65) -> linear BT.709 primaries.
vec3 p3ToBt709(vec3 c)
{
    return vec3(
         1.224940 * c.r - 0.224940 * c.g + 0.000000 * c.b,
        -0.042057 * c.r + 1.042057 * c.g + 0.000000 * c.b,
        -0.019638 * c.r - 0.078636 * c.g + 1.098274 * c.b);
}

// Soft highlight roll-off: linear below the knee, asymptotes to 1.0 above it.
float rolloff(float v)
{
    const float k = 0.8;
    return v <= k ? v : k + (1.0 - k) * (1.0 - exp(-(v - k) / (1.0 - k)));
}

vec3 srgbEncode(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3(0.0031308), c));
}

void main()
{
    // Rounded corners (thumbnail): discard fragments past the corner radius so the
    // already-drawn background shows through. Radii are per-axis in window-uv.
    if (u.rounding.z > 0.5) {
        vec2 q = abs(v_uv - 0.5) - (vec2(0.5) - u.rounding.xy);
        if (q.x > 0.0 && q.y > 0.0) {
            vec2 d = q / max(u.rounding.xy, vec2(1e-5));
            if (dot(d, d) > 1.0)
                discard;
        }
    }

    vec2 duv = (v_uv - 0.5) * u.uvScale + 0.5 + u.uvOffset;

    if (duv.x < 0.0 || duv.x > 1.0 || duv.y < 0.0 || duv.y > 1.0) {
        if (u.sdr > 0.5) fragColor = vec4(srgbEncode(u.bgColor.rgb), u.bgColor.a);
        else             fragColor = vec4(u.bgColor.rgb * u.scale, u.bgColor.a);
        return;
    }

    int rot = int(u.rot + 0.5);
    vec2 iuv;
    if (rot == 1)      iuv = vec2(duv.y, 1.0 - duv.x);
    else if (rot == 2) iuv = vec2(1.0 - duv.x, 1.0 - duv.y);
    else if (rot == 3) iuv = vec2(1.0 - duv.y, duv.x);
    else               iuv = duv;

    vec3 color = sampleBlurred(iuv);

    if (u.primaries > 1.5)
        color = max(p3ToBt709(color), vec3(0.0));
    else if (u.primaries > 0.5)
        color = max(bt2020ToBt709(color), vec3(0.0));

    color *= u.exposure;
    color *= u.dim; // lock-screen dimming in linear light

    if (u.sdr > 0.5) {
        if (u.imageHdr > 0.5) {
            float L = dot(color, vec3(0.2126, 0.7152, 0.0722));
            const float Lw = 6.0;
            float Lt = L * (1.0 + L / (Lw * Lw)) / (1.0 + L);
            color *= (L > 1e-4) ? (Lt / L) : 1.0;
        } else {
            color = vec3(rolloff(color.r), rolloff(color.g), rolloff(color.b));
        }
        fragColor = vec4(srgbEncode(color), 1.0);
    } else {
        fragColor = vec4(color * u.scale, 1.0);
    }
}
