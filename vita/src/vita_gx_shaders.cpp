#include "vita_gx_shaders.h"
#include "gl/pc_tev_shader.h"
#include "gl/vita_vs_key.h"
#include "vita_gxp_shaders.h"
#include "vita_platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <psp2/io/fcntl.h>

// Program building

GLuint vita_gx_load_gxp(GLenum type, const unsigned char* gxp, unsigned int size) {
    const unsigned int PREFIX = 5;
    unsigned int total = PREFIX + size;
    unsigned char* buf = (unsigned char*)malloc(total);
    if (!buf) return 0;
    memset(buf, 0, PREFIX);
    memcpy(buf + PREFIX, gxp, size);

    GLuint shader = glCreateShader(type);
    glShaderBinary(1, &shader, 0, buf, (GLsizei)total);
    free(buf);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        printf("[VITA GX] glShaderBinary failed (0x%04X, size=%u)\n", (unsigned)err, size);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint vita_gx_vertex_shader(void) {
    static GLuint vs = 0;
    if (vs == 0) {
        vs = vita_gx_load_gxp(GL_VERTEX_SHADER, gxp_vertex, gxp_vertex_size);
    }
    return vs;
}

GLuint vita_gx_vertex_shader_single(void) {
    static GLuint vs = 0;
    if (vs == 0) {
        vs = vita_gx_load_gxp(GL_VERTEX_SHADER, gxp_vertex_single, gxp_vertex_single_size);
    }
    return vs;
}

// Config lookup

int vita_gx_config_count(void) { return NECTAR_GXP_CONFIG_COUNT; }

uint64_t vita_gx_config_hash(int index) {
#if NECTAR_GXP_CONFIG_COUNT > 0
    if (index >= 0 && index < NECTAR_GXP_CONFIG_COUNT) return nectar_gxp_configs[index].hash;
#endif
    (void)index; return 0;
}

GLuint vita_gx_config_fragment(int index) {
#if NECTAR_GXP_CONFIG_COUNT > 0
    if (index >= 0 && index < NECTAR_GXP_CONFIG_COUNT)
        return vita_gx_load_gxp(GL_FRAGMENT_SHADER, nectar_gxp_configs[index].gxp,
                                nectar_gxp_configs[index].size);
#endif
    (void)index; return 0;
}

bool vita_gx_key_has_alpha_test(const PcTevShaderKey& key) {
    return pc_tev_alpha_test_always_passes(key) == false;
}

GLuint vita_gx_fallback_fragment(bool alphaTest) {
    static GLuint fbNoat = 0, fbAt = 0;
    if (alphaTest) {
        if (!fbAt) fbAt = vita_gx_load_gxp(GL_FRAGMENT_SHADER, gxp_fallback_at, gxp_fallback_at_size);
        return fbAt;
    }
    if (!fbNoat) fbNoat = vita_gx_load_gxp(GL_FRAGMENT_SHADER, gxp_fallback_noat, gxp_fallback_noat_size);
    return fbNoat;
}

// Capture

void vita_gx_capture_key(const PcTevShaderKey& key, uint64_t hash) {
    static bool enabled = getenv("NECTAR_NO_CAPTURE") == nullptr;
    if (!enabled) return;

    static uint64_t lastHash = 0;
    static bool lastValid = false;
    if (lastValid && hash == lastHash) return;
    lastHash = hash; lastValid = true;

    static std::unordered_map<uint64_t, bool> seen;
    if (seen.count(hash)) return;
    seen[hash] = true;

    // Append: [u64 hash][u16 keyBytes][key]
    static SceUID fd = -1;
    if (fd < 0) {
        fd = sceIoOpen(NECTAR_VITA_DATA_DIR "/tev_keys.bin",
                       SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
        if (fd < 0) return;
    }
    const uint16_t n = (uint16_t)sizeof(PcTevShaderKey);
    unsigned char rec[sizeof(hash) + sizeof(n) + sizeof(PcTevShaderKey)];
    memcpy(rec, &hash, sizeof(hash));
    memcpy(rec + sizeof(hash), &n, sizeof(n));
    memcpy(rec + sizeof(hash) + sizeof(n), &key, n);
    sceIoWrite(fd, rec, sizeof(rec));
}

// Uniform name mapping

GLint vita_gx_uniform_location(GLuint program, const char* name) {
    const char* bracket = strchr(name, '[');
    if (!bracket) return glGetUniformLocation(program, name);

    char flat[96];
    size_t base = (size_t)(bracket - name);
    if (base >= sizeof(flat) - 8) return glGetUniformLocation(program, name);
    memcpy(flat, name, base);
    int idx = atoi(bracket + 1);
    snprintf(flat + base, sizeof(flat) - base, "_%d", idx);
    return glGetUniformLocation(program, flat);
}

int vita_gx_vs_count(void) { return NECTAR_GXP_VS_COUNT; }

uint64_t vita_gx_vs_hash(int index) {
#if NECTAR_GXP_VS_COUNT > 0
    if (index >= 0 && index < NECTAR_GXP_VS_COUNT) return nectar_gxp_vs[index].hash;
#endif
    (void)index; return 0;
}

GLuint vita_gx_vs_shader(int index) {
#if NECTAR_GXP_VS_COUNT > 0
    if (index >= 0 && index < NECTAR_GXP_VS_COUNT)
        return vita_gx_load_gxp(GL_VERTEX_SHADER, nectar_gxp_vs[index].gxp, nectar_gxp_vs[index].size);
#endif
    (void)index; return 0;
}

int vita_gx_pair_count(void) { return NECTAR_GXP_PAIR_COUNT; }

void vita_gx_pair(int index, uint64_t* tevHash, uint64_t* vsHash, uint64_t* vsProgram) {
    *tevHash = 0; *vsHash = 0; *vsProgram = 0;
#if NECTAR_GXP_PAIR_COUNT > 0
    if (index >= 0 && index < NECTAR_GXP_PAIR_COUNT) {
        *tevHash = nectar_gxp_pairs[index].tev;
        *vsHash = nectar_gxp_pairs[index].vs;
        *vsProgram = nectar_gxp_pairs[index].prog;
    }
#else
    (void)index;
#endif
}

void vita_gx_capture_vs_pair(uint64_t tevHash, const VitaVsKey& key) {
    static bool enabled = getenv("NECTAR_NO_CAPTURE") == nullptr;
    if (!enabled) return;
    const uint64_t vsHash = vita_vs_hash_key(key);
    const uint64_t pairKey = tevHash ^ (vsHash * 0x9E3779B97F4A7C15ull) ^ (vsHash >> 7);
    static uint64_t lastPair = 0;
    static bool lastValid = false;
    if (lastValid && pairKey == lastPair) return;
    lastPair = pairKey; lastValid = true;
    static std::unordered_set<uint64_t> seen;
    if (!seen.insert(pairKey).second) return;
    static SceUID fd = -1;
    if (fd < 0) {
        fd = sceIoOpen(NECTAR_VITA_DATA_DIR "/vs_keys.bin",
                       SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
        if (fd < 0) return;
    }
    unsigned char rec[8 + 8 + 2 + sizeof(VitaVsKey)];
    const uint16_t n = (uint16_t)sizeof(VitaVsKey);
    memcpy(rec, &tevHash, 8);
    memcpy(rec + 8, &vsHash, 8);
    memcpy(rec + 16, &n, 2);
    memcpy(rec + 18, &key, sizeof(VitaVsKey));
    sceIoWrite(fd, rec, sizeof(rec));
}
