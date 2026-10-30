#ifndef _FASTGRID_H
#define _FASTGRID_H

#include "types.h"

class Graphics;
class Texture;
class Vector3f;

/**
 * @brief A spatial grid used for AI culling optimizations.
 */
class FastGrid {
public:
	FastGrid();

	void delAIGrid();
	bool aiCulling();
	bool aiCullingLarge(int);
	bool doCulling(const FastGrid&, f32);
	void updateGrid(const Vector3f&);
	void updateAIGrid(const Vector3f&, bool);

	// unused/inlined:
	void addAIGrid();
	void renderAIGrid2D(Graphics&);

	static void initAIGrid(u8);
	static void clearAIGrid();
	static void renderAIGrid(Graphics&);

	void setNeighbourSize(int size) { mNeighbourSize = size; }

#if defined(PIKI_PC_PORT)
	/** @brief doCulling's x and z tests on cell coordinates. */
	static bool doCullingXZ(s16 ax, s16 az, s16 bx, s16 bz, f32 radius, int shift)
	{
		s16 cellLimit = s16(radius) >> shift;

		// X axis
		s16 dx     = ax - bx;
		s16 absDx  = (dx > 0) ? dx : -(dx);
		s16 excess = (absDx == 0) ? 0 : absDx - 1;
		if (excess > cellLimit) {
			return true;
		}

		// Z axis
		s16 dz    = az - bz;
		s16 absDz = (dz > 0) ? dz : -(dz);
		excess    = (absDz == 0) ? 0 : absDz - 1;
		if (excess > cellLimit) {
			return true;
		}
		return false;
	}
#endif

	static u16 aiGridShift;
	static u8* aiGridMap;
	static u16 aiGridSize;
	static Texture* aiGridTex;

	s16 mGridPositionX;  // _00
	u8 _02[0x04 - 0x02]; // _02
	s16 mGridPositionY;  // _04
	u8 _06[0x08 - 0x06]; // _06
	s16 mGridPositionZ;  // _08
	u8 _0A[0x0C - 0x0A]; // _0A
	s16 mWidth;          // _0C, TODO: This is misnamed
	u8 _0E[0x10 - 0x0E]; // _0E
	s16 mHeight;         // _10, TODO: This is misnamed
	u8 _12[0x14 - 0x12]; // _12
	u16 mNeighbourSize;  // _14
	u8 _16[0x18 - 0x16]; // _16
};

#endif
