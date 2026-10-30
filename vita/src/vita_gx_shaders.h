#pragma once
// PS Vita GXP shader loading helpers for pc_gfx.cpp.
#include <cstdint>
#include <vitaGL.h>

struct PcTevShaderKey;

// Loads one precompiled GXP as a shader object. Returns 0 on failure.
GLuint vita_gx_load_gxp(GLenum type, const unsigned char* gxp, unsigned int size);

// The shared vertex shader object (gxp_vertex), created once.
GLuint vita_gx_vertex_shader(void);

// Shared vertex shader with a one-slot matrix palette.
GLuint vita_gx_vertex_shader_single(void);

// Baked per-config fragment table (from vita_gxp_shaders.h).
int      vita_gx_config_count(void);
uint64_t vita_gx_config_hash(int index);
GLuint   vita_gx_config_fragment(int index);          // shader object for config `index`

// Fallback fragment shader object (alpha-test-aware), for configs not baked.
GLuint vita_gx_fallback_fragment(bool alphaTest);

bool vita_gx_key_has_alpha_test(const PcTevShaderKey& key);

// Records a unique TEV key to ux0:data/Pikmin/tev_keys.bin.
void vita_gx_capture_key(const PcTevShaderKey& key, uint64_t hash);

// glGetUniformLocation with "base[i]" mapped to the Cg flat name "base_i".
GLint vita_gx_uniform_location(GLuint program, const char* name);

// Baked vertex-shader variants and (TEV hash, VS hash) pairs.
struct VitaVsKey;
int      vita_gx_vs_count(void);
uint64_t vita_gx_vs_hash(int index);
GLuint   vita_gx_vs_shader(int index);           // new shader object for that variant
int      vita_gx_pair_count(void);
void     vita_gx_pair(int index, uint64_t* tevHash, uint64_t* vsHash, uint64_t* vsProgram);   // vsProgram: variant to link
void     vita_gx_capture_vs_pair(uint64_t tevHash, const VitaVsKey& key);   // writes ux0:data/Pikmin/vs_keys.bin
