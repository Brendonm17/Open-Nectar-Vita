#include "DebugLog.h"
#ifdef __vita__
#include <psp2/pvf.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include "vita_button_glyphs.h"
#endif
#include "P2D/Font.h"
#include "Texture.h"
#include "sysNew.h"

/**
 * @todo: Documentation
 * @note UNUSED Size: 00009C
 */
DEFINE_ERROR(__LINE__) // Never used in the DLL

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000F0
 */
DEFINE_PRINT(nullptr)

static u32 packGXColor(const Colour& color)
{
	// Colour is not guaranteed to be 32-bit aligned inside P2DFont. Packing the
	// bytes explicitly also makes the native little-endian FIFO convention clear.
	return static_cast<u32>(color.r) | (static_cast<u32>(color.g) << 8)
	     | (static_cast<u32>(color.b) << 16) | (static_cast<u32>(color.a) << 24);
}

/**
 * @todo: Documentation
 */
Font* P2DFont::loadFont(immut char* name, int& rows, int& columns)
{
	Font* font       = nullptr;
	FntobjInfo* info = (FntobjInfo*)gsys->findGfxObject(name, 'font');
	if (info) {
		font = info->mFont;
	}

	if (!font) {
		font             = new Font();
		FntobjInfo* info = new FntobjInfo();
		info->mString    = StdSystem::stringDup(name);
		info->mId.setID('font');
		info->mFont = font;
		gsys->addGfxObject(info);

		char unused[PATH_MAX];
		sprintf(unused, "%s", name); // why.

		Texture* fontTex = gsys->loadTexture("bigFont.bti", true);
		rows             = 21;
		columns          = 42;
		font->setTexture(fontTex, rows, columns);

	} else {
		rows    = 21;
		columns = 42;
	}

	return font;
}

/**
 * @todo: Documentation
 */
P2DFont::P2DFont(immut char* fileName)
{
	int rows, columns;
	mFont     = loadFont(fileName, rows, columns);
	mFontType = 2;
	mWidth    = (mFont->mTexture->mWidth / rows);
	mLeading  = 5;
	mDescent  = (s32)((mFont->mTexture->mHeight / columns) * 0.0f);
	mAscent   = (mFont->mTexture->mHeight / columns) - mDescent;
}

/**
 * @todo: Documentation
 */
void P2DFont::setGX()
{
	// The GameCube GX state retained the font texture binding implicitly. The
	// native backend starts each frame with no active texture, so bind it
	// explicitly before emitting glyph quads.
	if (mFont && mFont->mTexture) {
		if (mFont->mTexture->mAttachName == -1) {
			mFont->mTexture->attach();
		}
		GXLoadTexObj(mFont->mTexture->mTexObj, GX_TEXMAP0);
	}

	GXSetNumChans(1);
	GXSetNumTevStages(1);
	GXSetNumTexGens(1);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);

	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_U16, 15);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
}

/**
 * @todo: Documentation
 */
void P2DFont::setGradColor(const Colour& topColour, const Colour& bottomColour)
{
	mTLColour = topColour;
	mTRColour = topColour;
	mBLColour = bottomColour;
	mBRColour = bottomColour;
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000040
 */
int P2DFont::charToIndex(int c)
{
	if (c & ~0xFF) {
		return mFont->charToIndex(u16(c));
	}

	return mFont->charToIndex(char(c));
}

/**
 * @todo: Documentation
 */
f32 P2DFont::getWidth(int charCode, int drawWidth)
{
	f32 a = mFont->mChars[charToIndex(charCode)].mCharSpacing;
	f32 b = drawWidth;
	a *= b / getWidth();
	return a + 1.0f;
}

/**
 * @todo: Documentation
 */
f32 P2DFont::drawChar(f32 xPos, f32 yPos, int charCode, int drawWidth, int drawHeight)
{
	FontChar* fc = &mFont->mChars[charToIndex(charCode)];
	f32 xScale   = f32(drawWidth) / getWidth();
	f32 yScale   = f32(drawHeight) / getHeight();
	f32 x0       = xPos - fc->mLeftOffset * xScale;
	f32 x1       = xPos + (fc->mWidth - fc->mLeftOffset) * xScale;
	f32 y0       = yPos - getAscent() * yScale;
	f32 y1       = yPos + getDescent() * yScale;

	u16 s0 = f32(fc->mTextureCoords.mMinX + 0.5f) * 32768.0f * mFont->mTexture->mWidthFactor;
	u16 t0 = f32(fc->mTextureCoords.mMinY + 0.5f) * 32768.0f * mFont->mTexture->mHeightFactor;
	u16 s1 = f32(fc->mTextureCoords.mMaxX + 0.5f) * 32768.0f * mFont->mTexture->mWidthFactor;
	u16 t1 = f32(fc->mTextureCoords.mMaxY + 0.5f) * 32768.0f * mFont->mTexture->mHeightFactor;

	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXBegin(GX_QUADS, GX_VTXFMT0, 4);

	GXPosition3f32(x0, y0, 0.0f);
	GXColor1u32(packGXColor(mTLColour));
	GXTexCoord2u16(s0, t0);

	GXPosition3f32(x1, y0, 0.0f);
	GXColor1u32(packGXColor(mTRColour));
	GXTexCoord2u16(s1, t0);

	GXPosition3f32(x1, y1, 0.0f);
	GXColor1u32(packGXColor(mBRColour));
	GXTexCoord2u16(s1, t1);

	GXPosition3f32(x0, y1, 0.0f);
	GXColor1u32(packGXColor(mBLColour));
	GXTexCoord2u16(s0, t1);

	GXEnd();
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 0);

	STACK_PAD_VAR(2);

	return x1 - x0;
}

#ifdef __vita__
namespace {
// PS Vita button glyphs, rasterised from sa0:data/font/pvf/psexchar.pvf.
enum { kPvfCell = 48, kPvfCells = 11, kPvfInk = 40 };
// Cells in pc_window_button_glyph order.
// Square, Triangle, L, R, Start, Select, left stick, right stick, d-pad.
const ScePvfCharCode kPvfChars[kPvfCells] = { '&', '%', '\'', '$', '0', '1', '5', '4', '2', '3', ',' };

ScePvfPointer pvfAlloc(ScePvfPointer, ScePvfU32 size) { return memalign(sizeof(int), (size + 3u) & ~3u); }
ScePvfPointer pvfRealloc(ScePvfPointer, ScePvfPointer p, ScePvfU32 size) { return realloc(p, (size + 3u) & ~3u); }
void          pvfFree(ScePvfPointer, ScePvfPointer p) { free(p); }

// One glyph's coverage centred in a cell-sized 8-bit buffer.
bool pvfRasterCell(ScePvfFontId font, ScePvfCharCode code, u8* cov)
{
	ScePvfCharInfo ci;
	if (scePvfGetCharInfo(font, code, &ci) < 0) return false;
	const int bw = ci.bitmapWidth, bh = ci.bitmapHeight;
	if (bw <= 0 || bh <= 0 || bw > kPvfCell || bh > kPvfCell) return false;
	const int dx = (kPvfCell - bw) / 2, dy = (kPvfCell - bh) / 2;
	ScePvfUserImageBufferRec img;
	memset(&img, 0, sizeof(img));
	img.pixelFormat  = SCE_PVF_USERIMAGE_DIRECT8;
	img.xPos64       = (dx << 6) - ci.glyphMetrics.horizontalBearingX64;
	img.yPos64       = (dy << 6) + ci.glyphMetrics.horizontalBearingY64;
	img.rect.width   = kPvfCell;
	img.rect.height  = kPvfCell;
	img.bytesPerLine = kPvfCell;
	img.buffer       = cov;
	memset(cov, 0, kPvfCell * kPvfCell);
	return scePvfGetCharGlyphImage(font, code, &img) == 0;
}

// The strip as GX RGBA8 (4x4 tiles: AR pairs then GB pairs), or null.
u8* pvfBuildStrip()
{
	ScePvfError   err  = 0;
	ScePvfInitRec init = { nullptr, 1, nullptr, nullptr, pvfAlloc, pvfRealloc, pvfFree };
	ScePvfLibId   lib  = scePvfNewLib(&init, &err);
	if (!lib || err) { printf("ERROR: ScePvf library failed (0x%08x); drawn button glyphs used\n", (unsigned)err); return nullptr; }
	scePvfSetEM(lib, 72.0f / (10.125f * 128.0f));
	scePvfSetResolution(lib, 128.0f, 128.0f);
	ScePvfFontId font = scePvfOpenUserFile(lib, (ScePvfPointer) "sa0:data/font/pvf/psexchar.pvf", 1, &err);
	if (!font || err) {
		printf("ERROR: psexchar.pvf unavailable (0x%08x); drawn button glyphs used\n", (unsigned)err);
		scePvfDoneLib(lib);
		return nullptr;
	}
	f32 size = 10.125f;
	scePvfSetCharSize(font, size, size);
	ScePvfCharInfo ci;
	bool ok = scePvfGetCharInfo(font, '%', &ci) == 0 && ci.bitmapHeight > 0;
	if (ok) {
		size = size * f32(kPvfInk) / f32(ci.bitmapHeight);
		scePvfSetCharSize(font, size, size);
	}
	const int W = kPvfCell * kPvfCells, H = kPvfCell;
	u8* strip = ok ? (u8*)memalign(32, W * H * 4) : nullptr;
	if (strip) memset(strip, 0, W * H * 4);
	static u8 cov[kPvfCell * kPvfCell];
	for (int c = 0; strip && c < kPvfCells; c++) {
		if (!pvfRasterCell(font, kPvfChars[c], cov)) { ok = false; break; }
		for (int y = 0; y < kPvfCell; y++) {
			for (int x = 0; x < kPvfCell; x++) {
				const int ax = c * kPvfCell + x;
				u8* tile     = strip + ((y / 4) * (W / 4) + (ax / 4)) * 64;
				const int in = ((y & 3) * 4 + (ax & 3)) * 2;
				tile[in]          = cov[y * kPvfCell + x]; // A
				tile[in + 1]      = 255;                   // R
				tile[32 + in]     = 255;                   // G
				tile[32 + in + 1] = 255;                   // B
			}
		}
	}
	scePvfClose(font);
	scePvfDoneLib(lib);
	if (!ok) { printf("ERROR: psexchar.pvf glyph rasterisation failed; drawn button glyphs used\n"); free(strip); return nullptr; }
	return strip;
}

Texture* sButtonGlyphs = nullptr;
int      sGlyphCell    = 0;
} // namespace

f32 P2DFont::drawButtonGlyph(f32 xPos, f32 yPos, int cell, int drawHeight)
{
	PikiPcPersistentAlloc pcKeep;
	if (!sButtonGlyphs) {
		sButtonGlyphs = new Texture();
		if (u8* strip = pvfBuildStrip()) {
			sButtonGlyphs->createBuffer(kPvfCell * kPvfCells, kPvfCell, TEX_FMT_RGBA8, strip);
			sGlyphCell = kPvfCell;
		} else {
			sButtonGlyphs->createBuffer(NECTAR_BUTTON_GLYPH_W, NECTAR_BUTTON_GLYPH_H, TEX_FMT_RGBA8, (void*)nectar_button_glyphs);
			sGlyphCell = NECTAR_BUTTON_GLYPH_CELL;
		}
		sButtonGlyphs->mTexFlags = Texture::TEX_CLAMP_S | Texture::TEX_CLAMP_T;
	}
	if (sButtonGlyphs->mAttachName == -1) sButtonGlyphs->attach();
	GXLoadTexObj(sButtonGlyphs->mTexObj, GX_TEXMAP0);
	const f32 yScale = f32(drawHeight) / getHeight();
	const f32 y0     = yPos - getAscent() * yScale;
	const f32 y1     = yPos + getDescent() * yScale;
	const f32 side   = y1 - y0;
	const f32 x0 = xPos, x1 = xPos + side;
	const f32 cw = f32(sGlyphCell) * sButtonGlyphs->mWidthFactor;
	const u16 s0 = u16(cell * cw * 32768.0f), s1 = u16((cell + 1) * cw * 32768.0f);
	const u16 t0 = 0, t1 = 32768;
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXBegin(GX_QUADS, GX_VTXFMT0, 4);
	const u32 top = packGXColor(Colour(255, 255, 255, mTLColour.a)), bottom = packGXColor(Colour(255, 255, 255, mBLColour.a));
	GXPosition3f32(x0, y0, 0.0f); GXColor1u32(top);    GXTexCoord2u16(s0, t0);
	GXPosition3f32(x1, y0, 0.0f); GXColor1u32(top);    GXTexCoord2u16(s1, t0);
	GXPosition3f32(x1, y1, 0.0f); GXColor1u32(bottom); GXTexCoord2u16(s1, t1);
	GXPosition3f32(x0, y1, 0.0f); GXColor1u32(bottom); GXTexCoord2u16(s0, t1);
	GXEnd();
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 0);
	GXLoadTexObj(mFont->mTexture->mTexObj, GX_TEXMAP0);
	return side;
}
#endif
