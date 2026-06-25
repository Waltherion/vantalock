#pragma once

#include <cstdint>
#include <vector>

class QString;

// A decoded HDR image: tightly packed RGBA half-float (fp16) pixels, linear,
// using libultrahdr's convention where 1.0 corresponds to 203 cd/m^2.
struct HdrImage {
    // How the source encoded its tone (for display + the info overlay).
    enum class HdrKind : unsigned char { Sdr, Pq, Hlg, GainMap, LinearHdr };
    // Colour primaries the pixel data is stored in (native; converted at display/encode).
    enum class Primaries : unsigned char { Bt709, Bt2020, DisplayP3 };

    int w = 0;
    int h = 0;
    std::vector<uint16_t> rgba16f; // w*h*4 halfs (raw fp16 bits)
    bool hdr = false;              // true HDR content (PQ/HLG/gain-map) vs SDR
    HdrKind kind = HdrKind::Sdr;   // specific transfer/source kind
    Primaries primaries = Primaries::Bt709; // native colour primaries

    bool valid() const { return w > 0 && h > 0 && rgba16f.size() == size_t(w) * h * 4; }
};

// Human-readable names for the info overlay.
const char *hdrKindName(HdrImage::HdrKind kind);
const char *primariesName(HdrImage::Primaries p);

// Decode an UltraHDR (gain-map) JPEG via libultrahdr. Returns an invalid HdrImage
// on failure (diagnostics go to stderr). This is the path mpv-based wallpapers
// lack: it understands the HDR gain-map metadata baked into the JPEG.
HdrImage decodeUltraHdr(const QString &path);

// Decode an AVIF via libavif, linearising PQ/HLG/sRGB and converting BT.2020
// primaries to BT.709, to the same fp16 linear (1.0 = 203 nits) convention.
// AVIF carries true HDR pixels directly -- the right format for graphic/drawn
// content, with none of the gain-map inverse-tonemapping banding.
// maxDim > 0 caps the longest side (downscaled before linearisation); 0 = full res.
HdrImage decodeAvif(const QString &path, int maxDim = 0);

// Decode a JPEG-XL via libjxl (HDR PQ/HLG or SDR) into the same fp16 pipeline.
HdrImage decodeJxl(const QString &path);

// Decode a HEIC/HEIF via libheif (HDR PQ/HLG, 10/12-bit, or SDR).
HdrImage decodeHeic(const QString &path);

// Pick the decoder by file extension (.avif, .jxl, .jpg -> HDR-aware; else QImage).
// maxDim > 0 caps the longest side where the decoder supports it (currently AVIF,
// which is the only format that can carry 45 MP); 0 = full resolution. A viewer
// decodes the active image at full res and can prefetch neighbours capped.
HdrImage decodeImage(const QString &path, int maxDim = 0);
