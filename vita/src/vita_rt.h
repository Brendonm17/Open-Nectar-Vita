#pragma once
// PS Vita render thread: replays the recorded GL/GXM command stream into vitaGL.
#include <stdint.h>
#include <stddef.h>
#include <vitaGL.h>

enum VitaRtOp : uint16_t {
    RT_NOP = 0,
    RT_USE_PROGRAM,          // u32 prog
    RT_UNIFORM_1I,           // i32 loc, i32 v
    RT_UNIFORM_1F,           // i32 loc, f32 v
    RT_UNIFORM_2I,           // i32 loc, i32 x, y
    RT_UNIFORM_4I,           // i32 loc, i32 x, y, z, w
    RT_UNIFORM_4F,           // i32 loc, f32 x, y, z, w
    RT_UNIFORM_4FV,          // i32 loc, i32 count, f32[count*4]
    RT_UNIFORM_MAT4,         // i32 loc, i32 count, u8 transpose, f32[16*count]
    RT_UNIFORM_MAT3,         // i32 loc, i32 count, u8 transpose, f32[9*count]
    RT_UNIFORM_IMAGE,        // u32 prog, u32 stage, u32 offset, u32 bytes, u8[bytes]
    RT_ACTIVE_TEXTURE,       // u32 unit
    RT_BIND_TEXTURE,         // u32 target, u32 id
    RT_TEX_PARAMETER_I,      // u32 target, u32 pname, i32 param
    RT_TEX_IMAGE_2D,         // u32 target, i32 level, i32 internal, i32 w, i32 h, u32 fmt, u32 type, u32 bytes, u8[bytes]
    RT_COMPRESSED_TEX_IMAGE_2D, // u32 target, i32 level, i32 internal, i32 w, i32 h, u32 bytes, u8[bytes]
    RT_GENERATE_MIPMAP,      // u32 target
    RT_TEX_SUB_IMAGE_2D,     // u32 target, i32 level, i32 x, i32 y, i32 w, i32 h, u32 fmt, u32 type, u32 bytes, u8[bytes]
    RT_DELETE_TEXTURE,       // u32 id
    RT_BIND_BUFFER,          // u32 target, u32 id
    RT_BUFFER_DATA,          // u32 target, u32 bytes, u32 usage, u8[bytes]
    RT_BUFFER_DATA_MAP,      // u32 target, u32 bytes, u32 usage, u32 bufId, u8[bytes]
    RT_BUFFER_SUB_DATA,      // u32 target, u32 offset, u32 bytes, u8[bytes]
    RT_DELETE_BUFFER,        // u32 id
    RT_VERTEX_ATTRIB_POINTER,// u32 index, i32 size, u32 type, u8 normalized, i32 stride, u32 offset
    RT_ENABLE,               // u32 cap
    RT_DISABLE,              // u32 cap
    RT_BLEND_FUNC,           // u32 src, u32 dst
    RT_BLEND_EQUATION,       // u32 mode
    RT_DEPTH_FUNC,           // u32 func
    RT_DEPTH_MASK,           // u8 flag
    RT_COLOR_MASK,           // u8 r,g,b,a
    RT_CULL_FACE,            // u32 mode
    RT_FRONT_FACE,           // u32 mode
    RT_VIEWPORT,             // i32 x,y,w,h
    RT_SCISSOR,              // i32 x,y,w,h
    RT_CLEAR_COLOR,          // f32 r,g,b,a
    RT_CLEAR_DEPTH,          // f32 d
    RT_CLEAR,                // u32 mask
    RT_DRAW_ARRAYS,          // u32 mode, i32 first, i32 count
    RT_DRAW_ELEMENTS,        // u32 mode, i32 count, u32 type, u32 offset
    RT_DRAW_CLIENT,          // u32 mode, i32 count, u32 bytes, u8[bytes]
    RT_GXM_VERTEX_STREAM,    // u32 index, u32 bufId, u32 offset
    RT_GXM_VERTEX_STREAM_ABS,// u32 index, const void* ptr
    RT_GXM_FRAGMENT_TEXTURE, // u32 unit, u32 glTexId
    RT_FAST_MODE,            // i32 enable
    RT_FAST_DRAW_INDEXED,    // u32 vbo, u32 ibo, i32 count, i32 firstIndex
    RT_FAST_DRAW_ARRAYS,     // u32 vbo, ptr attrBase, ptr gpuBase, u32 mode, i32 count
    RT_PRESENT,              // no payload
    RT_INIT_GFX,
    RT_SYNC,
    RT_GX_APPLY,             // VgxApplyRec + mirror blocks
    RT_GX_PALETTE,           // VgxPaletteRec + matrix blocks
    RT_GX_SYNC_BACK,
    RT_LINE_WIDTH,           // f32 width
    RT_EFB_COPY,             // u32 tex, i32 w, i32 h, i32 program, i32 depth, f32 u0, v0, u1, v1, i32 box
    RT_EFB_TARGET,           // u32 tex (0 = display), i32 w, i32 h
    RT_TEX_MIPCHAIN,         // u32 tex, i32 w, i32 h, i32 levels, u8[] RGBA8 levels
    RT_EFB_COMPOSITE,        // i32 kind, u32 tex0, u32 tex1, f32 p0, f32 p1
    RT_EFB_VFILTER,          // f32 row step, f32 edge, f32 centre
    RT_OP_COUNT              // op count
};

// RT_EFB_COPY programs.
enum { RT_EFB_PROG_RGBA = 0, RT_EFB_PROG_565 = 1, RT_EFB_PROG_I8 = 2, RT_EFB_PROG_Z8 = 3, RT_EFB_PROG_VFILTER = 4 };
// RT_EFB_COMPOSITE kinds.
enum { RT_EFB_COMP_BLUR = 0, RT_EFB_COMP_GREY = 1 };

// Game-thread API.
bool  vita_rt_start(void);
void  vita_rt_run_init_gfx(void);
void  vita_rt_begin_recording(void);
bool  vita_rt_recording(void);
void* vita_rt_emit(VitaRtOp op, size_t payloadBytes);   // payload pointer, or NULL
void  vita_rt_present(void);
void  vita_rt_sync(void);
GLuint vita_rt_alloc_texture_id(void);
GLuint vita_rt_alloc_buffer_id(void);
unsigned vita_rt_vram_free_kb(void);
void vita_rt_set_attr_layout(const uint32_t offsets[8], const int sizes[8], int stride);
void vita_rt_release_texture_id(GLuint id);
void vita_rt_release_buffer_id(GLuint id);

// Render-thread hooks.
void pc_gfx_init_impl(void);
void vita_rt_replay_draw_client(unsigned mode, int count, const void* verts, unsigned bytes);
// Translator uniform image of a program stage, or NULL.
uint8_t* vita_gx_xlat_image(GLuint prog, int stage, uint32_t* size);

// vitaGL uniform shadow of one stage, or NULL.
uint8_t* vita_rt_shadow(uint32_t prog, uint32_t stage, uint32_t* size);

// rtunif: uniform sections on the render thread.
struct ProgramLocations;
// Registers a program's location tables.
bool vita_rtgx_register(GLuint prog, const ProgramLocations* loc, const uint32_t imgSize[2]);
bool vita_rtgx_registry_ok(void);
// Enables rtunif.
bool vita_rtgx_configure(bool on, bool rawCacheOff);
bool vita_rtgx_loc_is(GLuint prog, const ProgramLocations* loc);
// Whether the render thread lost a record.
bool vita_rtgx_failed(void);
void vita_rtgx_apply(const uint8_t* q, uint32_t bytes);
void vita_rtgx_palette(const uint8_t* q, uint32_t bytes);
void vita_rtgx_sync_back(void);
// Notes replay ops lost to a broken ring.
void vita_rtgx_note_ring_damage(void);
