#include "simplify.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <queue>
#include <unordered_map>

namespace mm {

namespace {

struct Quadric {
	double a[10] = {}; // simétrica 4x4: xx xy xz xw yy yz yw zz zw ww
	void addPlane(double nx, double ny, double nz, double d, double w)
	{
		const double p[4] = { nx, ny, nz, d };
		int k             = 0;
		for (int i = 0; i < 4; ++i)
			for (int j = i; j < 4; ++j) a[k++] += w * p[i] * p[j];
	}
	void add(const Quadric& o)
	{
		for (int i = 0; i < 10; ++i) a[i] += o.a[i];
	}
	double eval(const Vec3& v) const
	{
		const double x = v.x, y = v.y, z = v.z;
		return a[0] * x * x + 2 * a[1] * x * y + 2 * a[2] * x * z + 2 * a[3] * x + a[4] * y * y + 2 * a[5] * y * z + 2 * a[6] * y + a[7] * z * z
		     + 2 * a[8] * z + a[9];
	}
};

struct Collapse {
	double cost;
	std::uint32_t from, to;
	bool operator>(const Collapse& o) const { return cost > o.cost; }
};

std::uint64_t hashBytes(const void* p, std::size_t n)
{
	std::uint64_t h        = 1469598103934665603ull;
	const unsigned char* c = static_cast<const unsigned char*>(p);
	for (std::size_t i = 0; i < n; ++i) h = (h ^ c[i]) * 1099511628211ull;
	return h;
}

} // namespace

std::size_t simplifyModel(ImpModel& model, std::size_t targetTris)
{
	const std::size_t triCount = model.verts.size() / 3;
	if (triCount <= targetTris || triCount < 16) return triCount;

	// Vértices únicos: misma posición, UV y material (una costura de UV o de
	// material deja dos vértices en el mismo sitio).
	struct Key {
		float p[3], uv[2];
		int material;
		bool operator==(const Key& o) const { return std::memcmp(this, &o, sizeof(Key)) == 0; }
	};
	struct KeyHash {
		std::size_t operator()(const Key& k) const { return std::size_t(hashBytes(&k, sizeof(Key))); }
	};
	std::unordered_map<Key, std::uint32_t, KeyHash> unique;
	unique.reserve(model.verts.size());
	std::vector<SkinVertex> verts;
	std::vector<std::uint32_t> tris(triCount * 3);
	std::vector<int> triMat(model.triMaterial);
	for (std::size_t i = 0; i < model.verts.size(); ++i) {
		const SkinVertex& v = model.verts[i];
		Key k;
		std::memset(&k, 0, sizeof(k));
		k.p[0] = v.p.x, k.p[1] = v.p.y, k.p[2] = v.p.z, k.uv[0] = v.uv.x, k.uv[1] = v.uv.y;
		k.material  = i / 3 < triMat.size() ? triMat[i / 3] : 0;
		const auto r = unique.emplace(k, std::uint32_t(verts.size()));
		if (r.second) verts.push_back(v);
		tris[i] = r.first->second;
	}
	const std::size_t vn = verts.size();

	// Vértices que no se mueven: los de un borde de la malla así indexada
	// (bordes de verdad y costuras de UV/material).
	std::unordered_map<std::uint64_t, int> edgeUse;
	edgeUse.reserve(tris.size());
	auto edgeKey = [](std::uint32_t a, std::uint32_t b) { return a < b ? (std::uint64_t(a) << 32 | b) : (std::uint64_t(b) << 32 | a); };
	for (std::size_t t = 0; t < triCount; ++t)
		for (int e = 0; e < 3; ++e) ++edgeUse[edgeKey(tris[t * 3 + std::size_t(e)], tris[t * 3 + std::size_t((e + 1) % 3)])];
	std::vector<char> locked(vn, 0);
	for (const auto& [k, n] : edgeUse) {
		if (n != 1) continue;
		locked[std::size_t(k >> 32)]         = 1;
		locked[std::size_t(k & 0xffffffffu)] = 1;
	}

	// Cuádricas y triángulos de cada vértice.
	std::vector<Quadric> q(vn);
	std::vector<std::vector<std::uint32_t>> vt(vn);
	for (std::size_t t = 0; t < triCount; ++t) {
		const Vec3& a = verts[tris[t * 3]].p;
		const Vec3& b = verts[tris[t * 3 + 1]].p;
		const Vec3& c = verts[tris[t * 3 + 2]].p;
		Vec3 n        = cross(b - a, c - a);
		const float l = length(n);
		if (l > 0) {
			n = n * (1.0f / l);
			for (int k = 0; k < 3; ++k) q[tris[t * 3 + std::size_t(k)]].addPlane(n.x, n.y, n.z, -dot(n, a), l);
		}
		for (int k = 0; k < 3; ++k) vt[tris[t * 3 + std::size_t(k)]].push_back(std::uint32_t(t));
	}

	std::vector<char> triAlive(triCount, 1);
	std::vector<char> vAlive(vn, 1);
	std::priority_queue<Collapse, std::vector<Collapse>, std::greater<Collapse>> heap;
	auto cost = [&](std::uint32_t from, std::uint32_t to) {
		Quadric s = q[from];
		s.add(q[to]);
		return s.eval(verts[to].p);
	};
	auto push = [&](std::uint32_t from, std::uint32_t to) {
		if (locked[from] || !vAlive[from] || !vAlive[to]) return;
		heap.push({ cost(from, to), from, to });
	};
	for (std::size_t t = 0; t < triCount; ++t)
		for (int e = 0; e < 3; ++e) {
			const std::uint32_t a = tris[t * 3 + std::size_t(e)], b = tris[t * 3 + std::size_t((e + 1) % 3)];
			push(a, b);
			push(b, a);
		}

	std::size_t alive = triCount;
	std::vector<std::uint32_t> around;
	// ¿Comparten algún triángulo vivo?
	auto neighbours = [&](std::uint32_t a, std::uint32_t b) {
		for (std::uint32_t t : vt[a]) {
			if (!triAlive[t]) continue;
			for (int k = 0; k < 3; ++k)
				if (tris[std::size_t(t) * 3 + std::size_t(k)] == b) return true;
		}
		return false;
	};
	// Ningún triángulo se da la vuelta ni se queda sin área.
	auto flipOk = [&](std::uint32_t from, std::uint32_t to) {
		for (std::uint32_t t : vt[from]) {
			if (!triAlive[t]) continue;
			const std::uint32_t* v = &tris[std::size_t(t) * 3];
			if (v[0] == to || v[1] == to || v[2] == to) continue;
			Vec3 p[3], np[3];
			for (int k = 0; k < 3; ++k) {
				p[k]  = verts[v[k]].p;
				np[k] = v[k] == from ? verts[to].p : p[k];
			}
			const Vec3 n0 = cross(p[1] - p[0], p[2] - p[0]), n1 = cross(np[1] - np[0], np[2] - np[0]);
			const float l0 = length(n0), l1 = length(n1);
			if (l1 <= l0 * 1e-3f || dot(n0, n1) < 0.3f * l0 * l1) return false;
		}
		return true;
	};
	// `from` pasa a ser `to`.
	auto collapse = [&](std::uint32_t from, std::uint32_t to) {
		for (std::uint32_t t : vt[from]) {
			if (!triAlive[t]) continue;
			std::uint32_t* v = &tris[std::size_t(t) * 3];
			if (v[0] == to || v[1] == to || v[2] == to) {
				triAlive[t] = 0;
				--alive;
				continue;
			}
			for (int k = 0; k < 3; ++k)
				if (v[k] == from) v[k] = to;
			vt[to].push_back(t);
		}
		vAlive[from] = 0;
		q[to].add(q[from]);
	};
	// Vecinos de `to`: costes nuevos (solo cambian sus aristas).
	auto refresh = [&](std::uint32_t to) {
		around.clear();
		auto& list = vt[to];
		list.erase(std::remove_if(list.begin(), list.end(), [&](std::uint32_t t) { return !triAlive[t]; }), list.end());
		for (std::uint32_t t : list)
			for (int k = 0; k < 3; ++k) around.push_back(tris[std::size_t(t) * 3 + std::size_t(k)]);
		std::sort(around.begin(), around.end());
		around.erase(std::unique(around.begin(), around.end()), around.end());
		for (std::uint32_t u : around) {
			if (u == to) continue;
			push(u, to);
			push(to, u);
		}
	};

	// Primera pasada: el interior de cada trozo de textura.
	while (alive > targetTris && !heap.empty()) {
		const Collapse c = heap.top();
		heap.pop();
		if (!vAlive[c.from] || !vAlive[c.to]) continue;
		// Coste viejo (una de las dos cuádricas ha crecido): a la cola otra vez.
		const double now = cost(c.from, c.to);
		if (now > c.cost * 1.0001 + 1e-12) {
			heap.push({ now, c.from, c.to });
			continue;
		}
		if (!neighbours(c.from, c.to) || !flipOk(c.from, c.to)) continue;
		collapse(c.from, c.to);
		refresh(c.to);
	}

	// Segunda pasada, si aún sobran (muchas costuras, como los de Mixamo o
	// Meshy): también las costuras, moviendo a la vez las copias de cada lado
	// para que no se abra una grieta ni se estropee la textura.
	if (alive > targetTris) {
		std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> twins;
		auto posKey = [&](std::uint32_t v) {
			std::uint32_t b[3];
			std::memcpy(b, &verts[v].p, sizeof(b));
			return hashBytes(b, sizeof(b));
		};
		for (std::uint32_t v = 0; v < vn; ++v)
			if (vAlive[v]) twins[posKey(v)].push_back(v);
		const std::vector<char> wasBorder = locked; // bordes y costuras de la primera pasada
		std::fill(locked.begin(), locked.end(), 0);
		heap = {};
		for (std::size_t t = 0; t < triCount; ++t) {
			if (!triAlive[t]) continue;
			for (int e = 0; e < 3; ++e) {
				const std::uint32_t a = tris[t * 3 + std::size_t(e)], b = tris[t * 3 + std::size_t((e + 1) % 3)];
				push(a, b);
				push(b, a);
			}
		}
		std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
		while (alive > targetTris && !heap.empty()) {
			const Collapse c = heap.top();
			heap.pop();
			if (!vAlive[c.from] || !vAlive[c.to]) continue;
			const double now = cost(c.from, c.to);
			if (now > c.cost * 1.0001 + 1e-12) {
				heap.push({ now, c.from, c.to });
				continue;
			}
			if (!neighbours(c.from, c.to)) continue;
			// Cada copia de `from` necesita una copia de `to` a su lado.
			const std::vector<std::uint32_t>& fromTwins = twins[posKey(c.from)];
			const std::vector<std::uint32_t>& toTwins   = twins[posKey(c.to)];
			pairs.clear();
			bool ok = true;
			for (std::uint32_t f : fromTwins) {
				if (!vAlive[f]) continue;
				std::uint32_t partner = UINT32_MAX;
				for (std::uint32_t t : toTwins)
					if (vAlive[t] && neighbours(f, t)) {
						partner = t;
						break;
					}
				if (partner == UINT32_MAX || !flipOk(f, partner)) {
					ok = false;
					break;
				}
				pairs.push_back({ f, partner });
			}
			// Un borde abierto (sin copias) solo se mueve por el propio borde,
			// hacia otro vértice del borde: así no se encoge el agujero.
			if (ok && fromTwins.size() == 1 && wasBorder[c.from]) {
				int shared = 0;
				for (std::uint32_t t : vt[c.from])
					if (triAlive[t]) {
						const std::uint32_t* v = &tris[std::size_t(t) * 3];
						shared += v[0] == c.to || v[1] == c.to || v[2] == c.to;
					}
				ok = shared == 1 && wasBorder[c.to];
			}
			if (!ok || pairs.empty()) continue;
			for (const auto& [f, t] : pairs) collapse(f, t);
			for (const auto& [f, t] : pairs) refresh(t);
		}
	}

	std::vector<SkinVertex> out;
	std::vector<int> outMat;
	out.reserve(alive * 3);
	for (std::size_t t = 0; t < triCount; ++t) {
		if (!triAlive[t]) continue;
		for (int k = 0; k < 3; ++k) out.push_back(verts[tris[t * 3 + std::size_t(k)]]);
		outMat.push_back(t < triMat.size() ? triMat[t] : 0);
	}
	model.verts       = std::move(out);
	model.triMaterial = std::move(outMat);
	return model.verts.size() / 3;
}

} // namespace mm
