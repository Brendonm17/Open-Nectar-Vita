#pragma once
// Drawrec: the spliced second GX recorder (pcDrawRec.cpp).
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Join sites.
enum {
    kDrJoinSim = 0,   // NewPikiGame::update, after BaseGameSection::draw
    kDrJoinGil,       // nectar_gil_release (os_stubs.cpp)
    kDrJoinSync,      // vita_gxq_sync
    kDrJoinFree,      // vita_gxq_note_free
    kDrJoinPresent,   // pc_gfx_present
    kDrJoinChunk,     // main chunk hand-over (vita_gxq.cpp close_chunk)
    kDrJoinReset,     // GameFlow::softReset
    kDrJoinMovie,     // MoviePlayer::startMovie
    kDrJoinAttach,    // StdSystem::attachObjs
    kDrJoinCine,      // CinematicPlayer / Actor set-up
    kDrJoinFork,      // next fork with the last segment open
    kDrJoinUnknown,   // read of an unknown DGX field
    kDrJoinBail,      // lane ended its part early
    kDrJoinFlush,     // Graphics::flushCachedShapes on a hoisted or stamped shape
    kDrJoinStamp,     // game-thread draw of a shape the open chain owns
    kDrJoinParts      // part fork past the part limit
};
// Join the open chain.
void vita_drawrec_join(int site);
// Main chunk hand-over count reached.
void vita_drawrec_chunk_closed(void);
// Arms the gameplay draw.
void vita_drawrec_arm(int on);
// Caller is the record worker.
int vita_drawrec_on_worker(void);

#ifdef __cplusplus
}

class BaseShape;
class Graphics;
class Matrix4f;
class ShapeDynMaterials;
class TekiMgr;
struct BTeki;

// Lanes
enum PcDrLaneId {
	kDrLanePiki,
	kDrLaneTeki,
	kDrLaneGrass,
	kDrLanePlant,
	kDrLanePikiHead,
	kDrLaneGoal,
	kDrLaneBuilding,
	kDrLaneBomb,
	kDrLanePellet,
	kDrLaneCarcass,
	kDrLaneWork,
	kDrLaneBridge,
	kDrLaneStick,
	kDrLaneUfo,
	kDrLaneCount
};
// Lane bits (pcDrawrecLanes).
enum {
	kDrBitPiki      = 1u << 0,
	kDrBitTeki      = 1u << 1,
	kDrBitTekiLight = 1u << 2,
	kDrBitGrass     = 1u << 3,
	kDrBitPlant     = 1u << 4,
	kDrBitPikiHead  = 1u << 5,
	kDrBitGoal      = 1u << 6,
	kDrBitBuilding  = 1u << 7,
	kDrBitBomb      = 1u << 8,
	kDrBitPellet    = 1u << 9,
	kDrBitCarcass   = 1u << 10,
	kDrBitWork      = 1u << 11,
	kDrBitBridge    = 1u << 12,
	kDrBitStick     = 1u << 13,
	kDrBitUfo       = 1u << 14,
	// S2 lanes
	kDrBitsS2 = kDrBitGrass | kDrBitPlant | kDrBitPikiHead | kDrBitGoal | kDrBitBuilding | kDrBitBomb | kDrBitPellet | kDrBitCarcass
	          | kDrBitWork | kDrBitBridge | kDrBitStick | kDrBitUfo
};
struct PcDrLaneDef {
	// Record body on gW.
	void (*mBody)(Graphics& gW, int idx);
	const char* mName;
};
void pcDrawrecRegisterLane(int lane, const PcDrLaneDef& def);
// Enabled lanes (kDrBit*), 0 when off.
unsigned pcDrawrecLanes(void);

// Price classes
enum { kDrTekiTypes = 35 }; // TEKI_TypeCount
enum {
	kDrPcPiki     = 0,
	kDrPcTekiMtx,       // identity-only teki record
	kDrPcTeki0,         // + teki type
	kDrPcGrass    = kDrPcTeki0 + kDrTekiTypes,
	kDrPcPlant,
	kDrPcPikiHead,
	kDrPcGoal,
	kDrPcBuilding,
	kDrPcBomb,
	kDrPcPellet,
	kDrPcCarcass,
	kDrPcWork,
	kDrPcBridge,
	kDrPcStick,
	kDrPcUfo,
	kDrPcCount
};
// Per-class price (us per record) and largest record size (bytes).
float pcDrawrecPrice(int cls);
unsigned pcDrawrecBytes(int cls);

// Record effects on sealed DGX fields
struct PcDrEffect {
	uint8_t meshes;       // drawMeshes ran
	uint8_t curMtx0;      // mCurrentMatrixId reset to 0
	uint8_t active;       // 0 unchanged, 1 activePtr, 2 &g_pcDrDeadMtx
	uint8_t lightModel;   // calcLighting(v) .. calcLighting(1.0f) pair ran
	const Matrix4f* activePtr;
};

// Write sets
// Values a record's materials write to three DGX fields; mOver past eight.
struct PcDrSet {
	unsigned mV[8];
	int mN;
	bool mOver;
	void add(unsigned x)
	{
		for (int i = 0; i < mN; i++) {
			if (mV[i] == x) {
				return;
			}
		}
		if (mN < 8) {
			mV[mN++] = x;
		} else {
			mOver = true;
		}
	}
	void addAll(const PcDrSet& o)
	{
		for (int i = 0; i < o.mN; i++) {
			add(o.mV[i]);
		}
		if (o.mOver) {
			mOver = true;
		}
	}
};
struct PcDrSets {
	PcDrSet mHasTexGen, mTexMtxBase, mLighting;
	void addAll(const PcDrSets& o)
	{
		mHasTexGen.addAll(o.mHasTexGen);
		mTexMtxBase.addAll(o.mTexMtxBase);
		mLighting.addAll(o.mLighting);
	}
};

// Chain
// Chain mode: 4 threaded, 0 on set-up failure.
int pcDrawrecMode(void);
// Chain fork. Returns gW or null.
Graphics* pcDrawrecForkChain(Graphics& gfx, float expectedUs, unsigned expectedBytes);
// Lane entry. Returns gW or null.
Graphics* pcDrawrecEnter(Graphics& gfx, int lane, const PcDrSets& sets, float expectedUs, unsigned expectedBytes, bool eligible,
                         const char* why);
// Adds shape write sets to the open part.
void pcDrawrecAddSets(const PcDrSets& sets);
// Adds a lighting flag write.
void pcDrawrecAddLightingWrite(unsigned v);
// Open part has room for one more record.
bool pcDrawrecCanPublish(void);
// Queues record idx of lane for the worker.
bool pcDrawrecPublishRec(int lane, int idx, const PcDrEffect& e, int priceClass);
// Part end (idempotent).
void pcDrawrecSeal(void);
// Early chain end: seal and join.
void pcDrawrecBail(void);
// No further part this frame.
void pcDrawrecChainComplete(void);
// Part forked this frame is still open.
bool pcDrawrecPartOpen(void);
// Chain forked and not joined.
bool pcDrawrecChainOpen(void);
// Texture-matrix base candidates of the last sealed part; -1 on overflow.
int pcDrawrecSpliceBases(unsigned* out, int max);

// Shape report
// Per-shape record requirements, cleared at softReset.
struct PcDrShapeInfo {
	const BaseShape* shape;
	uint8_t ok;           // recordable
	uint8_t cachedBlend;  // cached AlphaBlend material
	uint8_t lights;       // a light group
	uint8_t hiddenJoints; // first joint of a mesh hidden
	uint8_t nonPvw;       // a non-PVW material (useTexture)
	uint8_t scaled;       // scaled texture
	uint8_t env;          // an environment map
	const char* why; // first reason ok is 0
	PcDrSets writes; // material write sets
};
const PcDrShapeInfo& pcDrShapeInfo(const BaseShape* s);
// drawshape runs drawMeshes for this shape and mask.
bool pcDrShapeDrawsMeshes(const BaseShape* s, uint32_t mask);

// cacheShape hoist
// Runs cacheShape on the game thread when drawshape would; returns the hoist flag.
bool pcDrHoistCache(Graphics& m, BaseShape* s, ShapeDynMaterials* dm);
// A record of the open chain hoisted a shape.
extern int g_pcDrChainHoisted;

// Ownership stamps (Shape.h pcDrStampCheck)
enum { kDrStampDrawshape = 0, kDrStampAnimate, kDrStampSerial, kDrStampSites };

// Teki lane (src/plugPikiNakata/pcTekiDrawRec.cpp)
// Chain forked at T0.
extern int g_pcDrTekiRec;
// Teki pre-scan, gates and chain fork.
void pcTekiDrawrecBegin(Graphics& gfx, TekiMgr* mgr, int hideType);
// Teki record; false runs B serially.
bool pcTekiDrawrecPublish(BTeki& t, Graphics& gfx);

// Parts
class Creature;
// Forks the next part of the open chain. Returns gW or null.
Graphics* pcDrawrecForkPart(Graphics& gfx, int lane, float expectedUs, unsigned expectedBytes, bool refork, bool* retry);
// Ends the open part before a serial object.
void pcDrawrecPartBreak(int objType);
// Refork threshold and minimum chain-fork B, in us.
float pcDrawrecReforkUs(void);
float pcDrawrecMinB(void);

// S2 driver (src/plugPikiKando/pcItemDrawRec.cpp)
// Set between pcDrS2Begin and the S2 seal.
extern int g_pcDrS2Live;
// Object being recorded by pcDrObj.
extern Creature* g_pcDrRecObj;
// S2 begin, plant gate, end.
void pcDrS2Begin(Graphics& gfx, int hideType);
void pcDrS2PlantGate(void);
void pcDrS2End(void);
// Resets the S2 expected-B tail.
void pcDrS2Reset(void);
void pcDrObj(Creature* obj, Graphics& gfx);
// Opens a part for a record-class object; false runs it serially.
bool pcDrS2EnsurePart(Graphics& gfx, int lane);
// Deducts a published record's price class.
void pcDrS2Spent(int priceClass);
#define PC_DR_REFRESH(obj, gfx) (__builtin_expect(g_pcDrS2Live, 0) ? pcDrObj((obj), (gfx)) : (obj)->refresh(gfx))

// S2 lanes (pcItemDrawRec.cpp)
class ItemCreature;
// GrassGen or RockGen record.
bool pcDrGrassRecord(ItemCreature& gen, bool rock, Graphics& gfx);
// Sprout record.
struct PikiHeadItem;
bool pcDrPikiHeadBegin(PikiHeadItem& item, Graphics& gfx);
void pcDrPikiHeadPublish(PikiHeadItem& item, Graphics& gfx, bool onCamera);
// Posed-item record (goal, building, bomb, pellet, carcass, work, bridge, stick).
class Creature;
class Shape;
struct DynCollShape;
struct PcDrItemRec {
	Creature* obj;
	Shape* shape;
	ShapeDynMaterials* dyn;     // drawshape dyn
	ShapeDynMaterials* animDyn; // animate dyn
	DynCollShape* build;        // joint visibility
	float val;                  // animate value
	int lane, priceClass;
	bool animVal, light, ident, draw, stamp;
};
bool pcDrItemBegin(Creature& obj, Shape* shape, Graphics& gfx);
void pcDrItemRecord(const PcDrItemRec& r, Graphics& gfx);
// Stand-in for a record's dead local active matrix.
extern Matrix4f g_pcDrDeadMtx;
#endif
