#pragma once
// PS Vita: DGXGraphics record-side state.
#if defined(__vita__)
#include "Graphics.h"
#include "PVW.h"
#include "vita_gxq_ctx.h"

class Material;
class Shape;

// Material block last sent.
struct PcStateBlock {
	unsigned gen;     // category generation when sent
	unsigned genEmit; // generation moved by sent ops
	u32 size;
	bool valid;
	// 8-aligned for word compares.
	alignas(8) u8 key[16 * sizeof(PVWTevStage) + 8];
};

// Pre-cull union hint.
struct PcUnionHint {
	const Joint::MatPoly* mMatPoly;
	const Matrix4f* mJoint;
	int mGroup;
	u8 mAllOff;  // the group's last per-list run found every list off screen
	u8 mBackoff; // visits left before the union is tried again after a miss
};
enum { kPcUnionHints = 1024 };

struct PcDgxCtx {
	GxqCtx* rec;              // GX recorder context
	bool volatileArrays;      // initMesh: this shape rewrote its positions / normals this frame
	int cullPrev, tevGroup;
	bool sendMtx, sendTx, sendCol, sendNbt, sendTxUV[8];
	PcStateBlock pe, kcol, tev, chan, tevReg;
	// drawSingleMatpoly material and descriptor caches.
	struct {
		bool held;
		unsigned pass;
		Material* mat;
		f32 intensity;
		int hasTexGen;
		bool envMap;
		unsigned gen;
	} matCache;
	struct {
		Shape* model;
		int flags;
		const void* scale;
		u32 gen;
		bool valid;
	} vtxCache;
	unsigned meshPass;        // shape draws (initMesh)
	GColor colors[1];         // setAmbient colour pair
	PcUnionHint unionHints[kPcUnionHints];
	// Draw statistics sinks.
	u32* materialCount;
	u32* lightingSets;
	int* polygonCount;
	u32* dispCount;
	u32* lightSetNum;
	s32* activeLightCount;
	u32 local[6];             // this context's own sinks
};

extern PcDgxCtx gPcDgxMain; // game thread
// Pikmin draw-record context (gGxqSub).
extern PcDgxCtx gPcDgxSub;

// Draw-record fork: copies the game thread's decision state into the segment.
void pcDgxForkCopy(const PcDgxCtx& from, PcDgxCtx& to);
// Draw-record fence: forgets the game thread's sent state.
void pcDgxFence(PcDgxCtx& d);
// Draw-record join: adds the segment's statistics into gsys.
void pcDgxRetire(PcDgxCtx& sub, PcDgxCtx& main);
#endif
