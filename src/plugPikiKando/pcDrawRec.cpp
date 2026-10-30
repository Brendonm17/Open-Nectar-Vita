// PS Vita drawrec: the second GX recorder and its record worker.
#if defined(__vita__)
#include "Camera.h"
#include "Graphics.h"
#include "Joint.h"
#include "Material.h"
#include "Matrix4f.h"
#include "Mesh.h"
#include "PVW.h"
#include "Shape.h"
#include "Texture.h"
#include "system.h"
#include "vita_drawrec.h"
#include "vita_gxq_ctx.h"
#include "../sysDolphin/pcDgxCtx.h"
#include "timing/pc_prof.h"
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <new>

#if defined(DEVELOP) || defined(VERSION_PIKIDEMO)
#error "drawrec needs gsys->mTimer's start/stop to be no-ops (DEVELOP / VERSION_PIKIDEMO make them live)"
#endif

extern "C" void vita_offlock_register(uintptr_t lo, uintptr_t hi);
extern const void* g_pcPlainTextureVtbl; // plain Texture vtable

// Shared state
u16 g_pcDrChain        = 0; // open chain id, 0 if none
int g_pcDrChainHoisted = 0; // open chain hoisted a cacheShape
int g_pcDrTekiRec      = 0; // teki lane publishing
// Stand-in for a dead local active matrix.
Matrix4f g_pcDrDeadMtx;

namespace {

// Configuration
const unsigned kDrLanesOn   = kDrBitPiki | kDrBitTeki | kDrBitTekiLight;
const int kDrPrio           = 0xB1;
const unsigned kDrSpinUs    = 300;    // worker spin before sleeping (us)
const float kDrMinBUs       = 350.0f; // min expected B (us) to fork
const int kDrRecMax         = 4096;   // records per chain
const int kDrMaxParts       = 6;      // parts per chain
const float kDrReforkUs     = 250.0f; // expected B (us) left to re-fork after a break
const int kDrWarmupFrames   = 30;     // armed frames before the first fork
enum { kDrPartSlots = 8 };

int sDrMode       = -1;    // -1 unset, 4 on, 0 failed
unsigned sDrLanes = 0;     // 0 when off

// Lanes
PcDrLaneDef sDrLaneDefs[kDrLaneCount];

// gW: the segment's Graphics
alignas(alignof(DGXGraphics)) unsigned char sGwStorage[sizeof(DGXGraphics)];
DGXGraphics* sGw = nullptr;
MaterialHandler sGwHandler;   // gW's handler

// Record queue
// A record, or a part's PART/END boundary.
enum { kDrRecDraw = 0, kDrRecPart = 1, kDrRecEnd = 2 };
struct PcDrRec {
	u8 lane;
	u8 kind;
	u16 part;
	u32 idx;
};
static_assert(sizeof(PcDrRec) == 8, "a record is two words");
PcDrRec* sDrRecs  = nullptr;
u8* sDrRecCls     = nullptr;   // price class per record
int sDrRecN       = 0;         // records in the open chain

// Parts
// Game thread Graphics fields captured at a part's fork.
enum {
	kDrKnownHasTexGen = 1u << 0,
	kDrKnownLighting  = 1u << 1,
	kDrKnownMtxDepIdx = 1u << 2,
	kDrKnownCullMode  = 1u << 3,
	kDrKnownBase      = 1u << 4,
	kDrKnownTexture0  = 1u << 5, // .. << 12: mActiveTexture[0..7]
};
struct PcDrFill {
	int renderMode;
	u32 matRenderMask;
	const Matrix4f* lastModelMatrix;
	CoreNode* lights;
	Texture* activeTexture[8];
	int screenWidth, screenHeight;
	Colour bufferClear, prim, aux, fogColour, ambient, c36C;
	bool lighting, depth, envMap;
	u8 debugSnap;
	BOOL hasTexGen;
	u32 mtxDepIdx, cullFlip, lightMask;
	int blend, cullMode, curMtxId, texMtxBase;
	Vector3f sunPosition;
	Material* currentMaterial;
	f32 fogStart, fogEnd, fogDensity, lightDist, lightIntensity, lineWidth;
	Vector3f* customScale;
	Mtx projTex;
	GColor colors0;
	u32 known;   // kDrKnown*
};
// Fork values for the seal.
struct PcDrForkState {
	const Matrix4f* active;
	BOOL hasTexGen;
	int base;
	bool lighting;
};
enum { kDrBaseMax = 16 };   // SPLICE candidates per part
struct PcDrPart {
	u16 seg;
	u8 index;                                // index in the chain (0: chain fork)
	PcDrForkState f;
	u32 forkKnown;                           // tracked fields known at the fork
	Matrix4f snap;                           // active matrix at the fork
	alignas(16) unsigned char cam[sizeof(Camera)]; // camera at the fork
	PcDrFill fill;                           // game thread fields at the fork
	PcDrPartStage stage;                     // staged recorder mirrors
	PcDrEffect acc;                          // folded record effects
	PcDrSets sets;                           // union of record write sets
	bool sealed;
	bool knownBase;                          // texgen base known at the seal
	// Texgen base candidates at the fork and at the seal.
	int startBaseN;
	unsigned startBases[kDrBaseMax];
	int baseN;
	unsigned bases[kDrBaseMax];
	// End texgen base left the candidates.
	bool endBaseLeft;
};
PcDrPart sDrParts[kDrPartSlots];
int sDrPartN = 0;             // parts in the open chain
int sDrCur   = 0;             // last forked part
inline PcDrPart& pcDrCur()
{
	return sDrParts[sDrCur];
}
// SPLICE candidates of the last sealed part.
unsigned sDrBases[kDrBaseMax];
int sDrBaseN = 0;             // -1: overflowed

// Chain in flight
DGXGraphics* sDrMain   = nullptr;   // game thread Graphics that forked it
volatile int sDrOpen   = 0;         // forked and not joined
bool sDrComplete       = false;     // no part will follow
bool sDrInJoin         = false;
bool sDrArmed          = false;
bool sDrWarmedFrame    = false;     // warm-up counted this frame
int sDrWarmup          = 0;
SceUID sDrForkThread   = -1;
u16 sDrChainNext       = 0;
int sDrForkRefusal     = -1;        // last fork refusal, or -1
struct alignas(32) PcDrAtomic {
	std::atomic<int> v;
};
PcDrAtomic sDrPublished, sDrAllPublished, sDrDone, sDrWorkerWaiting;

// Fork refusal reasons.
enum { kDrRefuseState = 0, kDrRefuseStack, kDrRefuseOther };

// Prices
// Per-class B cost (us) and largest record bytes.
float sDrPrice[kDrPcCount];
unsigned sDrBytesMax[kDrPcCount];
unsigned sDrChainBytes[kDrPcCount];

// Record worker
SceUID sDrThread = -1, sDrGo = -1, sDrPktSema = -1;
int sDrThreadState = 0; // 0 not tried, 1 running, -1 failed
volatile uintptr_t sDrStackLo = 0, sDrStackHi = 0;
std::atomic<int> sDrStackReady { 0 };

// Game thread stack range.
uintptr_t sDrGameStackLo = 0, sDrGameStackHi = 0;

// Shape reports
enum { kDrShapeSlots = 512 };
struct PcDrShapeSlot {
	const BaseShape* key;
	PcDrShapeInfo info;
};
PcDrShapeSlot sDrShapes[kDrShapeSlots];
unsigned sDrShapeCount = 0;

inline SceUInt64 pcDrNow()
{
	return sceKernelGetProcessTimeWide();
}

// B runner
void pcDrBeginPart(PcDrPart& p);
void pcDrEndPart(PcDrPart& p);
// Runs record k on gW.
void pcDrRunRec(int k)
{
	const PcDrRec& r = sDrRecs[k];
	if (r.kind == kDrRecPart) {
		pcDrBeginPart(sDrParts[r.part]);
		return;
	}
	if (r.kind == kDrRecEnd) {
		pcDrEndPart(sDrParts[r.part]);
		return;
	}
	const int cls     = sDrRecCls[k];
	const uint32_t b0 = vita_gxq_seg_bytes();
	sDrLaneDefs[r.lane].mBody(*sGw, int(r.idx));
	const unsigned b = unsigned(vita_gxq_seg_bytes() - b0);
	if (b > sDrChainBytes[cls]) {
		sDrChainBytes[cls] = b;
	}
}
// Wakes a sleeping worker.
void pcDrWakeWorker()
{
	if (sDrWorkerWaiting.v.load(std::memory_order_seq_cst) && sDrWorkerWaiting.v.exchange(0, std::memory_order_seq_cst) == 1) {
		sceKernelSignalSema(sDrPktSema, 1);
	}
}
// Worker: waits for record k or part completion.
void pcDrWorkerWait(int k)
{
	const SceUInt64 t0 = pcDrNow();
	for (unsigned n = 1;; n++) {
		if (sDrPublished.v.load(std::memory_order_acquire) > k || sDrAllPublished.v.load(std::memory_order_acquire)) {
			return;
		}
		if ((n & 63u) == 0 && unsigned(pcDrNow() - t0) > kDrSpinUs) {
			break;
		}
		__asm__ volatile("yield" ::: "memory");
	}
	sDrWorkerWaiting.v.store(1, std::memory_order_seq_cst);
	if (sDrPublished.v.load(std::memory_order_seq_cst) > k || sDrAllPublished.v.load(std::memory_order_seq_cst)) {
		if (sDrWorkerWaiting.v.exchange(0, std::memory_order_seq_cst) == 0) {
			const int pvIdle = pc_prof_idle_set(1);
			sceKernelWaitSema(sDrPktSema, 1, NULL);
			pc_prof_idle_set(pvIdle);
		}
		return;
	}
	const int pvIdle = pc_prof_idle_set(1);
	sceKernelWaitSema(sDrPktSema, 1, NULL);
	pc_prof_idle_set(pvIdle);
}
// Worker: runs one chain's records.
void pcDrWorkerRun()
{
	int k = 0;
	for (;;) {
		const int pub = sDrPublished.v.load(std::memory_order_acquire);
		if (k < pub) {
			pcDrRunRec(k);
			++k;
			continue;
		}
		if (sDrAllPublished.v.load(std::memory_order_acquire)) {
			if (k >= sDrPublished.v.load(std::memory_order_acquire)) {
				break;
			}
			continue;
		}
		pcDrWorkerWait(k);
	}
}
int pcDrWorkerMain(SceSize, void*)
{
	if (!sDrStackReady.load(std::memory_order_acquire)) {
		SceKernelThreadInfo info;
		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) >= 0 && info.stack && info.stackSize > 0) {
			sDrStackLo = uintptr_t(info.stack);
			sDrStackHi = uintptr_t(info.stack) + uintptr_t(info.stackSize);
			vita_offlock_register(sDrStackLo, sDrStackHi);
			sDrStackReady.store(1, std::memory_order_release);
		}
	}
	pc_prof_register(3);
	for (;;) {
		pc_prof_idle_set(1);
		sceKernelWaitSema(sDrGo, 1, NULL);
		pc_prof_idle_set(0);
		pcDrWorkerRun();
		sDrDone.v.store(1, std::memory_order_seq_cst);
		vita_gxq_seg_signal_j();
	}
	return 0;
}
// Starts the record worker.
bool pcDrThreadStart()
{
	if (sDrThreadState != 0) {
		return sDrThreadState > 0;
	}
	sDrThreadState = -1;
	sDrGo          = sceKernelCreateSema("nectar_drawrec_go", 0, 0, 1, NULL);
	sDrPktSema     = sceKernelCreateSema("nectar_drawrec_pkt", 0, 0, 8, NULL);
	sDrThread      = sceKernelCreateThread("nectar_drawrec", pcDrWorkerMain, kDrPrio, 64 * 1024, 0,
	                                       SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2, NULL);
	if (sDrGo < 0 || sDrPktSema < 0 || sDrThread < 0) {
		printf("[VITA DRAWREC] ERROR: the record worker could not be created (0x%08X 0x%08X 0x%08X)\n", unsigned(sDrGo),
		       unsigned(sDrPktSema), unsigned(sDrThread));
		return false;
	}
	SceKernelThreadInfo info;
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	if (sceKernelGetThreadInfo(sDrThread, &info) >= 0 && info.stack && info.stackSize > 0) {
		sDrStackLo = uintptr_t(info.stack);
		sDrStackHi = uintptr_t(info.stack) + uintptr_t(info.stackSize);
		vita_offlock_register(sDrStackLo, sDrStackHi);
		sDrStackReady.store(1, std::memory_order_release);
	}
	sceKernelStartThread(sDrThread, 0, NULL);
	for (int i = 0; i < 2000 && !sDrStackReady.load(std::memory_order_acquire); ++i) {
		sceKernelDelayThread(100);
	}
	if (!sDrStackReady.load(std::memory_order_acquire)) {
		printf("[VITA DRAWREC] ERROR: the record worker could not report its stack\n");
		return false;
	}
	sDrThreadState = 1;
	return true;
}

// Set-up
// Seed prices.
void pcDrSeedPrices()
{
	for (int c = 0; c < kDrPcCount; c++) {
		sDrPrice[c]    = 60.0f;
		sDrBytesMax[c] = 8192;
	}
	for (int t = 0; t < kDrTekiTypes; t++) {
		sDrPrice[kDrPcTeki0 + t]    = 80.0f;
		sDrBytesMax[kDrPcTeki0 + t] = 16384;
	}
	sDrPrice[kDrPcTeki0 + 7]     = 220.0f; // TEKI_Palm
	sDrPrice[kDrPcPiki]          = 96.0f;
	sDrBytesMax[kDrPcPiki]       = 4096;
	sDrPrice[kDrPcTekiMtx]       = 2.0f;
	sDrBytesMax[kDrPcTekiMtx]    = 256;
	sDrPrice[kDrPcGrass]         = 70.0f;
	sDrPrice[kDrPcUfo]           = 650.0f;
}
void pcDrInit()
{
	if (sDrMode >= 0) {
		return;
	}
	int mode = 4;
	pcDrSeedPrices();
	g_pcDrDeadMtx.makeIdentity();
	if (!vita_gxq_seg_pool_ready()) {
		printf("[VITA DRAWREC] ERROR: no segment pool: drawrec is off\n");
		mode = 0;
	}
	if (mode) {
		sDrRecs   = static_cast<PcDrRec*>(memalign(32, size_t(kDrRecMax) * sizeof(PcDrRec)));
		sDrRecCls = static_cast<u8*>(memalign(32, size_t(kDrRecMax)));
		if (!sDrRecs || !sDrRecCls) {
			printf("[VITA DRAWREC] ERROR: out of memory for the record queue: drawrec is off\n");
			mode = 0;
		}
	}
	if (mode) {
		// Game thread stack bounds.
		SceKernelThreadInfo info;
		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) >= 0 && info.stack && info.stackSize > 0) {
			sDrGameStackLo = uintptr_t(info.stack);
			sDrGameStackHi = uintptr_t(info.stack) + uintptr_t(info.stackSize);
		}
		sGw = new (sGwStorage) DGXGraphics(PcCloneTag());
		sGwHandler.mGfx               = sGw;
		sGw->mDefaultMaterialHandler = &sGwHandler;
		sGw->mCurrentMaterialHandler = &sGwHandler;
		if (!pcDrThreadStart()) {
			mode = 0;
		}
	}
	sDrMode  = mode;
	sDrLanes = mode ? kDrLanesOn : 0u;
}

// All writes equal the fork value.
bool pcDrSingleton(const PcDrSet& w, unsigned atFork)
{
	if (w.mOver) {
		return false;
	}
	for (int i = 0; i < w.mN; i++) {
		if (w.mV[i] != atFork) {
			return false;
		}
	}
	return true;
}

// Pointer is on the game thread's stack.
bool pcDrOnGameStack(const void* p)
{
	const uintptr_t lo = sDrGameStackLo, hi = sDrGameStackHi;
	return hi > lo && uintptr_t(p) - lo < hi - lo;
}

// Pool has room for the expected records.
bool pcDrPoolRoom(unsigned expectedBytes)
{
	const unsigned long long need  = (unsigned long long)expectedBytes * 5ull / 4ull;
	const unsigned blocks          = unsigned((need + vita_gxq_seg_block_bytes() - 1) / vita_gxq_seg_block_bytes()) + 1u;
	return vita_gxq_seg_free_blocks() >= blocks;
}

// Captures the game thread's Graphics at a part's fork.
void pcDrFillCapture(PcDrFill& f, const DGXGraphics& m)
{
	f.renderMode      = m.mRenderMode;
	f.matRenderMask   = m.mMatRenderMask;
	f.lastModelMatrix = m.mLastModelMatrix;
	// Light list.
	f.lights          = m.mLight.mChild;
	f.known           = 0;
	for (int i = 0; i < 8; i++) {
		f.activeTexture[i] = m.mActiveTexture[i].pcPeek();
		f.known |= m.mActiveTexture[i].pcUnknown() ? 0u : u32(kDrKnownTexture0) << i;
	}
	f.screenWidth     = m.mScreenWidth;
	f.screenHeight    = m.mScreenHeight;
	f.bufferClear     = m.mBufferClearColour;
	f.prim            = m.mPrimaryColour;
	f.aux             = m.mAuxiliaryColour;
	f.lighting        = m.mIsLightingEnabled.pcPeek();
	f.depth           = m.mIsDepthEnabled;
	f.hasTexGen       = m.mHasTexGen.pcPeek();
	f.mtxDepIdx       = m.mMtxDepIdx.pcPeek();
	f.blend           = m.mBlendMode;
	f.cullMode        = m.mCullMode.pcPeek();
	f.cullFlip        = m.mCullFlip;
	f.sunPosition     = m.mSunPosition;
	f.currentMaterial = m.mCurrentMaterial;
	f.fogStart        = m.mFogStart;
	f.fogEnd          = m.mFogEnd;
	f.fogDensity      = m.mFogDensity;
	f.fogColour       = m.mFogColour;
	f.ambient         = m.mAmbientColour;
	f.c36C            = m._36C;
	f.lightDist       = m.mLightDistance;
	f.lightIntensity  = m.mLightIntensity;
	f.lightMask       = m.mActiveLightMask;
	f.lineWidth       = m.mLineWidth;
	f.customScale     = m.mCustomScale;
	f.debugSnap       = gsys->mToggleDebugInfo ? 1 : 0;
	f.envMap          = m.mIsEnvMapActive;
	f.curMtxId        = m.mCurrentMatrixId;
	f.texMtxBase      = m.mTexMtxBaseID.pcPeek();
	memcpy(f.projTex, m.mProjectionTextureMatrix, sizeof(f.projTex));
	f.colors0         = gPcDgxMain.colors[0];
	f.known |= (m.mHasTexGen.pcUnknown() ? 0u : u32(kDrKnownHasTexGen)) | (m.mIsLightingEnabled.pcUnknown() ? 0u : u32(kDrKnownLighting))
	         | (m.mMtxDepIdx.pcUnknown() ? 0u : u32(kDrKnownMtxDepIdx)) | (m.mCullMode.pcUnknown() ? 0u : u32(kDrKnownCullMode))
	         | (m.mTexMtxBaseID.pcUnknown() ? 0u : u32(kDrKnownBase));
}
// Applies a part's fill to gW.
void pcDrFillApply(PcDrPart& p)
{
	DGXGraphics& w       = *sGw;
	const PcDrFill& f    = p.fill;
	w.mRenderMode        = f.renderMode;
	w.mMatRenderMask     = f.matRenderMask;
	w.mLastModelMatrix   = f.lastModelMatrix;
	w.mActiveMatrix      = &p.snap;
	w.mLight.Child(f.lights);
	w.mCamera            = reinterpret_cast<Camera*>(p.cam);
	for (int i = 0; i < 8; i++) {
		if (f.known & (u32(kDrKnownTexture0) << i)) {
			w.mActiveTexture[i].pcAdopt(f.activeTexture[i]);
		}
	}
	w.mScreenWidth       = f.screenWidth;
	w.mScreenHeight      = f.screenHeight;
	w.mBufferClearColour = f.bufferClear;
	w.mPrimaryColour     = f.prim;
	w.mAuxiliaryColour   = f.aux;
	if (f.known & kDrKnownLighting) {
		w.mIsLightingEnabled.pcAdopt(f.lighting);
	}
	w.mIsDepthEnabled    = f.depth;
	if (f.known & kDrKnownHasTexGen) {
		w.mHasTexGen.pcAdopt(f.hasTexGen);
	}
	if (f.known & kDrKnownMtxDepIdx) {
		w.mMtxDepIdx.pcAdopt(f.mtxDepIdx);
	}
	w.mBlendMode         = f.blend;
	if (f.known & kDrKnownCullMode) {
		w.mCullMode.pcAdopt(f.cullMode);
	}
	w.mCullFlip          = f.cullFlip;
	w.mLightCam          = nullptr;
	w.mSunPosition       = f.sunPosition;
	w.mCurrentMaterial   = f.currentMaterial;
	w.mFogStart          = f.fogStart;
	w.mFogEnd            = f.fogEnd;
	w.mFogDensity        = f.fogDensity;
	w.mFogColour         = f.fogColour;
	w.mAmbientColour     = f.ambient;
	w._36C               = f.c36C;
	w.mLightDistance     = f.lightDist;
	w.mLightIntensity    = f.lightIntensity;
	w.mActiveLightMask   = f.lightMask;
	w.mLineWidth         = f.lineWidth;
	w.mCustomScale       = f.customScale;
	w.mPcPose            = nullptr;
	w.mPcPoseShape       = nullptr;
	w.mPcPoseAnimCtx     = 0;
	w.mPcPoseAnimData    = 0;
	w.mPcNoPrev          = 0;
	w.mPcCacheHoisted    = 0;
	w.mPcDebugSnap       = f.debugSnap;
	w.mIsEnvMapActive    = f.envMap;
	w.mCurrentMatrixId   = f.curMtxId;
	if (f.known & kDrKnownBase) {
		w.mTexMtxBaseID.pcAdopt(f.texMtxBase);
	}
	memcpy(w.mProjectionTextureMatrix, f.projTex, sizeof(w.mProjectionTextureMatrix));
	gPcDgxSub.colors[0] = f.colors0;
}

// Part boundaries
// PART: fences the recorder and DGX context, applies the fill.
void pcDrBeginPart(PcDrPart& p)
{
	vita_gxq_seg_part_begin(p.stage);
	pcDgxFence(gPcDgxSub);
	pcDrFillApply(p);
}
// Value is one of the part's SPLICE candidates.
bool pcDrBaseCandidate(const PcDrPart& p, unsigned base)
{
	for (int i = 0; i < p.baseN; i++) {
		if (p.bases[i] == base) {
			return true;
		}
	}
	return false;
}
// END: SUB_END, last block, end texgen base check.
void pcDrEndPart(PcDrPart& p)
{
	const unsigned base = unsigned(sGw->mTexMtxBaseID.pcPeek());
	vita_gxq_seg_finish(sGw->mHasTexGen.pcPeek() ? 1u : 0u, u32(base));
	p.endBaseLeft = !p.knownBase && p.baseN >= 0 && !pcDrBaseCandidate(p, base);
}

// Any game thread field still unknown.
bool pcDrAnyUnknown()
{
	const DGXGraphics& m = *sDrMain;
	bool any = m.mHasTexGen.pcUnknown() || m.mIsLightingEnabled.pcUnknown() || m.mMtxDepIdx.pcUnknown() || m.mCullMode.pcUnknown()
	        || m.mTexMtxBaseID.pcUnknown();
	for (int i = 0; i < 8; i++) {
		any = any || m.mActiveTexture[i].pcUnknown();
	}
	return any;
}
// Adopts gW's end values into unknown fields.
void pcDrAdoptUnknown()
{
	DGXGraphics& m = *sDrMain;
	DGXGraphics& w = *sGw;
	if (m.mHasTexGen.pcUnknown()) {
		m.mHasTexGen.pcAdopt(w.mHasTexGen.pcPeek());
	}
	if (m.mIsLightingEnabled.pcUnknown()) {
		m.mIsLightingEnabled.pcAdopt(w.mIsLightingEnabled.pcPeek());
	}
	for (int i = 0; i < 8; i++) {
		if (m.mActiveTexture[i].pcUnknown()) {
			m.mActiveTexture[i].pcAdopt(w.mActiveTexture[i].pcPeek());
		}
	}
	if (m.mMtxDepIdx.pcUnknown()) {
		m.mMtxDepIdx.pcAdopt(w.mMtxDepIdx.pcPeek());
	}
	if (m.mCullMode.pcUnknown()) {
		m.mCullMode.pcAdopt(w.mCullMode.pcPeek());
	}
	if (m.mTexMtxBaseID.pcUnknown()) {
		m.mTexMtxBaseID.pcAdopt(w.mTexMtxBaseID.pcPeek());
	}
}

// Waits for the worker to finish the chain.
void pcDrJoinWait()
{
	bool raised = false;
	while (!sDrDone.v.load(std::memory_order_acquire)) {
		if (vita_gxq_seg_take_starved() && vita_gxq_seg_call_open()) {
			vita_gxq_seg_rescue();
		}
		if (sDrDone.v.load(std::memory_order_acquire)) {
			break;
		}
		if (vita_gxq_seg_wait_j(250000) < 0 && !sDrDone.v.load(std::memory_order_acquire) && !raised) {
			raised = true;
			sceKernelChangeThreadPriority(sDrThread, 0xB0);
		}
	}
	if (raised) {
		sceKernelChangeThreadPriority(sDrThread, kDrPrio);
	}
}

// Publishes a PART or END record.
void pcDrPublishMarker(int kind, int part)
{
	PcDrRec& r         = sDrRecs[sDrRecN];
	r.lane             = 0;
	r.kind             = u8(kind);
	r.part             = u16(part);
	r.idx              = u32(part);
	sDrRecCls[sDrRecN] = 0;
	const int n        = ++sDrRecN;
	sDrPublished.v.store(n, std::memory_order_seq_cst);
	pcDrWakeWorker();
}

// Rescues a starved worker.
void pcDrPollStarved()
{
	if (!sDrOpen) {
		return;
	}
	if (vita_gxq_seg_call_open() && vita_gxq_seg_take_starved()) {
		vita_gxq_seg_rescue();
	}
}

// Captures a part's fork state.
void pcDrPartTake(PcDrPart& p, DGXGraphics& m, int index)
{
	p.index          = u8(index);
	PcDrForkState& f = p.f;
	f.active         = m.mActiveMatrix;
	f.hasTexGen      = m.mHasTexGen.pcPeek();
	f.base           = m.mTexMtxBaseID.pcPeek();
	f.lighting       = m.mIsLightingEnabled.pcPeek();
	memcpy(&p.snap, m.mActiveMatrix, sizeof(Matrix4f));
	// Camera copy.
	const Camera* cam = m.mCamera;
	memcpy(p.cam, cam, sizeof(Camera));
	Camera* copy = reinterpret_cast<Camera*>(p.cam);
	for (int i = 0; i < 6; i++) {
		const uintptr_t a = uintptr_t(cam->mPlanePointers[i]) - uintptr_t(&cam->mCullPlanes[0]);
		if (a < sizeof(cam->mCullPlanes)) {
			copy->mPlanePointers[i] = &copy->mCullPlanes[a / sizeof(CullingPlane)];
		}
	}
	memset(&p.acc, 0, sizeof(p.acc));
	memset(&p.sets, 0, sizeof(p.sets));
	p.sealed      = false;
	p.knownBase   = false;
	p.endBaseLeft = false;
	pcDrFillCapture(p.fill, m);
	p.forkKnown = p.fill.known;
	// Texgen base candidates at the fork.
	if (p.forkKnown & kDrKnownBase) {
		p.startBaseN    = 1;
		p.startBases[0] = unsigned(f.base);
	} else {
		p.startBaseN = sDrBaseN;
		for (int i = 0; i < sDrBaseN; i++) {
			p.startBases[i] = sDrBases[i];
		}
	}
}

// Forks a chain (part 0).
Graphics* pcDrFork(Graphics& gfx, int lane, float expectedUs, unsigned expectedBytes, bool eligible)
{
	(void)lane;
	pcDrInit();
	if (sDrOpen) {
		vita_drawrec_join(kDrJoinFork);
	}
	DGXGraphics& m = static_cast<DGXGraphics&>(gfx);
	int refusal    = -1;
	if (sDrMode == 0 || gDrawrecKill || !sDrArmed || !eligible) {
		refusal = kDrRefuseOther;
	} else if (m.mCurrentMaterialHandler != m.mDefaultMaterialHandler || m.mLightCam || m.mPcPoseShape || m.mPcCacheHoisted) {
		refusal = kDrRefuseState;
	} else if (!m.mActiveMatrix || pcDrOnGameStack(m.mActiveMatrix)) {
		refusal = kDrRefuseStack;
	} else if (expectedUs < kDrMinBUs) {
		refusal = kDrRefuseOther;
	} else if (sDrWarmup < kDrWarmupFrames) {
		if (!sDrWarmedFrame) {
			sDrWarmedFrame = true;
			++sDrWarmup;
		}
		refusal = kDrRefuseOther;
	} else if (!pcDrPoolRoom(expectedBytes)) {
		refusal = kDrRefuseOther;
	}
	sDrForkRefusal = refusal;
	if (refusal >= 0) {
		return nullptr;
	}

	sDrMain       = &m;
	sDrForkThread = sceKernelGetThreadId();
	if (pcDrAnyUnknown()) {
		pcDrAdoptUnknown();
	}
	// Part 0.
	sDrPartN    = 1;
	sDrCur      = 0;
	PcDrPart& p = sDrParts[0];
	pcDrPartTake(p, m, 0);
	// Recorder and DGX fork, gW.
	p.seg = vita_gxq_seg_fork();
	pcDgxForkCopy(gPcDgxMain, gPcDgxSub);
	pcDgxFence(gPcDgxMain);
	pcDrFillApply(p);
	// Chain id.
	if (++sDrChainNext == 0) {
		sDrChainNext = 1;
	}
	g_pcDrChain        = sDrChainNext;
	g_pcDrChainHoisted = 0;
	sDrRecN            = 0;
	memset(sDrChainBytes, 0, sizeof(sDrChainBytes));
	for (int c = 0; c < kDrPcCount; c++) {
		sDrBytesMax[c] -= sDrBytesMax[c] >> 6;
	}
	sDrComplete = false;
	sDrPublished.v.store(0, std::memory_order_relaxed);
	sDrAllPublished.v.store(0, std::memory_order_relaxed);
	sDrDone.v.store(0, std::memory_order_relaxed);
	sDrWorkerWaiting.v.store(0, std::memory_order_relaxed);
	sDrOpen = 1;
	sceKernelSignalSema(sDrGo, 1);
	return sGw;
}
// Forks a chain for a part.
Graphics* pcDrForkForPart(Graphics& gfx, int lane, float expectedUs, unsigned expectedBytes, bool* retry)
{
	Graphics* w = pcDrFork(gfx, lane, expectedUs, expectedBytes, true);
	if (!w && retry) {
		*retry = sDrForkRefusal == kDrRefuseState || sDrForkRefusal == kDrRefuseStack;
	}
	return w;
}
// Shape report.
void pcDrTexMtxBaseWrites(const Material& mat, int depIdx, PcDrSet& out)
{
	// setMatMatrices writes to mTexMtxBaseID.
	const bool hasTexGen = mat.mTextureInfo.mTevStageCount != 0;
	int matrixType       = 30;
	for (u32 i = 0, j = 0; i < mat.mTextureInfo.mTexGenDataCount; i++) {
		if (mat.mTextureInfo.mTexGenData[i].mTexGenType != 1) {
			continue;
		}
		if (mat.mTextureInfo.mTextureData[j].mAnimationFactor != 0xFF) {
			if (hasTexGen && mat.mTextureInfo.mTexGenData[i].mTexGenSrc == 1) {
				out.add(u32(matrixType));
				matrixType += int(mat.mTextureInfo.mTevStageCount) * depIdx * 3;
			} else {
				matrixType += 3;
			}
		}
		j++;
	}
}
inline bool pcDrPlainTexture(const Texture* tex)
{
	return !tex || (g_pcPlainTextureVtbl && *reinterpret_cast<const void* const*>(tex) == g_pcPlainTextureVtbl);
}
void pcDrComputeShapeInfo(const BaseShape* s, PcDrShapeInfo& info)
{
	memset(&info, 0, sizeof(info));
	info.shape        = s;
	info.lights       = s->mLightGroup.Child() != nullptr;
	const bool caching = (s->mShapeFlags & ShapeFlags::AllowCaching) != 0;
	const bool redraw  = (s->mShapeFlags & ShapeFlags::AlwaysRedraw) != 0;
	int hidden = 0;
	for (int i = 0; i < s->mMeshCount; i++) {
		const Mesh& mesh = s->mMeshList[i];
		if (mesh.mJointList && !JOINT_IS_VISIBLE(mesh.mJointList[0])) {
			++hidden;
		}
	}
	info.hiddenJoints = hidden != 0;
	bool dl = false, plain = true, blend = false;
	int nonPvw = 0, scaled = 0, env = 0;
	for (int i = 0; i < s->mMaterialCount; i++) {
		const Material& mat = s->mMaterialList[i];
		blend = blend || (mat.mFlags & MATFLAG_AlphaBlend) != 0;
		plain = plain && pcDrPlainTexture(mat.mTexture) && pcDrPlainTexture(mat.mEnvMapTexture);
		for (u32 k = 0; k < mat.mTextureInfo.mTextureDataCount; k++) {
			plain = plain && pcDrPlainTexture(mat.mTextureInfo.mTextureData[k].mTexture);
		}
		if (mat.mFlags & MATFLAG_PVW) {
			dl = dl || mat.mDisplayListPtr != nullptr;
			scaled += mat.mTextureInfo.mUseScale ? 1 : 0;
			// PVW branch.
			for (int p = 0; p < s->mTotalMatpolyCount; p++) {
				const Joint::MatPoly* mp = s->mMatpolyList[p];
				if (mp && mp->mIndex == i && mp->mMeshIndex >= 0 && mp->mMeshIndex < s->mMeshCount) {
					pcDrTexMtxBaseWrites(mat, s->mMeshList[mp->mMeshIndex].mMtxDepIdx, info.writes.mTexMtxBase);
				}
			}
			info.writes.mHasTexGen.add(mat.mTextureInfo.mTevStageCount != 0 ? 1u : 0u);
			info.writes.mLighting.add((mat.mLightingInfo.mCtrlFlag & LightingControlFlags::EnableColor0) ? 1u : 0u);
		} else {
			// Non-PVW branch.
			++nonPvw;
			info.writes.mHasTexGen.add(0u);
			if (mat.mEnvMapTexture) {
				++env;
				info.writes.mHasTexGen.add(1u);
			}
		}
	}
	info.nonPvw      = nonPvw != 0;
	info.scaled      = scaled != 0;
	info.env         = env != 0;
	info.cachedBlend = caching && !redraw && blend;
	info.ok          = !info.lights && !dl && plain && g_pcPlainTextureVtbl;
	info.why         = info.lights ? "a light group (drawlights)"
	                 : dl          ? "a register display-list material (it can write state the fence keeps)"
	                 : !g_pcPlainTextureVtbl ? "no plain Texture built yet to compare with"
	                 : !plain      ? "a texture that is not a plain Texture (makeResident)"
	                               : nullptr;
}
void pcDrShapeInfoClear()
{
	if (!sDrShapeCount) {
		return;
	}
	for (int i = 0; i < kDrShapeSlots; i++) {
		sDrShapes[i].key = nullptr;
	}
	sDrShapeCount = 0;
}

} // namespace

// Lane interface
void pcDrawrecRegisterLane(int lane, const PcDrLaneDef& def)
{
	if (lane >= 0 && lane < kDrLaneCount) {
		sDrLaneDefs[lane] = def;
	}
}

unsigned pcDrawrecLanes(void)
{
	pcDrInit();
	return sDrLanes;
}

int pcDrawrecMode(void)
{
	pcDrInit();
	return sDrMode;
}

float pcDrawrecPrice(int cls)
{
	return (cls >= 0 && cls < kDrPcCount) ? sDrPrice[cls] : 0.0f;
}

unsigned pcDrawrecBytes(int cls)
{
	return (cls >= 0 && cls < kDrPcCount) ? sDrBytesMax[cls] : 0u;
}

bool pcDrawrecPartOpen(void)
{
	return sDrOpen != 0 && !pcDrCur().sealed;
}

bool pcDrawrecChainOpen(void)
{
	return sDrOpen != 0;
}

int pcDrawrecSpliceBases(unsigned* out, int max)
{
	if (sDrBaseN < 0 || sDrBaseN > max) {
		return -1;
	}
	for (int i = 0; i < sDrBaseN; i++) {
		out[i] = sDrBases[i];
	}
	return sDrBaseN;
}

Graphics* pcDrawrecForkChain(Graphics& gfx, float expectedUs, unsigned expectedBytes)
{
	return pcDrFork(gfx, kDrLaneTeki, expectedUs, expectedBytes, true);
}

Graphics* pcDrawrecForkPart(Graphics& gfx, int lane, float expectedUs, unsigned expectedBytes, bool refork, bool* retry)
{
	pcDrInit();
	if (retry) {
		*retry = false;
	}
	if (!sDrOpen) {
		return pcDrForkForPart(gfx, lane, expectedUs, expectedBytes, retry);
	}
	if (!pcDrCur().sealed) {
		return sGw;
	}
	if (sDrComplete) {
		// New chain.
		return pcDrForkForPart(gfx, lane, expectedUs, expectedBytes, retry);
	}
	// Chain limits.
	if (sDrPartN >= kDrMaxParts || sDrRecN + 3 > kDrRecMax || vita_gxq_seg_call_chunk_closes() >= 2) {
		vita_drawrec_join(kDrJoinParts);
		return pcDrForkForPart(gfx, lane, expectedUs, expectedBytes, retry);
	}
	DGXGraphics& m = static_cast<DGXGraphics&>(gfx);
	int refusal    = -1;
	if (gDrawrecKill || !sDrArmed) {
		refusal = kDrRefuseOther;
	} else if (m.mCurrentMaterialHandler != m.mDefaultMaterialHandler || m.mLightCam || m.mPcPoseShape || m.mPcCacheHoisted) {
		refusal = kDrRefuseState;
	} else if (!m.mActiveMatrix || pcDrOnGameStack(m.mActiveMatrix)) {
		refusal = kDrRefuseStack;
	} else if (refork && expectedUs < kDrReforkUs) {
		refusal = kDrRefuseOther;
	} else if (!pcDrPoolRoom(expectedBytes)) {
		refusal = kDrRefuseOther;
	}
	if (refusal >= 0) {
		if (retry) {
			*retry = refusal == kDrRefuseState || refusal == kDrRefuseStack;
		}
		return nullptr;
	}
	// Part k fork.
	const int k = sDrPartN++;
	sDrCur      = k;
	PcDrPart& p = sDrParts[k];
	pcDrPartTake(p, m, k);
	p.seg = vita_gxq_seg_part_fork(p.stage);
	pcDgxFence(gPcDgxMain);
	pcDrPublishMarker(kDrRecPart, k);
	pcDrPollStarved();
	return sGw;
}

void pcDrawrecPartBreak(int objType)
{
	(void)objType;
	if (!sDrOpen || pcDrCur().sealed) {
		return;
	}
	pcDrawrecSeal();
}

float pcDrawrecReforkUs(void)
{
	return kDrReforkUs;
}

float pcDrawrecMinB(void)
{
	return kDrMinBUs;
}

Graphics* pcDrawrecEnter(Graphics& gfx, int lane, const PcDrSets& sets, float expectedUs, unsigned expectedBytes, bool eligible,
                         const char* why)
{
	(void)why;
	pcDrInit();
	if (sDrOpen && !pcDrCur().sealed) {
		// Append to the open part.
		if (!eligible) {
			pcDrawrecSeal();
			return nullptr;
		}
		pcDrCur().sets.addAll(sets);
		return sGw;
	}
	Graphics* w = pcDrFork(gfx, lane, expectedUs, expectedBytes, eligible);
	if (w) {
		pcDrCur().sets.addAll(sets);
	}
	return w;
}

void pcDrawrecAddSets(const PcDrSets& sets)
{
	pcDrCur().sets.addAll(sets);
}

void pcDrawrecAddLightingWrite(unsigned v)
{
	pcDrCur().sets.mLighting.add(v);
}

bool pcDrawrecCanPublish(void)
{
	if (!sDrOpen || pcDrCur().sealed) {
		return false;
	}
	if (sDrRecN + 2 > kDrRecMax) {
		pcDrawrecBail();
		return false;
	}
	return true;
}

bool pcDrawrecPublishRec(int lane, int idx, const PcDrEffect& e, int priceClass)
{
	if (!sDrOpen || pcDrCur().sealed || sDrRecN + 2 > kDrRecMax || lane < 0 || lane >= kDrLaneCount || !sDrLaneDefs[lane].mBody) {
		return false;
	}
	const int cls = (priceClass >= 0 && priceClass < kDrPcCount) ? priceClass : kDrPcPiki;
	PcDrRec& r    = sDrRecs[sDrRecN];
	r.lane        = u8(lane);
	r.kind        = kDrRecDraw;
	r.part        = u16(sDrCur);
	r.idx         = u32(idx);
	sDrRecCls[sDrRecN] = u8(cls);
	// Fold the effect.
	PcDrEffect& a = pcDrCur().acc;
	a.meshes |= e.meshes;
	a.curMtx0 |= e.curMtx0;
	a.lightModel |= e.lightModel;
	if (e.active) {
		a.active    = e.active;
		a.activePtr = e.activePtr;
	}
	const int n = ++sDrRecN;
	sDrPublished.v.store(n, std::memory_order_seq_cst);
	pcDrWakeWorker();
	if ((n & 15) == 0) {
		pcDrPollStarved();
	}
	return true;
}

void pcDrawrecSeal(void)
{
	if (!sDrOpen || pcDrCur().sealed) {
		return;
	}
	PcDrPart& p = pcDrCur();
	// Install fixed values; mark the rest unknown.
	DGXGraphics& m      = *sDrMain;
	const PcDrEffect& a = p.acc;
	if (a.meshes) {
		m.mCustomScale    = nullptr; // drawMeshes' useMaterial(nullptr)
		m.mIsEnvMapActive = false;
	}
	if (a.curMtx0) {
		m.mCurrentMatrixId = 0; // drawshape's restore / a useMatrix(..., 0)
	}
	m.mActiveMatrix = a.active == 0 ? p.f.active : a.active == 1 ? a.activePtr : &g_pcDrDeadMtx;
	if (a.lightModel) {
		// calcLighting pair.
		m.pcLightingModel();
	}
	// Candidate sets.
	const bool knownHasTexGen = (p.forkKnown & kDrKnownHasTexGen) && pcDrSingleton(p.sets.mHasTexGen, p.f.hasTexGen ? 1u : 0u);
	const bool knownBase      = (p.forkKnown & kDrKnownBase) && pcDrSingleton(p.sets.mTexMtxBase, unsigned(p.f.base));
	const bool knownLighting  = (p.forkKnown & kDrKnownLighting) && pcDrSingleton(p.sets.mLighting, p.f.lighting ? 1u : 0u);
	if (p.sets.mTexMtxBase.mOver || p.startBaseN < 0) {
		p.baseN = -1;
	} else {
		p.baseN = p.startBaseN;
		for (int i = 0; i < p.startBaseN; i++) {
			p.bases[i] = p.startBases[i];
		}
		for (int i = 0; i < p.sets.mTexMtxBase.mN && p.baseN >= 0; i++) {
			const unsigned v = p.sets.mTexMtxBase.mV[i];
			bool have        = false;
			for (int j = 0; j < p.baseN; j++) {
				have = have || p.bases[j] == v;
			}
			if (have) {
				continue;
			}
			if (p.baseN == kDrBaseMax) {
				p.baseN = -1;
				break;
			}
			p.bases[p.baseN++] = v;
		}
	}
	sDrBaseN = p.baseN;
	for (int i = 0; i < p.baseN; i++) {
		sDrBases[i] = p.bases[i];
	}
	p.knownBase = knownBase;
	if (!knownHasTexGen) {
		m.mHasTexGen.pcMarkUnknown();
	}
	if (!knownBase) {
		m.mTexMtxBaseID.pcMarkUnknown();
	}
	if (!knownLighting) {
		m.mIsLightingEnabled.pcMarkUnknown();
	}
	for (int i = 0; i < 8; i++) {
		m.mActiveTexture[i].pcMarkUnknown();
	}
	m.mMtxDepIdx.pcMarkUnknown();
	m.mCullMode.pcMarkUnknown();
	p.sealed = true;
	// Part END.
	pcDrPublishMarker(kDrRecEnd, p.index);
	pcDrPollStarved();
}

void pcDrawrecChainComplete(void)
{
	if (!sDrOpen || !pcDrCur().sealed || sDrComplete) {
		return;
	}
	sDrComplete = true;
	sDrAllPublished.v.store(1, std::memory_order_seq_cst);
	pcDrWakeWorker();
}

void pcDrawrecBail(void)
{
	if (!sDrOpen) {
		return;
	}
	pcDrawrecSeal();
	vita_drawrec_join(kDrJoinBail);
}
// Shape reports and hoist
const PcDrShapeInfo& pcDrShapeInfo(const BaseShape* s)
{
	static PcDrShapeInfo sFull;
	if (!g_pcPlainTextureVtbl) {
		pcDrComputeShapeInfo(s, sFull);
		return sFull;
	}
	u32 h = u32(uintptr_t(s) >> 4) * 2654435761u;
	for (int probe = 0; probe < kDrShapeSlots; probe++) {
		PcDrShapeSlot& slot = sDrShapes[(h + u32(probe)) & (kDrShapeSlots - 1)];
		if (slot.key == s) {
			return slot.info;
		}
		if (!slot.key) {
			pcDrComputeShapeInfo(s, slot.info);
			slot.key = s;
			++sDrShapeCount;
			return slot.info;
		}
	}
	memset(&sFull, 0, sizeof(sFull));
	sFull.shape = s;
	sFull.why   = "the shape report table is full (512 shapes)";
	return sFull;
}

bool pcDrShapeDrawsMeshes(const BaseShape* s, uint32_t mask)
{
	// drawshape's drawMeshes branch.
	if (!s->mMeshCount) {
		return false;
	}
	if (!(s->mShapeFlags & ShapeFlags::AlwaysRedraw) && (s->mShapeFlags & ShapeFlags::AllowCaching)) {
		mask &= ~u32(MATFLAG_AlphaBlend);
	}
	return (s->mShapeFlags & ShapeFlags::AlwaysRedraw) || (mask & (MATFLAG_Opaque | MATFLAG_AlphaTest | MATFLAG_InverseColorBlend));
}

bool pcDrHoistCache(Graphics& m, BaseShape* s, ShapeDynMaterials* dm)
{
	// drawshape's cache predicate.
	if (!s->mMeshCount || (s->mShapeFlags & ShapeFlags::AlwaysRedraw) || !(s->mShapeFlags & ShapeFlags::AllowCaching)
	    || !(m.mMatRenderMask & MATFLAG_AlphaBlend)) {
		return false;
	}
	m.cacheShape(s, dm);
	g_pcDrChainHoisted = 1;
	return true;
}

// Joins before a game thread draw of a stamped shape.
void pcDrStampJoin(const BaseShape* s, int site)
{
	(void)s;
	(void)site;
	if (!sDrOpen || sceKernelGetThreadId() != sDrForkThread) {
		return;
	}
	vita_drawrec_join(kDrJoinStamp);
}

// Unknown DGX field read.
void pcDrawrecUnknownRead(void)
{
	if (sDrForkThread < 0 || sceKernelGetThreadId() != sDrForkThread) {
		gDrawrecKill = 1;
		return;
	}
	if (sDrOpen) {
		vita_drawrec_join(kDrJoinUnknown);
	}
	if (sDrMain && !sDrOpen && pcDrAnyUnknown()) {
		pcDrAdoptUnknown();
	}
}

extern "C" void vita_drawrec_join(int site)
{
	if (site == kDrJoinReset) {
		pcDrShapeInfoClear();
		pcDrS2Reset();
	}
	if (!sDrOpen || sDrInJoin) {
		return;
	}
	if (sceKernelGetThreadId() != sDrForkThread) {
		return;
	}
	sDrInJoin = true;
	if (!pcDrCur().sealed) {
		pcDrawrecSeal();
	}
	sDrAllPublished.v.store(1, std::memory_order_seq_cst);
	pcDrWakeWorker();
	pcDrJoinWait();
	// Texgen base left its candidates.
	for (int k = 0; k < sDrPartN; k++) {
		if (sDrParts[k].endBaseLeft) {
			vita_gxq_forget_tex_mtx();
			gDrawrecKill = 1;
			break;
		}
	}
	pcDrAdoptUnknown();
	vita_gxq_seg_sync_latches();
	vita_gxq_seg_retire();
	pcDgxRetire(gPcDgxSub, gPcDgxMain);
	// Pool gate sizes.
	for (int c = 0; c < kDrPcCount; c++) {
		if (sDrChainBytes[c] > sDrBytesMax[c]) {
			sDrBytesMax[c] = sDrChainBytes[c];
		}
	}
	g_pcDrChain        = 0;
	g_pcDrChainHoisted = 0;
	g_pcDrTekiRec      = 0;
	sDrPartN           = 0;
	sDrCur             = 0;
	sDrOpen            = 0;
	sDrInJoin          = false;
}

extern "C" void vita_drawrec_chunk_closed(void)
{
	if (sDrOpen) {
		vita_drawrec_join(kDrJoinChunk);
	}
}

extern "C" void vita_drawrec_arm(int on)
{
	if (on) {
		sDrArmed       = true;
		sDrWarmedFrame = false;
		return;
	}
	vita_drawrec_join(kDrJoinSim);
	sDrArmed = false;
}

extern "C" int vita_drawrec_on_worker(void)
{
	if (!sDrStackReady.load(std::memory_order_acquire)) {
		return 0;
	}
	uintptr_t sp;
	__asm__ volatile("mov %0, sp" : "=r"(sp));
	return sp - sDrStackLo < sDrStackHi - sDrStackLo;
}
#endif
