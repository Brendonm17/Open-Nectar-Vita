#include "ObjectMgr.h"
#include "AIPerf.h"
#include "Creature.h"
#include "CreatureNode.h"
#include "DebugLog.h"
#include "sysNew.h"
#if defined(__vita__)
#include "gl/vita_drawrec.h"
#include "SearchSystem.h"
#include <cfloat>
#include <cstring>
#endif

/**
 * @todo: Documentation
 * @note UNUSED Size: 00009C
 */
DEFINE_ERROR(13)

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000F4
 */
DEFINE_PRINT("objectMgr");

/**
 * @todo: Documentation
 * @note UNUSED Size: 000080 (Matching by size)
 */
static f32 boundSphereDist(Creature* a, Creature* b)
{
	f32 dist = centreDist(a, b);
	return dist - (a->getBoundingSphereRadius() + b->getBoundingSphereRadius());
}

#if defined(PIKI_PC_PORT)
#include <algorithm>
#include <cstdio>
#include <vector>

#if defined(__vita__)
// Search snapshot
struct PcSearchSnap {
	static void planMono(MonoObjectMgr* self, ObjectMgr* other);
	static void planPoly(PolyObjectMgr* self, ObjectMgr* other);
	static void planObj(ObjectMgr* self, ObjectMgr* other);
	static int walkSet(int set);
};
static bool sPcSearchPlanning = false;
#endif

namespace {
struct SearchCand {
	Creature* obj;
	f32 radius;
	bool skip;        // not piki/navi, AI-culled and not cullable
	bool updatable;   // mSearchContext.updatable() at gather time
};
inline void candFrom(Creature* o, SearchCand& c)
{
	c.obj                = o;
	c.radius             = o->getBoundingSphereRadius();
	const bool isPlayer  = o->mObjType == OBJTYPE_Piki || o->mObjType == OBJTYPE_Navi;
	c.skip               = !isPlayer && o->mGrid.aiCulling() && !o->aiCullable();
	c.updatable          = o->mSearchContext.updatable();
}
std::vector<SearchCand> sCandOther, sCandSelf;
void gatherIter(ObjectMgr* mgr, std::vector<SearchCand>& out)
{
	out.clear();
	Iterator it(mgr);
	CI_LOOP(it)
	{
		SearchCand c;
		candFrom(*it, c);
		out.push_back(c);
	}
}

//
// Neighbour index for the pair loops
constexpr f32 kGridR0 = 48.0f;
struct GridEntry {
	s16 z, x;
	int idx;
};
std::vector<GridEntry> sGridSmall;
std::vector<int> sGridLarge;
std::vector<int> sGridHits;

inline bool gridLess(const GridEntry& a, const GridEntry& b)
{
	if (a.z != b.z) return a.z < b.z;
	if (a.x != b.x) return a.x < b.x;
	return a.idx < b.idx;
}

inline bool gridBegin(const std::vector<SearchCand>& cands)
{
	if (!AIPerf::useGrid) {
		return false;
	}
	sGridSmall.clear();
	sGridLarge.clear();
	for (int i = 0; i < int(cands.size()); i++) {
		const SearchCand& c = cands[i];
		if (!(c.radius <= kGridR0)) {   // also catches NaN
			sGridLarge.push_back(i);
			continue;
		}
		GridEntry e;
		e.z   = c.obj->mGrid.mGridPositionZ;
		e.x   = c.obj->mGrid.mGridPositionX;
		e.idx = i;
		sGridSmall.push_back(e);
	}
	std::sort(sGridSmall.begin(), sGridSmall.end(), gridLess);
	return true;
}

const std::vector<int>& gridQuery(const std::vector<SearchCand>& cands, FastGrid& g, f32 rA)
{
	sGridHits.clear();
	if (!(rA >= 0.0f && rA < 4096.0f)) {
		for (int i = 0; i < int(cands.size()); i++) {
			sGridHits.push_back(i);
		}
		return sGridHits;
	}
	const int L  = int(s16(rA + kGridR0) >> AIPerf::gridShift) + 1;
	const int cx = g.mGridPositionX;
	const int cz = g.mGridPositionZ;
	for (int z = cz - L; z <= cz + L; z++) {
		GridEntry lo;
		lo.z   = s16(z);
		lo.x   = s16(cx - L);
		lo.idx = -2147483647 - 1;
		for (std::vector<GridEntry>::const_iterator it = std::lower_bound(sGridSmall.begin(), sGridSmall.end(), lo, gridLess);
		     it != sGridSmall.end() && int(it->z) == z && int(it->x) <= cx + L; ++it) {
			sGridHits.push_back(it->idx);
		}
	}
	for (size_t k = 0; k < sGridLarge.size(); k++) {
		sGridHits.push_back(sGridLarge[k]);
	}
	std::sort(sGridHits.begin(), sGridHits.end());
	return sGridHits;
}
} // namespace
#endif


/**
 * @todo: Documentation
 */
void ObjectMgr::stickUpdate()
{
	Iterator iter(this);
	CI_LOOP(iter)
	{
		(*iter)->stickUpdate();
	}
}

/**
 * @todo: Documentation
 */
void ObjectMgr::invalidateSearch()
{
	Iterator iter(this);
	CI_LOOP(iter)
	{
		Creature* c = *iter;
		if (c->mSearchContext.updatable()) {
			c->mSearchBuffer.invalidate();
		}
	}
}

/**
 * @todo: Documentation
 */
void ObjectMgr::update()
{
	Iterator iter(this);
	CI_LOOP(iter)
	{
		(*iter)->update();
	}
}

/**
 * @todo: Documentation
 */
void ObjectMgr::store()
{
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000004 (Matching by size)
 */
void ObjectMgr::restore()
{
}

/**
 * @todo: Documentation
 */
void ObjectMgr::killAll()
{
	Iterator it(this);
	CI_LOOP(it)
	{
		Creature* obj = *it;
		if (obj->isAlive()) {
			obj->kill(true);
			obj->exitCourse();
			it.dec();
		}
	}

	// Doing it once wasnt good enough so we do it all again
	CI_LOOP(it)
	{
		Creature* obj = *it;
		if (obj->isAlive()) {
			obj->kill(true);
			obj->exitCourse();
			ERROR("xamasita");
			it.dec();
		}
	}
}

/**
 * @todo: Documentation
 */
void ObjectMgr::postUpdate(int unused, f32 deltaTime)
{
	Iterator it(this);
	CI_LOOP(it)
	{
		Creature* obj = *it;
		obj->postUpdate(unused, deltaTime);
		if (!AIPerf::insQuick && !AIPerf::updateSearchBuffer) {
			if (obj->mSearchBuffer.available()) {
				obj->mSearchBuffer.clear();
			}
		}
	}
}

void ObjectMgr::refresh(Graphics& gfx)
{
	Iterator it(this);
	CI_LOOP(it)
	{
		Creature* obj = *it;
#if defined(__vita__)
		PC_DR_REFRESH(obj, gfx);
#else
		obj->refresh(gfx);
#endif
	}
}

Creature* ObjectMgr::findClosest(immut Vector3f& pos, immut Condition* cond)
{
	Creature* ret = nullptr;
	f32 maxDist   = 12800.0f;
	FastGrid grid;
	grid.updateGrid(pos);
	Iterator it(this);
	CI_LOOP(it)
	{
		Creature* obj = *it;
		if (!AIPerf::iteratorCull || !obj->mGrid.doCulling(grid, maxDist)) {
			if (!cond || cond->satisfy(obj)) {
				Vector3f diff = obj->getCentre() - pos;
				f32 len       = diff.length();
				if (len < maxDist) {
					maxDist = len;
					ret     = obj;
				}
			}
		}
	}
	return ret;
}

/**
 * @todo: Documentation
 */
Creature* ObjectMgr::findClosest(immut Vector3f& pos, f32 maxLen, immut Condition* cond)
{
	Creature* ret = nullptr;
	f32 maxDist   = 12800.0f;
	FastGrid grid;
	grid.updateGrid(pos);
	Iterator it(this);
	CI_LOOP(it)
	{
		Creature* obj = *it;
		if (!AIPerf::iteratorCull || !obj->mGrid.doCulling(grid, maxDist)) {
			if (!cond || cond->satisfy(obj)) {
				Vector3f diff = obj->getCentre() - pos;
				f32 len       = diff.length();
				if (len <= maxLen && len < maxDist) {
					maxDist = len;
					ret     = obj;
				}
			}
		}
	}
	return ret;
}

/**
 * @todo: Documentation
 */
void ObjectMgr::drawShadow(Graphics& gfx, Texture* tex)
{
	Iterator it(this);
	CI_LOOP(it)
	{
		Creature* obj = *it;
		if (obj->aiCullable())
			obj->drawShadow(gfx);
	}
}

/**
 * @todo: Documentation
 */
MonoObjectMgr::MonoObjectMgr()
{
	mObjectList  = nullptr;
	mMaxElements = 0;
	mNumObjects  = 0;
	_38          = 0;
}

/**
 * @todo: Documentation
 */
void MonoObjectMgr::create(int num)
{
	mMaxElements = num;
	mObjectList  = new Creature*[mMaxElements];
	mEntryStatus = new int[mMaxElements];

	for (int i = 0; i < mMaxElements; i++) {
		mObjectList[i]  = createObject();
		mEntryStatus[i] = -1;
	}
	mNumObjects = 0;
}

/**
 * @todo: Documentation
 */
void MonoObjectMgr::postUpdate(int unused, f32 deltaTime)
{
	for (int i = 0; i < mMaxElements; i++) {
		if (mEntryStatus[i] == 0) {
			Creature* obj = mObjectList[i];
			obj->postUpdate(unused, deltaTime);
			if (!AIPerf::insQuick && !AIPerf::updateSearchBuffer) {
				if (obj->mSearchBuffer.available()) {
					obj->mSearchBuffer.clear();
				}
			}
		}
	}
}

/**
 * @todo: Documentation
 */
void MonoObjectMgr::drawShadow(Graphics& gfx, Texture*)
{
	for (int i = 0; i < mMaxElements; i++) {
		if (mEntryStatus[i] == 0) {
			Creature* obj = mObjectList[i];
			if (obj->aiCullable())
				obj->drawShadow(gfx);
		}
	}

	STACK_PAD_VAR(2);
}

/**
 * @todo: Documentation
 */
void MonoObjectMgr::refresh(Graphics& gfx)
{
	for (int i = 0; i < mMaxElements; i++) {
		if (mEntryStatus[i] == 0) {
			Creature* obj = mObjectList[i];
#if defined(__vita__)
			PC_DR_REFRESH(obj, gfx);
#else
			obj->refresh(gfx);
#endif
		}
	}
}

/**
 * @todo: Documentation
 */
void MonoObjectMgr::update()
{
	for (int i = 0; i < mMaxElements; i++) {
		if (mEntryStatus[i] == 0) {
			mObjectList[i]->update();
		}

		if (mEntryStatus[i] == -2 && mObjectList[i]->getCnt() == 0) {
			kill(mObjectList[i]);
			mEntryStatus[i] = -1;
		} else if (mEntryStatus[i] == -2) {
			PRINT(" objetcts[%d] : %s cannot die ! %d cnt\n", i, ObjType::getName(mObjectList[i]->mObjType), mObjectList[i]->getCnt());
			mObjectList[i]->clearCnt();
		}
	}
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000044
 */
int MonoObjectMgr::getEmptyIndex()
{
	for (int i = 0; i < mMaxElements; i++) {
		if (mEntryStatus[i] == -1) {
			return i;
		}
	}
	return -1;
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000044
 */
int MonoObjectMgr::getIndex(Creature* obj)
{
	for (int i = 0; i < mMaxElements; i++) {
		if (mObjectList[i] == obj) {
			return i;
		}
	}
	return -1;
}

/**
 * @todo: Documentation
 */
Creature* MonoObjectMgr::birth()
{
	if (mNumObjects >= mMaxElements) {
		PRINT("numObjects(%d) exceeds maxObjects(%d)\n", mNumObjects, mMaxElements);
		for (int i = 0; i < mMaxElements; i++) {
			if (mEntryStatus[i] == -2) {
				PRINT("*** (%d) WANT TO DIE : %s (%d refs)\n", i, mObjectList[i]->removable() ? "removable" : "not removable",
				      mObjectList[i]->getCnt());
			}
		}
		return nullptr;
	}

	int id = getEmptyIndex();
	if (id == -1) {
		PRINT("no empty slot !\n");
		return nullptr;
	}

	mEntryStatus[id] = 0;
	mNumObjects++;
	return mObjectList[id];
}

/**
 * @todo: Documentation
 */
void MonoObjectMgr::kill(Creature* obj)
{
	int id = getIndex(obj);
	if (id != -1) {
		if (!obj->removable()) {
			mEntryStatus[id] = -2;
		} else {
			mNumObjects--;
			mEntryStatus[id] = -1;
		}
	}
}

/**
 * @todo: Documentation
 */
Creature* MonoObjectMgr::getCreature(int id)
{
	if (id < 0 || id >= mMaxElements || mEntryStatus[id] != 0) {
		return nullptr;
	}
	return mObjectList[id];
}

/**
 * @todo: Documentation
 */
int MonoObjectMgr::getFirst()
{
	int ret = -1;
	for (int i = 0; i < mMaxElements; i++) {
		if (mEntryStatus[i] == 0) {
			ret = i;
			break;
		}
	}
	if (ret == -1) {
		return mMaxElements;
	}
	return ret;
}

/**
 * @todo: Documentation
 */
int MonoObjectMgr::getNext(int id)
{
	int ret = -1;
	for (int i = id + 1; i < mMaxElements; i++) {
		if (mEntryStatus[i] == 0) {
			ret = i;
			break;
		}
	}
	if (ret == -1) {
		return mMaxElements;
	}
	return ret;
}

/**
 * @todo: Documentation
 */
bool MonoObjectMgr::isDone(int index)
{
	if (index >= mMaxElements) {
		return true;
	}

	return false;
}

/**
 * @todo: Documentation
 */
void MonoObjectMgr::search(ObjectMgr* mgr)
{
    PikiPcPersistentAlloc pcKeep;
#if defined(__vita__)
	if (sPcSearchPlanning) { PcSearchSnap::planMono(this, mgr); return; } // the snapshot's plan pass
#endif
#if defined(PIKI_PC_PORT)
	if (mgr == this) {
		searchSelf();
		return;
	}
	gatherIter(mgr, sCandOther);
	const bool indexed = gridBegin(sCandOther);
	for (int i = 0; i < mMaxElements; i++) {
		if (mEntryStatus[i] != 0) {
			continue;
		}
		Creature* obj = mObjectList[i];
		if (AIPerf::insQuick && !obj->mSearchContext.updatable()) {
			continue;
		}
		bool isPlayer = false;
		if (obj->mObjType == OBJTYPE_Piki || obj->mObjType == OBJTYPE_Navi) {
			isPlayer = true;
		}
		if (!isPlayer && obj->mGrid.aiCulling() && !obj->aiCullable()) {
			continue;
		}
		const f32 rA   = obj->getBoundingSphereRadius();
		const bool upA = obj->mSearchContext.updatable();
		auto visit     = [&](const SearchCand& c) {
			Creature* obj2 = c.obj;
			if (AIPerf::useUpdateMgr) {
				if (!upA && !c.updatable) {
					return;
				}
			}
			if (c.skip) {
				return;
			}
			if (AIPerf::useGrid) {
				if (obj->mGrid.doCulling(obj2->mGrid, rA + c.radius)) {
					return;
				}
			}
			f32 dist = centreDist(obj, obj2) - (rA + c.radius);
			if (dist <= 300.0f && obj->isAlive() && obj->mSearchBuffer.available()) {
				if (!AIPerf::useUpdateMgr || upA) {
					obj->mSearchBuffer.insert(obj2, dist);
				}
			}
			if (dist <= 300.0f && obj2->isAlive() && obj2->mSearchBuffer.available()) {
				if (!AIPerf::useUpdateMgr || c.updatable) {
					obj2->mSearchBuffer.insert(obj, dist);
				}
			}
		};
		if (indexed) {
			const std::vector<int>& hits = gridQuery(sCandOther, obj->mGrid, rA);
			for (size_t k = 0; k < hits.size(); k++) {
				visit(sCandOther[hits[k]]);
			}
		} else {
			for (const SearchCand& c : sCandOther) {
				visit(c);
			}
		}
	}
#else
	if (mgr == this) {
		searchSelf();
		return;
	}

	for (int i = 0; i < mMaxElements; i++) {
		if (mEntryStatus[i] != 0) {
			continue;
		}

		Creature* obj = mObjectList[i];
		if (AIPerf::insQuick && !obj->mSearchContext.updatable()) {
			continue;
		}

		bool isPlayer = false;
		if (obj->mObjType == OBJTYPE_Piki || obj->mObjType == OBJTYPE_Navi) {
			isPlayer = true;
		}
		if (!isPlayer && obj->mGrid.aiCulling() && !obj->aiCullable()) {
			continue;
		}

		Iterator it(mgr);
		CI_LOOP(it)
		{
			Creature* obj2 = *it;
			if (AIPerf::useUpdateMgr) {
				if (!obj->mSearchContext.updatable() && !obj2->mSearchContext.updatable()) {
					continue;
				}
			}

			bool isPlayer = false;
			if (obj2->mObjType == OBJTYPE_Piki || obj2->mObjType == OBJTYPE_Navi) {
				isPlayer = true;
			}
			if (!isPlayer && obj2->mGrid.aiCulling() && !obj2->aiCullable()) {
				continue;
			}
			if (AIPerf::useGrid) {
				if (obj->mGrid.doCulling(obj2->mGrid, obj->getBoundingSphereRadius() + obj2->getBoundingSphereRadius())) {
					continue;
				}
			}
			f32 dist = boundSphereDist(obj, obj2);

			if (dist <= 300.0f && obj->isAlive() && obj->mSearchBuffer.available()) {
				if (!AIPerf::useUpdateMgr || obj->mSearchContext.updatable()) {
					obj->mSearchBuffer.insert(obj2, dist);
				}
			}
			if (dist <= 300.0f && obj2->isAlive() && obj2->mSearchBuffer.available()) {
				if (!AIPerf::useUpdateMgr || obj2->mSearchContext.updatable()) {
					obj2->mSearchBuffer.insert(obj, dist);
				}
			}
		}
	}
#endif
}

/**
 * @todo: Documentation
 */
void MonoObjectMgr::searchSelf()
{
    PikiPcPersistentAlloc pcKeep;
#if defined(PIKI_PC_PORT)
	{
		sCandSelf.clear();
		for (int i = 0; i < mMaxElements; i++) {
			if (mEntryStatus[i] != 0) {
				continue;
			}
			SearchCand c;
			candFrom(mObjectList[i], c);
			sCandSelf.push_back(c);
		}
	}
	bool mabiki        = false;
	const int cnt      = int(sCandSelf.size());
	const bool indexed = gridBegin(sCandSelf);
	for (int ia = 0; ia < cnt; ia++) {
		const SearchCand& ca = sCandSelf[ia];
		Creature* obj        = ca.obj;
		if (obj->mObjType == OBJTYPE_Piki && AIPerf::pikiMabiki) {
			mabiki = true;
		}
		if (ca.skip) {
			continue;
		}
		auto visit = [&](int ib) {
			const SearchCand& cb = sCandSelf[ib];
			Creature* obj2       = cb.obj;
			if (mabiki || AIPerf::useUpdateMgr) {
				if (!ca.updatable && !cb.updatable) {
					return;
				}
			}
			if (cb.skip) {
				return;
			}
			if (AIPerf::useGrid) {
				if (obj->mGrid.doCulling(obj2->mGrid, ca.radius + cb.radius)) {
					return;
				}
			}
			f32 dist = centreDist(obj, obj2) - (ca.radius + cb.radius);
			if (dist <= 300.0f && obj->isAlive()) {
				if (obj->isAlive() && obj->mSearchBuffer.available()) {
					if ((!AIPerf::useUpdateMgr && !mabiki) || ca.updatable) {
						obj->mSearchBuffer.insert(obj2, dist);
					}
				}
				if (obj2->isAlive() && obj2->mSearchBuffer.available()) {
					if ((!AIPerf::useUpdateMgr && !mabiki) || cb.updatable) {
						obj2->mSearchBuffer.insert(obj, dist);
					}
				}
			}
		};
		if (indexed) {
			const std::vector<int>& hits = gridQuery(sCandSelf, obj->mGrid, ca.radius);
			for (size_t k = 0; k < hits.size(); k++) {
				if (hits[k] > ia) {
					visit(hits[k]);
				}
			}
		} else {
			for (int ib = ia + 1; ib < cnt; ib++) {
				visit(ib);
			}
		}
	}
#else
	bool mabiki = false;
	for (int i = 0; i < mMaxElements - 1; i++) {
		if (mEntryStatus[i] != 0) {
			continue;
		}

		Creature* obj = mObjectList[i];

		bool isPlayer = false;
		if (obj->mObjType == OBJTYPE_Piki || obj->mObjType == OBJTYPE_Navi) {
			isPlayer = true;
			if (AIPerf::pikiMabiki && obj->mObjType == OBJTYPE_Piki) {
				mabiki = true;
			}
		}
		if (!isPlayer && obj->mGrid.aiCulling() && !obj->aiCullable()) {
			continue;
		}

		for (int j = i + 1; j < mMaxElements; j++) {
			if (mEntryStatus[j] != 0) {
				continue;
			}
			Creature* obj2 = mObjectList[j];

			if (mabiki || AIPerf::useUpdateMgr) {
				if (!obj->mSearchContext.updatable() && !obj2->mSearchContext.updatable()) {
					continue;
				}
			}

			bool isPlayer = false;
			if (obj2->mObjType == OBJTYPE_Piki || obj2->mObjType == OBJTYPE_Navi) {
				isPlayer = true;
			}
			if (!isPlayer && obj2->mGrid.aiCulling() && !obj2->aiCullable()) {
				continue;
			}
			if (AIPerf::useGrid) {
				if (obj->mGrid.doCulling(obj2->mGrid, obj->getBoundingSphereRadius() + obj2->getBoundingSphereRadius())) {
					continue;
				}
			}
			f32 dist = boundSphereDist(obj, obj2);

			if (dist <= 300.0f && obj->isAlive()) {
				if (obj->isAlive() && obj->mSearchBuffer.available()) {
					if ((!AIPerf::useUpdateMgr && !mabiki) || obj->mSearchContext.updatable()) {
						obj->mSearchBuffer.insert(obj2, dist);
					}
				}
				if (obj2->isAlive() && obj2->mSearchBuffer.available()) {
					if ((!AIPerf::useUpdateMgr && !mabiki) || obj2->mSearchContext.updatable()) {
						obj2->mSearchBuffer.insert(obj, dist);
					}
				}
			}
		}
	}
#endif
}

/**
 * @todo: Documentation
 */
PolyObjectMgr::PolyObjectMgr(int num)
{
	mObjectPool     = 0;
	mEntryCount     = 0;
	mMaxClassLength = num;
	mEntries        = new PolyData[num];

	for (int i = 0; i < num; i++) {
		mEntries[i].mClassObj  = 0;
		mEntries[i].mClassSize = 0;
		mEntries[i].mClassId   = -1;
	}
}

/**
 * @todo: Documentation
 */
void PolyObjectMgr::create(int num)
{
	mActiveObjects = 0;
	mPoolCapacity  = num;
	mObjectIndices = new int[mPoolCapacity];
	for (int i = 0; i < mPoolCapacity; i++) {
		mObjectIndices[i] = -1;
	}
	mMaxSize = 0;
}

/**
 * @todo: Documentation
 */
void PolyObjectMgr::update()
{
	for (int i = 0; i < mPoolCapacity; i++) {

		if (mObjectIndices[i] >= 0) {
			Creature* obj = get(i);
			if (obj) {
				obj->update();
			}
		}

		if (mObjectIndices[i] == -2 && get(i)->removable()) {
			PRINT_KANDO("killing ... %x\n", get(i));
			kill(get(i));
		} else if (mObjectIndices[i] == -2) {
			get(i)->clearCnt();
		}
	}
}

/**
 * @todo: Documentation
 */
void PolyObjectMgr::postUpdate(int unused, f32 deltaTime)
{
	for (int i = 0; i < mPoolCapacity; i++) {
		if (mObjectIndices[i] >= 0) {
			Creature* obj = get(i);
			if (obj) {
				obj->postUpdate(unused, deltaTime);
			}
		}
	}
}

/**
 * @todo: Documentation
 */
void PolyObjectMgr::refresh(Graphics& gfx)
{
	for (int i = 0; i < mPoolCapacity; i++) {
		if (mObjectIndices[i] >= 0) {
			Creature* obj = get(i);
			if (obj) {
#if defined(__vita__)
				PC_DR_REFRESH(obj, gfx);
#else
				obj->refresh(gfx);
#endif
			}
		}
	}
}

/**
 * @todo: Documentation
 */
void PolyObjectMgr::drawShadow(Graphics& gfx, Texture*)
{
	for (int i = 0; i < mPoolCapacity; i++) {
		if (mObjectIndices[i] >= 0) {
			Creature* obj = get(i);
			if (obj && obj->aiCullable()) {
				obj->drawShadow(gfx);
			}
		}
	}
}

/**
 * @todo: Documentation
 */
void PolyObjectMgr::beginRegister()
{
}

/**
 * @todo: Documentation
 */
void PolyObjectMgr::registerClass(int id, Creature* obj, int size)
{
	for (int i = 0; i < mMaxClassLength; i++) {
		if (id == mEntries[i].mClassId) {
			PRINT("registerClass::id %d duplicates\n", id);
			ERROR("hello mck\n");
			return;
		}
	}

	if (mMaxSize < size) {
		mMaxSize = size;
	}

	mEntries[mEntryCount].mClassObj  = obj;
	mEntries[mEntryCount].mClassSize = size;
	mEntries[mEntryCount].mClassId   = id;
	mEntryCount++;
}

/**
 * @todo: Documentation
 */
void PolyObjectMgr::endRegister()
{
	immut size_t poolSize = static_cast<size_t>(mPoolCapacity) * static_cast<size_t>(mMaxSize);
	mObjectPool           = new u8[poolSize];
	for (size_t i = 0; i < poolSize; i++) {
		mObjectPool[i] = -1;
	}
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000044
 */
int PolyObjectMgr::getEmptyIndex()
{
	for (int i = 0; i < mPoolCapacity; i++) {
		if (mObjectIndices[i] == -1) {
			return i;
		}
	}

	return -1;
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000080
 */
int PolyObjectMgr::getIndex(Creature* obj)
{
	int ret = -1;
	for (int i = 0; i < mPoolCapacity; i++) {
		if (get(i) == obj) {
			ret = i;
			break;
		}
	}
	return ret;
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000048
 */
int PolyObjectMgr::getTemplateIndex(int id)
{
	for (int i = 0; i < mMaxClassLength; i++) {
		if (mEntries[i].mClassId == id) {
			return i;
		}
	}
	return -1;
}

/**
 * @todo: Documentation
 */
Creature* PolyObjectMgr::get(int i)
{
	if (!mObjectPool || i < 0 || i >= mPoolCapacity) {
		return nullptr;
	}

	return reinterpret_cast<Creature*>(mObjectPool + static_cast<size_t>(mMaxSize) * static_cast<size_t>(i));
}

/**
 * @todo: Documentation
 */
Creature* PolyObjectMgr::birth(int id)
{
	if (mActiveObjects >= mPoolCapacity) {
		return nullptr;
	}

	int templateId = getTemplateIndex(id);
	if (templateId == -1) {
		PRINT("<PolyObjectMgr::birth> id %d is invalid\n", id);
		return nullptr;
	}

	int emptyId = getEmptyIndex();
	if (emptyId == -1) {
		PRINT("<PolyObjectMgr> failed to give birth new object\n");
		return nullptr;
	}

	u8 *templateData, *newObject;
	PolyData* newTemplate = &mEntries[templateId];

	newObject               = (u8*)get(emptyId);
	mObjectIndices[emptyId] = id;
	templateData            = (u8*)newTemplate->mClassObj;
	for (int i = 0; i < newTemplate->mClassSize; i++) {
		newObject[i] = templateData[i];
	}

	mActiveObjects++;

	return (Creature*)newObject;
}

/**
 * @todo: Documentation
 */
void PolyObjectMgr::kill(Creature* obj)
{
	int id = getIndex(obj);
	if (obj->removable()) {
		PRINT_KANDO("%x really dead (index %d) \n", obj, id);
		mObjectIndices[id] = -1;
		mActiveObjects--;
	} else {
		mObjectIndices[id] = -2;
	}
}

/**
 * @todo: Documentation
 */
Creature* PolyObjectMgr::getCreature(int id)
{
	if (id < 0 || id >= mPoolCapacity || mObjectIndices[id] < 0) {
		return nullptr;
	}
	return get(id);
}

/**
 * @todo: Documentation
 */
int PolyObjectMgr::getFirst()
{
	int next = -1;
	for (int i = 0; i < mPoolCapacity; i++) {
		int candidate = mObjectIndices[i];
		if (mObjectIndices[i] >= 0) {
			next = i;
			break;
		}
	}
	if (next == -1) {
		return mPoolCapacity;
	}
	return next;
}

/**
 * @todo: Documentation
 */
int PolyObjectMgr::getNext(int idx)
{
	int next = -1;
	for (int i = idx + 1; i < mPoolCapacity; i++) {
		int candidate = mObjectIndices[i];
		if (mObjectIndices[i] >= 0) {
			next = i;
			break;
		}
	}
	if (next == -1) {
		return mPoolCapacity;
	}
	return next;
}

/**
 * @todo: Documentation
 */
bool PolyObjectMgr::isDone(int idx)
{
	if (idx >= mPoolCapacity) {
		return true;
	}
	return false;
}

/**
 * @todo: Documentation
 */
void PolyObjectMgr::search(ObjectMgr* mgr)
{
#if defined(__vita__)
	if (sPcSearchPlanning) { PcSearchSnap::planPoly(this, mgr); return; } // the snapshot's plan pass
#endif
	if (mgr == this) {
		searchSelf();
	}
}

/**
 * @todo: Documentation
 */
void PolyObjectMgr::searchSelf()
{
    PikiPcPersistentAlloc pcKeep;
#if defined(PIKI_PC_PORT)
	{
		sCandSelf.clear();
		for (int i = 0; i < mPoolCapacity; i++) {
			if (mObjectIndices[i] < 0) {
				continue;
			}
			SearchCand c;
			candFrom(get(i), c);
			sCandSelf.push_back(c);
		}
	}
	const int cnt      = int(sCandSelf.size());
	const bool indexed = gridBegin(sCandSelf);
	for (int ia = 0; ia < cnt; ia++) {
		const SearchCand& ca = sCandSelf[ia];
		Creature* obj        = ca.obj;
		if (ca.skip) {
			continue;
		}
		auto visit = [&](int ib) {
			const SearchCand& cb = sCandSelf[ib];
			Creature* obj2       = cb.obj;
			if (AIPerf::useUpdateMgr) {
				if (!ca.updatable && !cb.updatable) {
					return;
				}
			}
			if (cb.skip) {
				return;
			}
			if (AIPerf::useGrid) {
				if (obj->mGrid.doCulling(obj2->mGrid, ca.radius + cb.radius)) {
					return;
				}
			}
			f32 dist = centreDist(obj, obj2) - (ca.radius + cb.radius);
			if (dist <= 300.0f) {
				if (obj->isAlive() && obj->mSearchBuffer.available()) {
					if ((!AIPerf::useUpdateMgr) || ca.updatable) {
						obj->mSearchBuffer.insert(obj2, dist);
					}
				}
				if (obj2->isAlive() && obj2->mSearchBuffer.available()) {
					if ((!AIPerf::useUpdateMgr) || cb.updatable) {
						obj2->mSearchBuffer.insert(obj, dist);
					}
				}
			}
		};
		if (indexed) {
			const std::vector<int>& hits = gridQuery(sCandSelf, obj->mGrid, ca.radius);
			for (size_t k = 0; k < hits.size(); k++) {
				if (hits[k] > ia) {
					visit(hits[k]);
				}
			}
		} else {
			for (int ib = ia + 1; ib < cnt; ib++) {
				visit(ib);
			}
		}
	}
#else
	for (int i = 0; i < mPoolCapacity - 1; i++) {
		if (mObjectIndices[i] < 0) {
			continue;
		}

		Creature* obj = get(i);

		bool isPlayer = false;
		if (obj->mObjType == OBJTYPE_Piki || obj->mObjType == OBJTYPE_Navi) {
			isPlayer = true;
		}
		if (!isPlayer && obj->mGrid.aiCulling() && !obj->aiCullable()) {
			continue;
		}

		for (int j = i + 1; j < mPoolCapacity; j++) {
			if (mObjectIndices[j] < 0) {
				continue;
			}
			Creature* obj2 = get(j);

			if (AIPerf::useUpdateMgr) {
				if (!obj->mSearchContext.updatable() && !obj2->mSearchContext.updatable()) {
					continue;
				}
			}

			bool isPlayer = false;
			if (obj2->mObjType == OBJTYPE_Piki || obj2->mObjType == OBJTYPE_Navi) {
				isPlayer = true;
			}
			if (!isPlayer && obj2->mGrid.aiCulling() && !obj2->aiCullable()) {
				continue;
			}
			if (AIPerf::useGrid) {
				if (obj->mGrid.doCulling(obj2->mGrid, obj->getBoundingSphereRadius() + obj2->getBoundingSphereRadius())) {
					continue;
				}
			}
			f32 dist = boundSphereDist(obj, obj2);

			if (dist <= 300.0f) {
				if (obj->isAlive() && obj->mSearchBuffer.available()) {
					if ((!AIPerf::useUpdateMgr) || obj->mSearchContext.updatable()) {
						obj->mSearchBuffer.insert(obj2, dist);
					}
				}
				if (obj2->isAlive() && obj2->mSearchBuffer.available()) {
					if ((!AIPerf::useUpdateMgr) || obj2->mSearchContext.updatable()) {
						obj2->mSearchBuffer.insert(obj, dist);
					}
				}
			}
		}
	}
#endif
}

/**
 * Searches for nearby objects and populates each object's search buffer with potential interaction targets.
 * Uses spatial partitioning and culling for performance optimization.
 * @param otherMgr The object manager containing potential target objects
 *
 */
void ObjectMgr::search(ObjectMgr* otherMgr)
{
    PikiPcPersistentAlloc pcKeep;
#if defined(__vita__)
	if (sPcSearchPlanning) { PcSearchSnap::planObj(this, otherMgr); return; } // the snapshot's plan pass
#endif
#if defined(PIKI_PC_PORT)
	gatherIter(this, sCandSelf);
	gatherIter(otherMgr, sCandOther);
	const bool indexed = gridBegin(sCandOther);
	for (const SearchCand& ca : sCandSelf) {
		Creature* sourceObj = ca.obj;
		if (ca.skip) {
			continue;
		}
		auto visit = [&](const SearchCand& cb) {
			Creature* targetObj = cb.obj;
			if (cb.skip) {
				return;
			}
			if (AIPerf::useGrid) {
				if (sourceObj->mGrid.doCulling(targetObj->mGrid, ca.radius + cb.radius)) {
					return;
				}
			}
			f32 distance                = centreDist(sourceObj, targetObj) - (ca.radius + cb.radius);
			const f32 INTERACTION_RANGE = 300.0f;
			if (distance <= INTERACTION_RANGE && sourceObj->isAlive() && sourceObj->mSearchBuffer.available()) {
				if (!AIPerf::useUpdateMgr || ca.updatable) {
					sourceObj->mSearchBuffer.insert(targetObj, distance);
				}
			}
			if (distance <= INTERACTION_RANGE && targetObj->isAlive() && targetObj->mSearchBuffer.available()) {
				if (!AIPerf::useUpdateMgr || cb.updatable) {
					targetObj->mSearchBuffer.insert(sourceObj, distance);
				}
			}
		};
		if (indexed) {
			const std::vector<int>& hits = gridQuery(sCandOther, sourceObj->mGrid, ca.radius);
			for (size_t k = 0; k < hits.size(); k++) {
				visit(sCandOther[hits[k]]);
			}
		} else {
			for (const SearchCand& cb : sCandOther) {
				visit(cb);
			}
		}
	}
#else
	Iterator sourceIter(this);
	CI_LOOP(sourceIter)
	{
		Creature* sourceObj = *sourceIter;

		// Skip non-important objects (not Pikmin / Player) unless they have specific flags
		bool skipObj
		    = (sourceObj->mObjType != OBJTYPE_Piki && sourceObj->mObjType != OBJTYPE_Navi) && !sourceObj->isCreatureFlag(CF_AIAlwaysActive);
		if (skipObj && sourceObj->mGrid.aiCulling() && !sourceObj->aiCullable()) {
			continue;
		}

		Iterator it(otherMgr);
		CI_LOOP(it)
		{
			Creature* targetObj = *it;

			skipObj = (targetObj->mObjType != OBJTYPE_Piki && targetObj->mObjType != OBJTYPE_Navi)
			       && !targetObj->isCreatureFlag(CF_AIAlwaysActive);
			if (skipObj && targetObj->mGrid.aiCulling() && !targetObj->aiCullable()) {
				continue;
			}

			// Use spatial grid to quickly cull distant objects
			if (AIPerf::useGrid) {
				if (sourceObj->mGrid.doCulling(targetObj->mGrid,
				                               sourceObj->getBoundingSphereRadius() + targetObj->getBoundingSphereRadius())) {
					continue;
				}
			}

			// Calculate actual distance between objects (accounting for their sizes)
			f32 distance = boundSphereDist(sourceObj, targetObj);

			// Check if objects are within interaction range
			const f32 INTERACTION_RANGE = 300.0f;

			// Add objects to each other's search buffers if they're:
			// 1) Within interaction range
			// 2) Alive
			// 3) Have initialized data lists
			// 4) Pass update manager checks (if enabled)
			if (distance <= INTERACTION_RANGE && sourceObj->isAlive() && sourceObj->mSearchBuffer.available()) {
				// Add target to source's search buffer if conditions met
				if (!AIPerf::useUpdateMgr || sourceObj->mSearchContext.updatable()) {
					sourceObj->mSearchBuffer.insert(targetObj, distance);
				}
			}

			if (distance <= INTERACTION_RANGE && targetObj->isAlive() && targetObj->mSearchBuffer.available()) {
				// Add source to target's search buffer if conditions met
				if (!AIPerf::useUpdateMgr || targetObj->mSearchContext.updatable()) {
					targetObj->mSearchBuffer.insert(sourceObj, distance);
				}
			}
		}
	}
#endif
}

#if defined(__vita__)
#if defined(__ARM_FEATURE_FMA) || !defined(FLT_EVAL_METHOD) || (FLT_EVAL_METHOD != 0)
static const bool kPcSnapFloatExact = false;
#else
static const bool kPcSnapFloatExact = true;
#endif

namespace {

// The plan
enum PcSnapKind {
	kPcSnapMono, // MonoObjectMgr::search
	kPcSnapPoly, // PolyObjectMgr::search to itself
	kPcSnapObj,  // ObjectMgr::search (the bosses)
};
struct PcSnapCall {
	ObjectMgr* mSelf;
	ObjectMgr* mOther;
	int mKind;
	int mSelfSet; // snapshot sets of mSelf and mOther
	int mOtherSet;
};
const int kPcSnapMaxCalls = 48;
PcSnapCall sPcPlan[kPcSnapMaxCalls];
int sPcPlanCount     = 0;
bool sPcPlanOverflow = false;

// The snapshot
struct PcSnapSet {
	ObjectMgr* mMgr;
	MonoObjectMgr* mMono;         // it searched as a MonoObjectMgr
	PolyObjectMgr* mPoly;         // it searched as a PolyObjectMgr
	int mFirst, mCount;           // its entries, sPcEnt[mFirst ..]
	int mSmallFirst, mSmallCount; // small index, sorted by cell then entry
	int mLargeFirst, mLargeCount; // large index
	int mUpdFirst, mUpdCount;     // updatable this frame, ascending (sPcIdxUpd)
};
const int kPcSnapMaxSets = 16;
PcSnapSet sPcSets[kPcSnapMaxSets];
int sPcSetCount = 0;

// One object
struct PcSnapEnt {
	f32 mCx, mCy, mCz; // getCentre()
	f32 mR;            // getBoundingSphereRadius()
	s16 mGx, mGz;      // mGrid cell
	u16 mFlags;
	u16 mPad;
	Creature* mObj;
	SearchBuffer* mBuf; // &mObj->mSearchBuffer
};
static_assert(sizeof(PcSnapEnt) == 32, "one snapshot entry per 32-byte line");
enum {
	kPcEntSkip       = 1 << 0, // candFrom's skip
	kPcEntUpd        = 1 << 1, // mSearchContext.updatable()
	kPcEntPiki       = 1 << 2, // a Pikmin (the Mono self-search's mabiki)
	kPcEntAvail      = 1 << 3, // mSearchBuffer.available()
	kPcEntCentre     = 1 << 4, // mCx/mCy/mCz hold getCentre()
	kPcEntAliveKnown = 1 << 5,
	kPcEntAlive      = 1 << 6, // isAlive()
	kPcEntInsKnown   = 1 << 7,
	kPcEntIns        = 1 << 8, // isVisible() && isAlive()
};
const int kPcSnapMaxEnt = 2048; // all managers together
PcSnapEnt sPcEnt[kPcSnapMaxEnt];
int sPcEntCount = 0;
GridEntry sPcIdxSmall[kPcSnapMaxEnt];
int sPcIdxLarge[kPcSnapMaxEnt];
int sPcIdxUpd[kPcSnapMaxEnt];
int sPcIdxSmallCount = 0, sPcIdxLargeCount = 0, sPcIdxUpdCount = 0;
u32 sPcHitBits[kPcSnapMaxEnt / 32]; // one window query's candidates

// One call's inserts, in order
struct PcSnapEvt {
	SearchBuffer* mBuf;
	Creature* mTgt;
	f32 mDist;
};
const int kPcSnapMaxEvt = 8192;
PcSnapEvt sPcEvt[kPcSnapMaxEvt];
int sPcEvtCount = 0;

enum { kPcFbOff, kPcFbConfig, kPcFbOverflow, kPcFbNull };

int pcSnapGate()
{
	if (!kPcSnapFloatExact) {
		return kPcFbOff;
	}
	if (!AIPerf::insQuick || AIPerf::useGrid != 2 || AIPerf::useUpdateMgr || AIPerf::loopOptimise || !AIPerf::pikiMabiki) {
		return kPcFbConfig;
	}
	return -1;
}

// The set holding mgr, added if new
int pcSnapSetOf(ObjectMgr* mgr)
{
	for (int i = 0; i < sPcSetCount; i++) {
		if (sPcSets[i].mMgr == mgr) {
			return i;
		}
	}
	if (sPcSetCount >= kPcSnapMaxSets) {
		return -1;
	}
	PcSnapSet& set = sPcSets[sPcSetCount];
	memset(&set, 0, sizeof(set));
	set.mMgr = mgr;
	return sPcSetCount++;
}

void pcSnapNote(int kind, ObjectMgr* self, ObjectMgr* other, MonoObjectMgr* mono, PolyObjectMgr* poly)
{
	if (sPcPlanCount >= kPcSnapMaxCalls || !self || !other) {
		sPcPlanOverflow = true;
		return;
	}
	const int a = pcSnapSetOf(self);
	const int b = (other == self) ? a : pcSnapSetOf(other);
	if (a < 0 || b < 0) {
		sPcPlanOverflow = true;
		return;
	}
	if (mono) {
		sPcSets[a].mMono = mono;
	}
	if (poly) {
		sPcSets[a].mPoly = poly;
	}
	PcSnapCall& call = sPcPlan[sPcPlanCount++];
	call.mSelf       = self;
	call.mOther      = other;
	call.mKind       = kind;
	call.mSelfSet    = a;
	call.mOtherSet   = b;
}

inline void pcSnapFill(PcSnapEnt& e, Creature* obj)
{
	e.mObj              = obj;
	e.mBuf              = &obj->mSearchBuffer;
	e.mR                = obj->getBoundingSphereRadius();
	const bool isPlayer = obj->mObjType == OBJTYPE_Piki || obj->mObjType == OBJTYPE_Navi;
	u16 flags           = 0;
	if (!isPlayer && obj->mGrid.aiCulling() && !obj->aiCullable()) {
		flags |= kPcEntSkip;
	}
	if (obj->mSearchContext.updatable()) {
		flags |= kPcEntUpd;
	}
	if (obj->mObjType == OBJTYPE_Piki) {
		flags |= kPcEntPiki;
	}
	if (obj->mSearchBuffer.available()) {
		flags |= kPcEntAvail;
	}
	e.mGx    = obj->mGrid.mGridPositionX;
	e.mGz    = obj->mGrid.mGridPositionZ;
	e.mFlags = flags;
	e.mPad   = 0;
	e.mCx = e.mCy = e.mCz = 0.0f;
}

// The set's neighbour index
void pcSnapIndex(PcSnapSet& set)
{
	set.mSmallFirst          = sPcIdxSmallCount;
	set.mLargeFirst          = sPcIdxLargeCount;
	set.mUpdFirst            = sPcIdxUpdCount;
	const PcSnapEnt* entries = sPcEnt + set.mFirst;
	for (int j = 0; j < set.mCount; j++) {
		const PcSnapEnt& e = entries[j];
		if (!(e.mR <= kGridR0)) { // also catches NaN
			sPcIdxLarge[sPcIdxLargeCount++] = j;
		} else {
			GridEntry g;
			g.z                             = e.mGz;
			g.x                             = e.mGx;
			g.idx                           = j;
			sPcIdxSmall[sPcIdxSmallCount++] = g;
		}
		if (e.mFlags & kPcEntUpd) {
			sPcIdxUpd[sPcIdxUpdCount++] = j;
		}
	}
	set.mSmallCount = sPcIdxSmallCount - set.mSmallFirst;
	set.mLargeCount = sPcIdxLargeCount - set.mLargeFirst;
	set.mUpdCount   = sPcIdxUpdCount - set.mUpdFirst;
	std::sort(sPcIdxSmall + set.mSmallFirst, sPcIdxSmall + sPcIdxSmallCount, gridLess);
}

bool pcSnapQuery(const PcSnapSet& set, s16 gx, s16 gz, f32 rA)
{
	if (!(rA >= 0.0f && rA < 4096.0f)) {
		return false;
	}
	const int L  = int(s16(rA + kGridR0) >> AIPerf::gridShift) + 1;
	const int cx = gx;
	const int cz = gz;
	memset(sPcHitBits, 0, size_t((set.mCount + 31) >> 5) * sizeof(u32));
	const GridEntry* first = sPcIdxSmall + set.mSmallFirst;
	const GridEntry* last  = first + set.mSmallCount;
	for (int z = cz - L; z <= cz + L; z++) {
		GridEntry lo;
		lo.z   = s16(z);
		lo.x   = s16(cx - L);
		lo.idx = -2147483647 - 1;
		for (const GridEntry* it = std::lower_bound(first, last, lo, gridLess); it != last && int(it->z) == z && int(it->x) <= cx + L;
		     ++it) {
			sPcHitBits[it->idx >> 5] |= 1u << (it->idx & 31);
		}
	}
	const int* large = sPcIdxLarge + set.mLargeFirst;
	for (int k = 0; k < set.mLargeCount; k++) {
		sPcHitBits[large[k] >> 5] |= 1u << (large[k] & 31);
	}
	return true;
}

template <typename Visit>
inline void pcSnapForHits(int count, bool indexed, int from, Visit&& visit)
{
	if (!indexed) {
		for (int j = from; j < count; j++) {
			visit(j);
		}
		return;
	}
	const int words = (count + 31) >> 5;
	int w           = from >> 5;
	if (w >= words) {
		return;
	}
	u32 bits = sPcHitBits[w] & (~0u << (from & 31));
	for (;;) {
		while (bits) {
			const int b = __builtin_ctz(bits);
			bits &= bits - 1;
			visit((w << 5) + b);
		}
		if (++w >= words) {
			return;
		}
		bits = sPcHitBits[w];
	}
}

inline bool pcEntAlive(PcSnapEnt& e)
{
	if (!(e.mFlags & kPcEntAliveKnown)) {
		e.mFlags |= kPcEntAliveKnown | (e.mObj->isAlive() ? kPcEntAlive : 0);
	}
	return (e.mFlags & kPcEntAlive) != 0;
}

inline bool pcEntIns(PcSnapEnt& e)
{
	if (!(e.mFlags & kPcEntInsKnown)) {
		bool ins;
		if ((e.mFlags & kPcEntAliveKnown) && !(e.mFlags & kPcEntAlive)) {
			ins = false;
		} else {
			ins = e.mObj->isVisible() && pcEntAlive(e);
		}
		e.mFlags |= kPcEntInsKnown | (ins ? kPcEntIns : 0);
	}
	return (e.mFlags & kPcEntIns) != 0;
}

inline void pcEntCentre(PcSnapEnt& e)
{
	if (!(e.mFlags & kPcEntCentre)) {
		const Vector3f c = e.mObj->getCentre();
		e.mCx            = c.x;
		e.mCy            = c.y;
		e.mCz            = c.z;
		e.mFlags |= kPcEntCentre;
	}
}

// centreDist(a, b)
inline f32 pcEntDist(PcSnapEnt& a, PcSnapEnt& b)
{
	pcEntCentre(a);
	pcEntCentre(b);
	Vector3f sep = Vector3f(a.mCx, a.mCy, a.mCz) - Vector3f(b.mCx, b.mCy, b.mCz);
	return sep.length();
}

// Makes the listed inserts
void pcSnapApply()
{
	for (int i = 0; i < sPcEvtCount; i++) {
		const PcSnapEvt& ev = sPcEvt[i];
		ev.mBuf->insertQuickVisible(ev.mTgt, ev.mDist);
	}
	sPcEvtCount = 0;
}

// owner.mSearchBuffer.insert(tgt, dist)
inline void pcSnapEmit(PcSnapEnt& owner, PcSnapEnt& tgt, f32 dist)
{
	if (!pcEntIns(tgt)) {
		return;
	}
	if (sPcEvtCount == kPcSnapMaxEvt) {
		pcSnapApply();
	}
	PcSnapEvt& ev = sPcEvt[sPcEvtCount++];
	ev.mBuf       = owner.mBuf;
	ev.mTgt       = tgt.mObj;
	ev.mDist      = dist;
}

// MonoObjectMgr::search(other), mgr != this
void pcSnapMonoOther(const PcSnapSet& S, const PcSnapSet& O)
{
	PcSnapEnt* src  = sPcEnt + S.mFirst;
	PcSnapEnt* oth  = sPcEnt + O.mFirst;
	const int* upd  = sPcIdxUpd + S.mUpdFirst;
	const int shift = AIPerf::gridShift;
	for (int k = 0; k < S.mUpdCount; k++) { // updatable sources, slot order
		PcSnapEnt& s = src[upd[k]];
		if (s.mFlags & kPcEntSkip) {
			continue;
		}
		const f32 rA       = s.mR;
		const bool indexed = pcSnapQuery(O, s.mGx, s.mGz, rA);
		pcSnapForHits(O.mCount, indexed, 0, [&](int j) {
			PcSnapEnt& t = oth[j];
			if (t.mFlags & kPcEntSkip) {
				return;
			}
			const f32 rs = rA + t.mR;
			if (FastGrid::doCullingXZ(s.mGx, s.mGz, t.mGx, t.mGz, rs, shift)) {
				return;
			}
			const f32 dist = pcEntDist(s, t) - rs;
			if (dist <= 300.0f && pcEntAlive(s) && (s.mFlags & kPcEntAvail)) {
				pcSnapEmit(s, t, dist);
			}
			if (dist <= 300.0f && pcEntAlive(t) && (t.mFlags & kPcEntAvail)) {
				pcSnapEmit(t, s, dist);
			}
		});
	}
}

// MonoObjectMgr::searchSelf
void pcSnapMonoSelf(const PcSnapSet& M)
{
	PcSnapEnt* ent  = sPcEnt + M.mFirst;
	const int n     = M.mCount;
	const int* upd  = sPcIdxUpd + M.mUpdFirst;
	const int nUpd  = M.mUpdCount;
	const int shift = AIPerf::gridShift;
	int updAfter    = 0; // the first of upd[] after ia
	bool mabiki     = false;
	for (int ia = 0; ia < n; ia++) {
		PcSnapEnt& a = ent[ia];
		while (updAfter < nUpd && upd[updAfter] <= ia) {
			updAfter++;
		}
		if ((a.mFlags & kPcEntPiki) && AIPerf::pikiMabiki) {
			mabiki = true; // sticky from the first Pikmin on
		}
		if (a.mFlags & kPcEntSkip) {
			continue;
		}
		const bool upA = (a.mFlags & kPcEntUpd) != 0;
		auto visit     = [&](int ib) {
			PcSnapEnt& b   = ent[ib];
			const bool upB = (b.mFlags & kPcEntUpd) != 0;
			if (mabiki && !upA && !upB) {
				return;
			}
			if (b.mFlags & kPcEntSkip) {
				return;
			}
			const f32 rs = a.mR + b.mR;
			if (FastGrid::doCullingXZ(a.mGx, a.mGz, b.mGx, b.mGz, rs, shift)) {
				return;
			}
			const f32 dist = pcEntDist(a, b) - rs;
			if (dist <= 300.0f && pcEntAlive(a)) {
				if (pcEntAlive(a) && (a.mFlags & kPcEntAvail)) {
					if (!mabiki || upA) {
						pcSnapEmit(a, b, dist);
					}
				}
				if (pcEntAlive(b) && (b.mFlags & kPcEntAvail)) {
					if (!mabiki || upB) {
						pcSnapEmit(b, a, dist);
					}
				}
			}
		};
		if (mabiki && !upA) {
			for (int p = updAfter; p < nUpd; p++) {
				visit(upd[p]);
			}
		} else {
			const bool indexed = pcSnapQuery(M, a.mGx, a.mGz, a.mR);
			pcSnapForHits(n, indexed, ia + 1, visit);
		}
	}
}

// PolyObjectMgr::searchSelf
void pcSnapPolySelf(const PcSnapSet& M)
{
	PcSnapEnt* ent  = sPcEnt + M.mFirst;
	const int n     = M.mCount;
	const int shift = AIPerf::gridShift;
	for (int ia = 0; ia < n; ia++) {
		PcSnapEnt& a = ent[ia];
		if (a.mFlags & kPcEntSkip) {
			continue;
		}
		const bool indexed = pcSnapQuery(M, a.mGx, a.mGz, a.mR);
		pcSnapForHits(n, indexed, ia + 1, [&](int ib) {
			PcSnapEnt& b = ent[ib];
			if (b.mFlags & kPcEntSkip) {
				return;
			}
			const f32 rs = a.mR + b.mR;
			if (FastGrid::doCullingXZ(a.mGx, a.mGz, b.mGx, b.mGz, rs, shift)) {
				return;
			}
			const f32 dist = pcEntDist(a, b) - rs;
			if (dist <= 300.0f) {
				if (pcEntAlive(a) && (a.mFlags & kPcEntAvail)) {
					pcSnapEmit(a, b, dist);
				}
				if (pcEntAlive(b) && (b.mFlags & kPcEntAvail)) {
					pcSnapEmit(b, a, dist);
				}
			}
		});
	}
}

void pcSnapObj(const PcSnapSet& S, const PcSnapSet& O)
{
	PcSnapEnt* src  = sPcEnt + S.mFirst;
	PcSnapEnt* oth  = sPcEnt + O.mFirst;
	const int shift = AIPerf::gridShift;
	for (int k = 0; k < S.mCount; k++) {
		PcSnapEnt& s = src[k];
		if (s.mFlags & kPcEntSkip) {
			continue;
		}
		const f32 rA       = s.mR;
		const bool indexed = pcSnapQuery(O, s.mGx, s.mGz, rA);
		pcSnapForHits(O.mCount, indexed, 0, [&](int j) {
			PcSnapEnt& t = oth[j];
			if (t.mFlags & kPcEntSkip) {
				return;
			}
			const f32 rs = rA + t.mR;
			if (FastGrid::doCullingXZ(s.mGx, s.mGz, t.mGx, t.mGz, rs, shift)) {
				return;
			}
			const f32 dist = pcEntDist(s, t) - rs;
			if (dist <= 300.0f && pcEntAlive(s) && (s.mFlags & kPcEntAvail)) {
				pcSnapEmit(s, t, dist);
			}
			if (dist <= 300.0f && pcEntAlive(t) && (t.mFlags & kPcEntAvail)) {
				pcSnapEmit(t, s, dist);
			}
		});
	}
}

// Runs plan call k on the snapshot
void pcSnapRunCall(int k)
{
	const PcSnapCall& call = sPcPlan[k];
	sPcEvtCount            = 0;
	const PcSnapSet& self  = sPcSets[call.mSelfSet];
	const PcSnapSet& other = sPcSets[call.mOtherSet];
	switch (call.mKind) {
	case kPcSnapMono:
		if (call.mSelfSet == call.mOtherSet) {
			pcSnapMonoSelf(self);
		} else {
			pcSnapMonoOther(self, other);
		}
		break;
	case kPcSnapPoly:
		pcSnapPolySelf(self);
		break;
	default:
		pcSnapObj(self, other);
		break;
	}
	pcSnapApply();
}

int pcSnapRunFrame(SearchSystem* sys)
{
	sPcPlanCount      = 0;
	sPcPlanOverflow   = false;
	sPcSetCount       = 0;
	sPcSearchPlanning = true;
	sys->legacyCalls(); // every search() only notes itself
	sPcSearchPlanning = false;
	if (sPcPlanOverflow) {
		return kPcFbOverflow; // snapshot capacity exceeded
	}
	sPcEntCount = 0;
	for (int i = 0; i < sPcSetCount; i++) {
		const int why = PcSearchSnap::walkSet(i);
		if (why >= 0) {
			return why; // more than kPcSnapMaxEnt objects
		}
	}
	sPcIdxSmallCount = sPcIdxLargeCount = sPcIdxUpdCount = 0;
	for (int i = 0; i < sPcSetCount; i++) {
		pcSnapIndex(sPcSets[i]);
	}
	for (int k = 0; k < sPcPlanCount; k++) {
		pcSnapRunCall(k);
	}
	return -1;
}

} // namespace

void PcSearchSnap::planMono(MonoObjectMgr* self, ObjectMgr* other)
{
	pcSnapNote(kPcSnapMono, self, other, self, nullptr);
}

void PcSearchSnap::planPoly(PolyObjectMgr* self, ObjectMgr* other)
{
	if (other == self) {
		pcSnapNote(kPcSnapPoly, self, other, nullptr, self);
	}
}

void PcSearchSnap::planObj(ObjectMgr* self, ObjectMgr* other)
{
	pcSnapNote(kPcSnapObj, self, other, nullptr, nullptr);
}

int PcSearchSnap::walkSet(int i)
{
	PcSnapSet& set = sPcSets[i];
	set.mFirst     = sPcEntCount;
	if (set.mMono) {
		MonoObjectMgr* mgr = set.mMono;
		for (int k = 0; k < mgr->mMaxElements; k++) {
			if (mgr->mEntryStatus[k] != 0) {
				continue;
			}
			Creature* obj = mgr->mObjectList[k];
			if (!obj) {
				return kPcFbNull;
			}
			if (sPcEntCount >= kPcSnapMaxEnt) {
				return kPcFbOverflow;
			}
			pcSnapFill(sPcEnt[sPcEntCount++], obj);
		}
	} else if (set.mPoly) {
		PolyObjectMgr* mgr = set.mPoly;
		for (int k = 0; k < mgr->mPoolCapacity; k++) {
			if (mgr->mObjectIndices[k] < 0) {
				continue;
			}
			Creature* obj = mgr->get(k);
			if (!obj) {
				return kPcFbNull;
			}
			if (sPcEntCount >= kPcSnapMaxEnt) {
				return kPcFbOverflow;
			}
			pcSnapFill(sPcEnt[sPcEntCount++], obj);
		}
	} else {
		Iterator it(set.mMgr);
		CI_LOOP(it)
		{
			if (sPcEntCount >= kPcSnapMaxEnt) {
				return kPcFbOverflow;
			}
			pcSnapFill(sPcEnt[sPcEntCount++], *it);
		}
	}
	set.mCount = sPcEntCount - set.mFirst;
	return -1;
}

void pcSearchUpdate(SearchSystem* sys)
{
	if (pcSnapGate() < 0 && pcSnapRunFrame(sys) < 0) {
		return;
	}
	sys->legacyCalls();
}

// MonoObjectMgr::findClosest
Creature* MonoObjectMgr::findClosest(immut Vector3f& pos, immut Condition* cond)
{
	Creature* ret = nullptr;
	f32 maxDist   = 12800.0f;
	FastGrid grid;
	grid.updateGrid(pos);
	for (int i = 0; i < mMaxElements; i++) {
		if (mEntryStatus[i] != 0) {
			continue;
		}
		Creature* obj = mObjectList[i];
		if (!obj) {
			break;
		}
		if (!AIPerf::iteratorCull || !obj->mGrid.doCulling(grid, maxDist)) {
			if (!cond || cond->satisfy(obj)) {
				Vector3f diff = obj->getCentre() - pos;
				f32 len       = diff.length();
				if (len < maxDist) {
					maxDist = len;
					ret     = obj;
				}
			}
		}
	}
	return ret;
}

Creature* MonoObjectMgr::findClosest(immut Vector3f& pos, f32 maxLen, immut Condition* cond)
{
	Creature* ret = nullptr;
	f32 maxDist   = 12800.0f;
	FastGrid grid;
	grid.updateGrid(pos);
	for (int i = 0; i < mMaxElements; i++) {
		if (mEntryStatus[i] != 0) {
			continue;
		}
		Creature* obj = mObjectList[i];
		if (!obj) {
			break;
		}
		if (!AIPerf::iteratorCull || !obj->mGrid.doCulling(grid, maxDist)) {
			if (!cond || cond->satisfy(obj)) {
				Vector3f diff = obj->getCentre() - pos;
				f32 len       = diff.length();
				if (len <= maxLen && len < maxDist) {
					maxDist = len;
					ret     = obj;
				}
			}
		}
	}
	return ret;
}
#endif

#if defined(__vita__)
// node.cpp: watched-list stepping
CoreNode* pcNodeListAt(CoreNode* root, int idx);
int pcNodeListCount(CoreNode* root);
#endif

/**
 * @todo: Documentation
 */
Creature* CreatureNodeMgr::getCreature(int idx)
{
#if defined(__vita__)
	CreatureNode* node = static_cast<CreatureNode*>(pcNodeListAt(&mRootNode, idx));
#else
	CreatureNode* node = static_cast<CreatureNode*>(mRootNode.mChild);
	for (int i = 0; i < idx; i++) {
		node = static_cast<CreatureNode*>(node->mNext);
	}
#endif

	if (!node) {
		return nullptr;
	}

	return node->mCreature;
}

/**
 * @todo: Documentation
 */
int CreatureNodeMgr::getFirst()
{
	return 0;
}

/**
 * @todo: Documentation
 */
int CreatureNodeMgr::getNext(int idx)
{
	return idx + 1;
}

/**
 * @todo: Documentation
 */
bool CreatureNodeMgr::isDone(int idx)
{
#if defined(__vita__)
	if (idx >= pcNodeListCount(&mRootNode)) {
#else
	if (idx >= mRootNode.getChildCount()) {
#endif
		return true;
	}

	return false;
}

/**
 * @todo: Documentation
 */
int CreatureNodeMgr::getSize()
{
#if defined(__vita__)
	return pcNodeListCount(&mRootNode);
#else
	return mRootNode.getChildCount();
#endif
}
