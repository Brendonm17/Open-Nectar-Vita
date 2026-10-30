/* Scalar emulation of the NEON intrinsics used by the JAudio mixer, for host tests. */
#pragma once

#include <cstdint>
#include <cstring>

typedef int16_t int16x4_t __attribute__((vector_size(8)));
typedef int32_t int32x2_t __attribute__((vector_size(8)));
typedef int32_t int32x4_t __attribute__((vector_size(16)));
typedef int64_t int64x2_t __attribute__((vector_size(16)));

namespace jaudio_neon_emu {

inline int32_t wrap32(int64_t value) { return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(value))); }

inline int16_t saturate16(int64_t value)
{
	return static_cast<int16_t>(value < -32768 ? -32768 : (value > 32767 ? 32767 : value));
}

} // namespace jaudio_neon_emu

// Loads, stores, lane access.
inline int16x4_t vld1_s16(const int16_t* p)
{
	int16x4_t r;
	std::memcpy(&r, p, sizeof(r));
	return r;
}
inline int32x4_t vld1q_s32(const int32_t* p)
{
	int32x4_t r;
	std::memcpy(&r, p, sizeof(r));
	return r;
}
inline void vst1_s16(int16_t* p, int16x4_t v) { std::memcpy(p, &v, sizeof(v)); }
inline void vst1q_s32(int32_t* p, int32x4_t v) { std::memcpy(p, &v, sizeof(v)); }
inline int32x2_t vget_low_s32(int32x4_t v) { return int32x2_t { v[0], v[1] }; }
inline int32x2_t vget_high_s32(int32x4_t v) { return int32x2_t { v[2], v[3] }; }
inline int32x4_t vcombine_s32(int32x2_t lo, int32x2_t hi) { return int32x4_t { lo[0], lo[1], hi[0], hi[1] }; }
inline int64_t vgetq_lane_s64(int64x2_t v, int lane) { return v[lane]; }
inline int32x4_t vdupq_n_s32(int32_t x) { return int32x4_t { x, x, x, x }; }

// VMOVL.S16: widen.
inline int32x4_t vmovl_s16(int16x4_t v) { return int32x4_t { v[0], v[1], v[2], v[3] }; }

// VADD.I32 / VPADD.I32 (wrap), VPADDL.S32 (widen, no wrap).
inline int32x4_t vaddq_s32(int32x4_t a, int32x4_t b)
{
	int32x4_t r;
	for (int i = 0; i < 4; ++i) r[i] = jaudio_neon_emu::wrap32(int64_t(a[i]) + b[i]);
	return r;
}
inline int32x2_t vpadd_s32(int32x2_t a, int32x2_t b)
{
	return int32x2_t { jaudio_neon_emu::wrap32(int64_t(a[0]) + a[1]), jaudio_neon_emu::wrap32(int64_t(b[0]) + b[1]) };
}
inline int64x2_t vpaddlq_s32(int32x4_t v) { return int64x2_t { int64_t(v[0]) + v[1], int64_t(v[2]) + v[3] }; }

// VMULL.S16 / VMULL.S32: widening products.
inline int32x4_t vmull_s16(int16x4_t a, int16x4_t b)
{
	int32x4_t r;
	for (int i = 0; i < 4; ++i) r[i] = int32_t(a[i]) * int32_t(b[i]);
	return r;
}
inline int64x2_t vmull_s32(int32x2_t a, int32x2_t b) { return int64x2_t { int64_t(a[0]) * b[0], int64_t(a[1]) * b[1] }; }

// VMULL.S16 / VMLAL.S16 by a lane: widening product, accumulate with wrap.
inline int32x4_t vmull_lane_s16(int16x4_t a, int16x4_t v, int lane)
{
	int32x4_t r;
	for (int i = 0; i < 4; ++i) r[i] = int32_t(a[i]) * int32_t(v[lane]);
	return r;
}
inline int32x4_t vmlal_lane_s16(int32x4_t acc, int16x4_t a, int16x4_t v, int lane)
{
	int32x4_t r;
	for (int i = 0; i < 4; ++i) r[i] = jaudio_neon_emu::wrap32(int64_t(acc[i]) + int64_t(a[i]) * v[lane]);
	return r;
}

// VMUL.I32: the low 32 bits of the product.
inline int32x4_t vmulq_s32(int32x4_t a, int32x4_t b)
{
	int32x4_t r;
	for (int i = 0; i < 4; ++i) r[i] = jaudio_neon_emu::wrap32(int64_t(a[i]) * b[i]);
	return r;
}

// VSRA.S32: accumulate an arithmetic right shift, with wrap.
inline int32x4_t vsraq_n_s32(int32x4_t acc, int32x4_t a, int n)
{
	int32x4_t r;
	for (int i = 0; i < 4; ++i) r[i] = jaudio_neon_emu::wrap32(int64_t(acc[i]) + (a[i] >> n));
	return r;
}

// VSHRN.I64: shift right, keep the low 32 bits (truncating narrow).
inline int32x2_t vshrn_n_s64(int64x2_t a, int n)
{
	return int32x2_t { jaudio_neon_emu::wrap32(a[0] >> n), jaudio_neon_emu::wrap32(a[1] >> n) };
}

// VQSHRN.S32 / VQMOVN.S32: shift right (flooring, no rounding), saturate to 16 bits.
inline int16x4_t vqshrn_n_s32(int32x4_t a, int n)
{
	int16x4_t r;
	for (int i = 0; i < 4; ++i) r[i] = jaudio_neon_emu::saturate16(a[i] >> n);
	return r;
}
inline int16x4_t vqmovn_s32(int32x4_t a)
{
	int16x4_t r;
	for (int i = 0; i < 4; ++i) r[i] = jaudio_neon_emu::saturate16(a[i]);
	return r;
}
