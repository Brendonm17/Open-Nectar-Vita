#pragma once
// PS Vita: Cg equivalents of the port's GLSL shaders.
// Array uniforms are flat name_i.
#include <string>

struct PcTevShaderKey;

const char* vita_cg_vertex_source(void);
const char* vita_cg_fallback_fragment_source(void);
std::string vita_cg_fragment_source(const PcTevShaderKey& key);
// softAlpha: alpha-test failures are zeroed instead of discarded.
std::string vita_cg_fragment_source(const PcTevShaderKey& key, bool softAlpha);
bool vita_tev_has_alpha_test(const PcTevShaderKey& key);
constexpr unsigned long long kVitaSoftAlphaTag = 0x5AF7A1FA5AF7A1FAull;   // must match pc_gfx.cpp
// Elided variant: no alpha test.
constexpr unsigned long long kVitaElideAlphaTag = 0xE11DEA1FA0E11DEAull;   // must match pc_gfx.cpp

// Vertex-shader specialisation (vita_vs_cg.cpp): Cg for one VitaVsKey.
struct VitaVsKey;
std::string vita_cg_vertex_source_for(const VitaVsKey& key);

// Varyings a TEV config's fragment program reads (bits 0-3 vTexCoord0-3,
// bit 4 vColor1, bits 5-7 material sources).
unsigned vita_tev_needed_texcoords(const PcTevShaderKey& key);
bool vita_tev_uses_channel1(const PcTevShaderKey& key);
std::string vita_cg_vertex_source_for(const VitaVsKey& key, unsigned needMask);
