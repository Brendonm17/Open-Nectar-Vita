#if defined(PIKI_PC_PORT)
extern "C" void pc_settings_set_in_field(int on);   /* settings/pc_settings.cpp */
#endif
#include "Controller.h"
#include "DebugLog.h"
#include "Dolphin/gx.h"
#include "FlowController.h"
#include "Graphics.h"
#include "MovSampleSection.h"
#include "gameflow.h"
#include "jaudio/app_inter.h"
#include "jaudio/piki_scene.h"
#include "sysNew.h"
#include "system.h"
#if defined(PIKI_PC_PORT)
#include "gl/pc_gfx.h"
#include <string.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
#endif

u16 ImgH;
u16 ImgW;
GXTexObj YtexObj;
GXTexObj UVtexObj;
OSThread playbackThread;
u8 playbackThreadStack[0x1000];
bool finishPlayback;
#ifdef __vita__
extern "C" void vita_audio_set_movie(int on);
#endif

/**
 * @todo: Documentation
 * @note UNUSED Size: 00009C
 */
DEFINE_ERROR(__LINE__) // Never used in the DLL

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000F4
 */
DEFINE_PRINT("MovSample")

/**
 * @todo: Documentation
 */
void convHVQM4TexY8UV8(int stride, int height, u8* src, u8* dst)
{
	u32* out;
	int i, j;

	// Part 1: Y plane processing
	u32* y0 = (u32*)src;
	u32* y1 = y0 + (stride / 4);
	u32* y2 = y1 + (stride / 4);
	u32* y3 = y2 + (stride / 4);

	for (i = height, out = (u32*)dst; i > 0; i -= 4) {
		for (j = stride; j > 0; j -= 8) {
			out[0] = y0[0];
			out[1] = y0[1];
			out[2] = y1[0];
			out[3] = y1[1];
			out[4] = y2[0];
			out[5] = y2[1];
			out[6] = y3[0];
			out[7] = y3[1];

			y0 += 2;
			y1 += 2;
			y2 += 2;
			y3 += 2;
			out += 8;
		}

		// advance to next 4 lines
		y0 = y3;
		y1 = y0 + (stride / 4);
		y2 = y1 + (stride / 4);
		y3 = y2 + (stride / 4);
	}

	// Part 2: UV plane processing
	u8* srcuv = src + (stride * height);

	// base pointers for four lines of U and V
	u8* u0 = srcuv;
	u8* u1 = u0 + (stride / 2);
	u8* u2 = u1 + (stride / 2);
	u8* u3 = u2 + (stride / 2);

	u8* v0 = srcuv + ((stride / 2) * (height / 2));
	u8* v1 = v0 + (stride / 2);
	u8* v2 = v1 + (stride / 2);
	u8* v3 = v2 + (stride / 2);

	u8* o = dst + (stride * height);

	for (i = height / 2; i > 0; i -= 4) {
		for (j = stride / 2; j > 0; j -= 4) {
			for (int k = 0; k < 4; k++) {
				o[0] = u0[k]; o[1] = v0[k]; o += 2;
			}
			u0 += 4; v0 += 4;
			for (int k = 0; k < 4; k++) {
				o[0] = u1[k]; o[1] = v1[k]; o += 2;
			}
			u1 += 4; v1 += 4;
			for (int k = 0; k < 4; k++) {
				o[0] = u2[k]; o[1] = v2[k]; o += 2;
			}
			u2 += 4; v2 += 4;
			for (int k = 0; k < 4; k++) {
				o[0] = u3[k]; o[1] = v3[k]; o += 2;
			}
			u3 += 4; v3 += 4;

		}

		// advance to next block of 4 UV lines
		u0 = u3;
		u1 = u0 + (stride / 2);
		u2 = u1 + (stride / 2);
		u3 = u2 + (stride / 2);

		v0 = v3;
		v1 = v0 + (stride / 2);
		v2 = v1 + (stride / 2);
		v3 = v2 + (stride / 2);
	}

	DCStoreRange(dst, (stride * height) + ((stride / 2) * (height / 2)) * 2);
}

/**
 * @todo: Documentation
 */
#if defined(PIKI_PC_PORT)
// Linear plane packer
static void convHVQM4LinearY8UV8(int stride, int height, const u8* src, u8* dst)
{
	memcpy(dst, src, size_t(stride) * size_t(height));
	const u8* u = src + stride * height;
	const u8* v = u + (stride / 2) * (height / 2);
	u8* o       = dst + stride * height;
	const int count = (stride / 2) * (height / 2);
	int k = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
	for (; k + 16 <= count; k += 16) {
		uint8x16x2_t uv;
		uv.val[0] = vld1q_u8(u + k);
		uv.val[1] = vld1q_u8(v + k);
		vst2q_u8(o + k * 2, uv);
	}
#endif
	for (; k < count; ++k) {
		o[k * 2]     = u[k];
		o[k * 2 + 1] = v[k];
	}
}
#endif

static void* playbackFunc(void*)
{
	while (!finishPlayback) {
#if defined(PIKI_PC_PORT)
		for (int i = 0; i < 4 && !finishPlayback; i++) {
			Jac_StreamMovieUpdate();
		}
		OSYieldThread();
#else
		Jac_StreamMovieUpdate();
#endif
	}
	return nullptr;
}

/**
 * @brief TODO
 */
struct MovSampleSetupSection : public Node {
	MovSampleSetupSection()
	{
		setName("MovSample section");
		mController = new Controller(1);
#if defined(VERSION_GPIJ01) || defined(VERSION_DPIJ01_PIKIDEMO)
#else
		_28 = 160;
#endif
		_30 = 0;
		gsys->setFade(1.0f);

		static const char* movieNames[MOV_COUNT] = {
			"../MovieData/cntA_S.h4m", "../MovieData/cntB_S.h4m", "../MovieData/cntC_S.h4m",
			"../MovieData/cntD_S.h4m", "../MovieData/sr_S.h4m",   "../MovieData/srhp_S.h4m",
		};
#if PIKI_USE_JAUDIO
		int movieBufferSize = 0xe00000;
		u8* movieBuffer     = new (PIKI_ALIGNED(0x20)) u8[movieBufferSize];
		Jac_StreamMovieInit(movieNames[gameflow.mCurrIntroMovieID], movieBuffer, movieBufferSize);
#if defined(PIKI_PC_PORT)
		pc_gfx_set_movie_planes_linear(1);
#ifdef __vita__
		vita_audio_set_movie(1);
#endif
#endif
		ImgW                = 640;
		ImgH                = 480;
		int yuvBufferSize   = 0x70800;
		mYuvFrameBuffers[0] = new (PIKI_ALIGNED(0x20)) u8[yuvBufferSize];
		mYuvFrameBuffers[1] = new (PIKI_ALIGNED(0x20)) u8[yuvBufferSize];
		for (int i = 0; i < 2; i++) {
			u8* frameBuffer  = mYuvFrameBuffers[i];
			u8* chromaBuffer = &mYuvFrameBuffers[i][ImgW * ImgH];
			memset(frameBuffer, 0x10, ImgW * ImgH);
			memset(chromaBuffer, 0x80, (ImgW / 2) * (ImgH / 2) * 2);
		}
		OSCreateThread(&playbackThread, &playbackFunc, 0, playbackThreadStack + sizeof(playbackThreadStack), sizeof(playbackThreadStack),
		               0x14, OS_THREAD_ATTR_DETACH);
		finishPlayback = false;
		OSResumeThread(&playbackThread);
#endif
	}

	virtual void update() // _10 (weak)
	{
#if defined(PIKI_PC_PORT)
		OSYieldThread();
#endif
		mController->update();

		int pictureStatus = 0;
		u8* pictureData   = nullptr;
		int pictureWidth, pictureHeight;
		if (gsys->mDvdErrorCode < DvdError::ReadingDisc) { // AKA: DvdError::None (no issue)
			pictureStatus = Jac_StreamMovieGetPicture(&pictureData, &pictureWidth, &pictureHeight);
		}

		if (pictureData && pictureStatus) {
#if defined(PIKI_PC_PORT)
			convHVQM4LinearY8UV8(pictureWidth, pictureHeight, pictureData, mYuvFrameBuffers[mFrameBufferIndex ^ 1]);
#elif defined(VERSION_GPIJ01) || defined(VERSION_DPIJ01_PIKIDEMO)
			convHVQM4TexY8UV8(pictureWidth, pictureHeight, pictureData, mYuvFrameBuffers[mFrameBufferIndex]);
#else
			convHVQM4TexY8UV8(pictureWidth, pictureHeight, pictureData, mYuvFrameBuffers[mFrameBufferIndex ^ 1]);
#endif
			mFrameBufferIndex ^= 1;
#if defined(PIKI_PC_PORT)
			OSYieldThread();
#endif
		}

		bool shouldExit = false;
		if (flowCont.mEndingType == ENDING_None && mController->keyClick(KBBTN_START | KBBTN_A | KBBTN_B)) {
			shouldExit = true;
		}

		if (shouldExit || pictureStatus == -1) {
#if defined(PIKI_PC_PORT)
			finishPlayback = true;
#endif
			Jac_StreamMovieStop();
			OSCancelThread(&playbackThread);
#if defined(PIKI_PC_PORT)
			OSJoinThread(&playbackThread, nullptr);
#endif

			if (flowCont.mEndingType != ENDING_None) {
				Jac_SceneExit(SCENE_Exit, 0);
				flowCont.mEndingType = ENDING_None;
			}

			gameflow.mNextGameSectionID = SECTION_Titles;
			gsys->softReset();
		}

		STACK_PAD_VAR(1);
	}
	virtual void draw(Graphics& gfx) // _14 (weak)
	{
#if defined(PIKI_PC_PORT)
		OSYieldThread();
		pc_gfx_set_section_clip_43(0);
		pc_settings_set_in_field(0);
		if (finishPlayback) {
			GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
			GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
			GXSetTevKColor(GX_KCOLOR0, (GXColor) { 255, 255, 255, 255 });
			GXSetTevKColor(GX_KCOLOR1, (GXColor) { 255, 255, 255, 255 });
			GXSetTevKColor(GX_KCOLOR2, (GXColor) { 255, 255, 255, 255 });
			GXSetTevKColor(GX_KCOLOR3, (GXColor) { 255, 255, 255, 255 });
			GXSetTevColorS10(GX_TEVREG0, (GXColorS10) { 0, 0, 0, 0 });
			GXSetTevColor(GX_TEVREG0, (GXColor) { 255, 255, 255, 255 });
			pc_gfx_set_movie_planes_linear(0);
#ifdef __vita__
			vita_audio_set_movie(0);
#endif
			return;
		}
#endif
		gfx.setViewport(AREA_FULL_SCREEN(gfx));
		gfx.setScissor(AREA_FULL_SCREEN(gfx));
		gfx.setClearColour(COLOUR_TRANSPARENT);
		gfx.clearBuffer(Graphics::ClearBufferFlag::Both, false);
		Matrix4f mtx;
		STACK_PAD_VAR(64);
		gfx.setOrthogonal(mtx.mMtx, AREA_FULL_SCREEN(gfx));

		GXSetNumTexGens(2);
		GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3X4, GX_TG_TEX0, 60, 0, 125);
		GXSetTexCoordGen2(GX_TEXCOORD1, GX_TG_MTX3X4, GX_TG_TEX0, 60, 0, 125);
		GXInvalidateTexAll();
		GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
		GXSetNumTevStages(4);

		GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
		GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_C0);
		GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
		GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_KONST, GX_CA_A0);
		GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
		GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
		GXSetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
		GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP1);

		GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR_NULL);
		GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_CPREV);
		GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_FALSE, GX_TEVPREV);
		GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_TEXA, GX_CA_KONST, GX_CA_APREV);
		GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_FALSE, GX_TEVPREV);
		GXSetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K1);
		GXSetTevKAlphaSel(GX_TEVSTAGE1, GX_TEV_KASEL_K1_A);
		GXSetTevSwapMode(GX_TEVSTAGE1, GX_TEV_SWAP0, GX_TEV_SWAP0);

		GXSetTevOrder(GX_TEVSTAGE2, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
		GXSetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_CPREV);
		GXSetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GXSetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_TEXA, GX_CA_KONST, GX_CA_APREV);
		GXSetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GXSetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K2);
		GXSetTevKAlphaSel(GX_TEVSTAGE2, GX_TEV_KASEL_K2_A);
		GXSetTevSwapMode(GX_TEVSTAGE2, GX_TEV_SWAP0, GX_TEV_SWAP2);

		GXSetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
		GXSetTevColorIn(GX_TEVSTAGE3, GX_CC_CPREV, GX_CC_APREV, GX_CC_KONST, GX_CC_ZERO);
		GXSetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GXSetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
		GXSetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GXSetTevSwapMode(GX_TEVSTAGE3, GX_TEV_SWAP0, GX_TEV_SWAP0);
		GXSetTevKColorSel(GX_TEVSTAGE3, GX_TEV_KCSEL_K3);

		setTevColors();

		GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
		GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_ALPHA, GX_CH_ALPHA, GX_CH_ALPHA);
		GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA, GX_CH_RED);

		u8* data = mYuvFrameBuffers[mFrameBufferIndex];
		GXInitTexObj(&YtexObj, data, ImgW, ImgH, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GXInitTexObjLOD(&YtexObj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
		GXLoadTexObj(&YtexObj, GX_TEXMAP1);
#if defined(PIKI_PC_PORT)
		OSYieldThread();   // between the two plane uploads
#endif

		GXInitTexObj(&UVtexObj, &data[ImgW * ImgH], ImgW / 2, ImgH / 2, GX_TF_IA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GXInitTexObjLOD(&UVtexObj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
		GXLoadTexObj(&UVtexObj, GX_TEXMAP0);

		gfx.setColour(COLOUR_WHITE, true);

		// WHY WONT YOU USE DIFFERENT REGISTERS
		int width, height;
		gfx.testRectangle(RectArea(0, 0, width = 640, height = 480));

		GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
		GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
		GXSetTevSwapMode(GX_TEVSTAGE1, GX_TEV_SWAP0, GX_TEV_SWAP0);
		GXSetTevSwapMode(GX_TEVSTAGE2, GX_TEV_SWAP0, GX_TEV_SWAP0);
		GXSetTevSwapMode(GX_TEVSTAGE3, GX_TEV_SWAP0, GX_TEV_SWAP0);

		gfx.setColour(Colour(255, 255, 64, 255), true);
		gfx.setAuxColour(Colour(255, 0, 64, 255));

		gameflow.drawLoadLogo(gfx, false, gameflow.mLevelBannerTex, gameflow.mLevelBannerFadeValue);
	}

	// not in the DLL, but needed for stack ordering
	void setTevColors()
	{
#if PIKI_USE_DGX
		GXSetTevColorS10(GX_TEVREG0, (GXColorS10) { -111, 0, -138, 68 });
		GXSetTevKColor(GX_KCOLOR0, (GXColor) { 102, 0, 255, 50 });
		GXSetTevKColor(GX_KCOLOR1, (GXColor) { 148, 0, 148, 148 });
		GXSetTevKColor(GX_KCOLOR2, (GXColor) { 203, 0, 5, 207 });
		GXSetTevKColor(GX_KCOLOR3, (GXColor) { 0, 255, 0, 0 });
#endif
	}

	// _00     = VTBL
	// _00-_20 = Node
	int _20; // _20
	int _24; // _24
#if defined(VERSION_GPIJ01) || defined(VERSION_DPIJ01_PIKIDEMO)
#else
	int _28; // _28
#endif
	Controller* mController; // _2C
	int _30;                 // _30
	int _34;                 // _34
	int _38;                 // _38
	int mFrameBufferIndex;   // _3C
	int _40;                 // _40
	int _44;                 // _44
	u8* mYuvFrameBuffers[2]; // _48
};

/**
 * @todo: Documentation
 */
void MovSampleSection::init()
{
	Node::init("<MovSampleSection>");
// run h4m movies at 60 fps
#if defined(VERSION_DPIJ01_PIKIDEMO)
	gsys->setFrameClamp(2);
#else
	gsys->setFrameClamp(1);
#endif

	gsys->mTimerState = TS_Off;
	gsys->startLoading(nullptr, true, 60);

	add(new MovSampleSetupSection);
	gsys->endLoading();
}
