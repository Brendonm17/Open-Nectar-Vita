#pragma once
// PS Vita vertex-shader specialisation key. Fixed 16 bytes, hashed raw.
#include <stdint.h>
#include <string.h>

enum : uint8_t { kVitaVsKeyVersion = 1 };

// Texgen mode per output slot 0..3 (the four texcoord varyings the fragment reads).
//   0        this slot's own attribute (aTexCoord<slot>), no matrix
//   1        position (worldPos) * uTcMtx_<slot>
//   2        normal (N, 1) * uTcMtx_<slot>
//   3..10    attribute texcoord (src - GX_TG_TEX0) * uTcMtx_<slot>
//   11..14   an earlier generated slot (src - GX_TG_TEXCOORD0) * uTcMtx_<slot>
//   20..27   attribute texcoord whose matrix is the IDENTITY at draw time: no multiply
struct VitaVsKey {
    uint8_t version = kVitaVsKeyVersion;
    uint8_t paletteSlots = 1;   // declared uPosPal rows / 3: 1, 3, 6 or 10 (dynamic index only when > 1)
    uint8_t chan0En = 0;        // channel 0 lit; otherwise vLit0 carries the -1 "disabled" sentinel
    uint8_t numLights0 = 0;     // 0..4, unrolled
    uint8_t attn0 = 0;          // distance attenuation on channel 0 lights
    uint8_t chan1En = 0;
    uint8_t chan1Mode = 0;      // 0 diffuse lights (numLights1 > 0), 1 specular half-vector, 2 ambient only
    uint8_t numLights1 = 0;     // 0..4, mode 0 only
    uint8_t attn1 = 0;          // mode 0 only
    uint8_t tcMode[4] = { 0, 0, 0, 0 };
    uint8_t pad[3] = { 0, 0, 0 };
};
static_assert(sizeof(VitaVsKey) == 16, "VitaVsKey is hashed as raw bytes: keep it padding-free");

static inline uint64_t vita_vs_hash_key(const VitaVsKey& k) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&k);
    for (size_t i = 0; i < sizeof(k); ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static inline bool vita_vs_key_needs_normal(const VitaVsKey& k) {
    if (k.chan0En && k.numLights0 > 0) return true;
    if (k.chan1En && (k.chan1Mode == 1 || (k.chan1Mode == 0 && k.numLights1 > 0))) return true;
    for (int i = 0; i < 4; ++i) if (k.tcMode[i] == 2) return true;
    return false;
}
