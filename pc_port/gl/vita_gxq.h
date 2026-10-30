#pragma once
// PS Vita GX-level command queue.
#include <stddef.h>
#include <stdint.h>

bool vita_gxq_start(void);              // spawns the translator and starts recording
bool vita_gxq_recording(void);
void vita_gxq_note_dl_register_write(void);          // translator: a display list wrote a BP/XF register

// S1: vertex-state images.
struct GxqVtxImage {
    uint32_t descMask;       // attributes with a descriptor set (bit = GXAttr)
    uint32_t desc[26];       // GXAttrType per attribute
    uint32_t fmtIdx;         // vertex format of fmt[]
    uint32_t fmtMask;        // attributes with a format set
    uint32_t fmt[26];        // gxq_vtx_fmt_pack(cnt, type, frac)
    uint16_t gens;           // GX-level calls in the run
};
static inline uint32_t gxq_vtx_fmt_pack(uint32_t cnt, uint32_t type, uint8_t frac) { return (cnt << 16) | (type << 8) | frac; }
// kGxqVtxApplied: done. kGxqVtxLegacy: not recording, caller makes the original calls.
enum { kGxqVtxLegacy = 0, kGxqVtxApplied = 1 };
// Image builders for the GX list setters.
struct _GXVtxDescList;
struct _GXVtxAttrFmtList;
void pc_gx_vtx_image_init(GxqVtxImage& img);
void pc_gx_vtx_image_add_fmt_list(GxqVtxImage& img, int fmt, const _GXVtxAttrFmtList* list);   // list not null
void pc_gx_vtx_image_add_desc_list(GxqVtxImage& img, const _GXVtxDescList* list);            // list not null
void pc_gx_set_vtx_attr_fmtv_calls(int fmt, const _GXVtxAttrFmtList* list);
void pc_gx_set_vtx_descv_calls(const _GXVtxDescList* list);

// S4: generation-on-emit.

#ifdef __cplusplus
extern "C" {
#endif
int  vita_gxq_pipe_depth(void);   // frames in flight per stage boundary
void vita_gxq_sync(void);
void vita_gxq_note_free(const void* ptr, size_t bytes);
#ifdef __cplusplus
}
#endif
