/* PS Vita: PFNGL*PROC typedefs for pc_gfx.cpp's function-pointer loader. */
#ifndef NECTAR_VITA_GLEXT_H
#define NECTAR_VITA_GLEXT_H
#include "gl.h"
#ifdef __cplusplus
#define NECTAR_PFN(NAME, FN) typedef decltype(&FN) NAME;
NECTAR_PFN(PFNGLACTIVETEXTUREPROC, glActiveTexture)
/* PFNGLATTACHSHADERPROC: declared in pc_gfx.cpp */
NECTAR_PFN(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation)
/* PFNGLBINDBUFFERPROC: declared in pc_gfx.cpp */
NECTAR_PFN(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)
NECTAR_PFN(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer)
NECTAR_PFN(PFNGLBLENDEQUATIONPROC, glBlendEquation)
NECTAR_PFN(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer)
/* PFNGLBUFFERDATAPROC: declared in pc_gfx.cpp */
/* PFNGLBUFFERSUBDATAPROC: declared in pc_gfx.cpp */
NECTAR_PFN(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)
/* PFNGLCOMPILESHADERPROC: declared in pc_gfx.cpp */
/* PFNGLCREATEPROGRAMPROC: declared in pc_gfx.cpp */
/* PFNGLCREATESHADERPROC: declared in pc_gfx.cpp */
NECTAR_PFN(PFNGLDELETEPROGRAMPROC, glDeleteProgram)
NECTAR_PFN(PFNGLDELETESHADERPROC, glDeleteShader)
/* PFNGLENABLEVERTEXATTRIBARRAYPROC: declared in pc_gfx.cpp */
NECTAR_PFN(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer)
NECTAR_PFN(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)
/* PFNGLGENBUFFERSPROC: declared in pc_gfx.cpp */
NECTAR_PFN(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)
NECTAR_PFN(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers)
NECTAR_PFN(PFNGLGETACTIVEUNIFORMPROC, glGetActiveUniform)
/* PFNGLGETATTRIBLOCATIONPROC: declared in pc_gfx.cpp */
NECTAR_PFN(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)
NECTAR_PFN(PFNGLGETPROGRAMIVPROC, glGetProgramiv)
NECTAR_PFN(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)
NECTAR_PFN(PFNGLGETSHADERIVPROC, glGetShaderiv)
/* PFNGLGETUNIFORMLOCATIONPROC: declared in pc_gfx.cpp */
/* PFNGLLINKPROGRAMPROC: declared in pc_gfx.cpp */
NECTAR_PFN(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage)
/* PFNGLSHADERSOURCEPROC: declared in pc_gfx.cpp */
/* PFNGLUNIFORM1FPROC: declared in pc_gfx.cpp */
/* PFNGLUNIFORM1IPROC: declared in pc_gfx.cpp */
/* PFNGLUNIFORM2IPROC: declared in pc_gfx.cpp */
/* PFNGLUNIFORM4FPROC: declared in pc_gfx.cpp */
/* PFNGLUNIFORM4IPROC: declared in pc_gfx.cpp */
/* PFNGLUNIFORMMATRIX3FVPROC: declared in pc_gfx.cpp */
/* PFNGLUNIFORMMATRIX4FVPROC: declared in pc_gfx.cpp */
/* PFNGLUSEPROGRAMPROC: declared in pc_gfx.cpp */
/* PFNGLVERTEXATTRIBPOINTERPROC: declared in pc_gfx.cpp */
#undef NECTAR_PFN
typedef void (*PFNGLGETUNIFORMIVPROC)(GLuint program, GLint location, GLint* params);
#endif /* __cplusplus */
#endif /* NECTAR_VITA_GLEXT_H */
