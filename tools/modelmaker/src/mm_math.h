#pragma once

// Cuentas del creador de modelos sobre los tipos de nectar_assets: Vec3 y
// Mat34 (afín 3x4 por filas, como Mtx de GX y Matrix4f del juego).

#include "math3d.h"
#include "nectar_assets/mod.h"

#include <algorithm>
#include <cmath>

namespace mm {

using nectar::Mat34;
using nectar::Vec2;
using nectar::Vec3;

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3 operator*(const Vec3& a, float k) { return { a.x * k, a.y * k, a.z * k }; }
inline Vec3& operator+=(Vec3& a, const Vec3& b) { return a = a + b; }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(const Vec3& a)
{
	const float l = length(a);
	return l > 1e-12f ? a * (1.0f / l) : Vec3 { 0, 1, 0 };
}
inline Vec3 vmin(const Vec3& a, const Vec3& b) { return { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) }; }
inline Vec3 vmax(const Vec3& a, const Vec3& b) { return { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) }; }

inline Vec3 translationOf(const Mat34& m) { return { m.m[0][3], m.m[1][3], m.m[2][3] }; }
inline void setTranslation(Mat34& m, const Vec3& t)
{
	m.m[0][3] = t.x;
	m.m[1][3] = t.y;
	m.m[2][3] = t.z;
}
inline Mat34 translation(const Vec3& t)
{
	Mat34 m;
	setTranslation(m, t);
	return m;
}
inline Mat34 uniformScale(float k)
{
	Mat34 m;
	m.m[0][0] = m.m[1][1] = m.m[2][2] = k;
	return m;
}

/// Inversa afín general (las animaciones pueden escalar).
inline Mat34 inverse(const Mat34& a)
{
	const auto& m  = a.m;
	const float c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
	const float c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
	const float c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
	const float det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
	Mat34 r;
	if (std::fabs(det) < 1e-12f) return r;
	const float k = 1.0f / det;
	r.m[0][0]     = c00 * k;
	r.m[0][1]     = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * k;
	r.m[0][2]     = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * k;
	r.m[1][0]     = c01 * k;
	r.m[1][1]     = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * k;
	r.m[1][2]     = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * k;
	r.m[2][0]     = c02 * k;
	r.m[2][1]     = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * k;
	r.m[2][2]     = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * k;
	for (int row = 0; row < 3; ++row) r.m[row][3] = -(r.m[row][0] * m[0][3] + r.m[row][1] * m[1][3] + r.m[row][2] * m[2][3]);
	return r;
}

/// Giro de `angle` radianes alrededor del eje unitario `axis`.
inline Mat34 axisAngle(const Vec3& axis, float angle)
{
	const float c = std::cos(angle), s = std::sin(angle), t = 1 - c;
	const float x = axis.x, y = axis.y, z = axis.z;
	Mat34 r;
	r.m[0][0] = t * x * x + c;
	r.m[0][1] = t * x * y - s * z;
	r.m[0][2] = t * x * z + s * y;
	r.m[1][0] = t * x * y + s * z;
	r.m[1][1] = t * y * y + c;
	r.m[1][2] = t * y * z - s * x;
	r.m[2][0] = t * x * z - s * y;
	r.m[2][1] = t * y * z + s * x;
	r.m[2][2] = t * z * z + c;
	return r;
}

/// El giro más corto que lleva la dirección `from` a `to`.
inline Mat34 rotationBetween(const Vec3& from, const Vec3& to)
{
	const Vec3 a = normalize(from), b = normalize(to);
	const float c = std::clamp(dot(a, b), -1.0f, 1.0f);
	Vec3 axis     = cross(a, b);
	if (length(axis) < 1e-6f) {
		if (c > 0) return Mat34();
		axis = std::fabs(a.x) < 0.9f ? cross(a, Vec3 { 1, 0, 0 }) : cross(a, Vec3 { 0, 1, 0 });
	}
	return axisAngle(normalize(axis), std::acos(c));
}

/// `r` aplicado alrededor del punto `pivot`.
inline Mat34 about(const Vec3& pivot, const Mat34& r) { return translation(pivot) * r * translation(pivot * -1.0f); }

/// Para la vista (matrices por columnas de math3d.h).
inline nm::M4 toM4(const Mat34& a)
{
	nm::M4 r;
	for (int row = 0; row < 3; ++row)
		for (int col = 0; col < 4; ++col) r.at(row, col) = a.m[row][col];
	return r;
}

inline Mat34 lerpJoint(const Mat34& a, const Mat34& b, float t)
{
	Mat34 r;
	for (int row = 0; row < 3; ++row)
		for (int col = 0; col < 4; ++col) r.m[row][col] = a.m[row][col] + (b.m[row][col] - a.m[row][col]) * t;
	return r;
}

} // namespace mm
