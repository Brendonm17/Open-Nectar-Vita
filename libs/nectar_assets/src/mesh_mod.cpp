#include "nectar_assets/mesh_mod.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace nectar {

namespace {

struct Out {
	std::vector<std::uint8_t> b;
	std::size_t lenAt = 0;
	void u8_(std::uint8_t v) { b.push_back(v); }
	void u16_(int v)
	{
		b.push_back(std::uint8_t(v >> 8));
		b.push_back(std::uint8_t(v));
	}
	void u32_(std::uint32_t v)
	{
		for (int s = 24; s >= 0; s -= 8) b.push_back(std::uint8_t(v >> s));
	}
	void f32_(float f)
	{
		std::uint32_t v;
		std::memcpy(&v, &f, 4);
		u32_(v);
	}
	void vec(const Vec3& v)
	{
		f32_(v.x);
		f32_(v.y);
		f32_(v.z);
	}
	void pad32()
	{
		while (b.size() & 0x1F) b.push_back(0);
	}
	void begin(std::uint32_t type)
	{
		pad32();
		u32_(type);
		lenAt = b.size();
		u32_(0);
	}
	void end()
	{
		pad32();
		const std::uint32_t len = std::uint32_t(b.size() - lenAt - 4);
		for (int i = 0; i < 4; i++) b[lenAt + std::size_t(i)] = std::uint8_t(len >> (24 - 8 * i));
	}
};

int pow2Below(int v, int lo, int hi)
{
	int p = lo;
	while (p * 2 <= v && p * 2 <= hi) p *= 2;
	return p;
}

/// Lados en potencia de 2 (8..512), por media de cajas o vecino más cercano.
Image fitPow2(const Image& src)
{
	if (src.empty()) {
		Image white;
		white.resize(8, 8);
		std::fill(white.rgba.begin(), white.rgba.end(), std::uint8_t(255));
		return white;
	}
	const int w = pow2Below(src.width, 8, 512), h = pow2Below(src.height, 8, 512);
	Image out;
	out.resize(w, h);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const int x0 = x * src.width / w, x1 = std::max(x0 + 1, (x + 1) * src.width / w);
			const int y0 = y * src.height / h, y1 = std::max(y0 + 1, (y + 1) * src.height / h);
			unsigned sum[4] = {};
			int n           = 0;
			for (int sy = y0; sy < y1; sy++) {
				for (int sx = x0; sx < x1; sx++, n++) {
					const std::uint8_t* p = src.at(sx, sy);
					for (int c = 0; c < 4; c++) sum[c] += p[c];
				}
			}
			std::uint8_t* d = out.at(x, y);
			for (int c = 0; c < 4; c++) d[c] = std::uint8_t(sum[c] / unsigned(n));
		}
	}
	return out;
}

} // namespace

std::size_t modTextureBytes(int format, int width, int height)
{
	if (width <= 0 || height <= 0 || width > 1024 || height > 1024) return 0;
	auto blocks = [&](int bw, int bh, int bytes) {
		return std::size_t((width + bw - 1) / bw) * std::size_t((height + bh - 1) / bh) * std::size_t(bytes);
	};
	switch (format) {
	case MOD_TEX_RGB565:
	case MOD_TEX_RGB5A3:
	case MOD_TEX_IA8: return blocks(4, 4, 32);
	case MOD_TEX_CMPR: return blocks(8, 8, 32);
	case MOD_TEX_I4: return blocks(8, 8, 32);
	case MOD_TEX_I8:
	case MOD_TEX_IA4:
	case MOD_TEX_Z8: return blocks(8, 4, 32);
	case MOD_TEX_RGBA8: return blocks(4, 4, 64);
	default: return 0;
	}
}

std::vector<std::uint8_t> encodeRgb5a3(const Image& img)
{
	std::vector<std::uint8_t> out;
	out.reserve(std::size_t(img.width) * img.height * 2);
	for (int by = 0; by < img.height; by += 4) {
		for (int bx = 0; bx < img.width; bx += 4) {
			for (int y = by; y < by + 4; y++) {
				for (int x = bx; x < bx + 4; x++) {
					const std::uint8_t* p = img.at(std::min(x, img.width - 1), std::min(y, img.height - 1));
					std::uint16_t v;
					if (p[3] >= 0xE0) {
						v = std::uint16_t(0x8000 | ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3));
					} else {
						v = std::uint16_t(((p[3] >> 5) << 12) | ((p[0] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[2] >> 4));
					}
					out.push_back(std::uint8_t(v >> 8));
					out.push_back(std::uint8_t(v));
				}
			}
		}
	}
	return out;
}

bool writeMeshMod(const MeshModInput& in, std::vector<std::uint8_t>& out, std::string& error)
{
	if (in.tris.empty() || in.positions.empty()) {
		error = "la malla no tiene triángulos";
		return false;
	}
	// Normales y UV que faltan: una normal de cara por triángulo y una UV (0, 0).
	std::vector<Vec3> normals = in.normals;
	std::vector<Vec2> uvs     = in.uvs;
	const int zeroUv          = int(uvs.size());
	uvs.push_back(Vec2 {});
	std::vector<MeshModTri> tris = in.tris;
	for (MeshModTri& t : tris) {
		for (int k = 0; k < 3; k++) {
			if (t.pos[k] < 0 || std::size_t(t.pos[k]) >= in.positions.size()) {
				error = "un triángulo apunta a un vértice que no existe";
				return false;
			}
		}
		bool needNormal = false;
		for (int k = 0; k < 3; k++) needNormal = needNormal || t.nrm[k] < 0 || std::size_t(t.nrm[k]) >= in.normals.size();
		if (needNormal) {
			const Vec3 &a = in.positions[std::size_t(t.pos[0])], &b = in.positions[std::size_t(t.pos[1])], &c = in.positions[std::size_t(t.pos[2])];
			// Como el juego: cara de delante en sentido horario (n = (c-a) x (b-a)).
			const Vec3 u { c.x - a.x, c.y - a.y, c.z - a.z }, v { b.x - a.x, b.y - a.y, b.z - a.z };
			Vec3 n { u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
			const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
			if (l > 0) n = { n.x / l, n.y / l, n.z / l };
			else n = { 0, 1, 0 };
			const int ni = int(normals.size());
			normals.push_back(n);
			for (int k = 0; k < 3; k++) t.nrm[k] = ni;
		}
		for (int k = 0; k < 3; k++) {
			if (t.uv[k] < 0 || std::size_t(t.uv[k]) >= in.uvs.size()) t.uv[k] = zeroUv;
		}
		if (t.material < 0 || std::size_t(t.material) >= in.materials.size()) t.material = 0;
	}
	if (in.positions.size() > 0xFFFE || normals.size() > 0xFFFE || uvs.size() > 0xFFFE) {
		error = "la malla tiene demasiados vértices (más de 65534): divídela en partes";
		return false;
	}
	const std::size_t materialCount = std::max<std::size_t>(1, in.materials.size());

	Vec3 bmin { 1e30f, 1e30f, 1e30f }, bmax { -1e30f, -1e30f, -1e30f };
	for (const Vec3& p : in.positions) {
		bmin = { std::min(bmin.x, p.x), std::min(bmin.y, p.y), std::min(bmin.z, p.z) };
		bmax = { std::max(bmax.x, p.x), std::max(bmax.y, p.y), std::max(bmax.z, p.z) };
	}

	Out w;
	w.begin(0x00); // cabecera
	w.pad32();
	w.u32_(0);
	w.u32_(0); // flags de la forma
	w.end();

	w.begin(0x10);
	w.u32_(std::uint32_t(in.positions.size()));
	w.pad32();
	for (const Vec3& p : in.positions) w.vec(p);
	w.end();

	w.begin(0x11);
	w.u32_(std::uint32_t(normals.size()));
	w.pad32();
	for (const Vec3& n : normals) w.vec(n);
	w.end();

	w.begin(0x18);
	w.u32_(std::uint32_t(uvs.size()));
	w.pad32();
	for (const Vec2& t : uvs) {
		w.f32_(t.x);
		w.f32_(t.y);
	}
	w.end();

	// Texturas: una por material, RGB5A3.
	w.begin(0x20);
	w.u32_(std::uint32_t(materialCount));
	w.pad32();
	for (std::size_t m = 0; m < materialCount; m++) {
		const Image img                      = fitPow2(m < in.materials.size() ? in.materials[m].texture : Image());
		const std::vector<std::uint8_t> data = encodeRgb5a3(img);
		w.u16_(img.width);
		w.u16_(img.height);
		w.u32_(MOD_TEX_RGB5A3);
		w.u32_(1); // imágenes
		for (int k = 0; k < 4; k++) w.u32_(0);
		w.u32_(std::uint32_t(data.size()));
		w.b.insert(w.b.end(), data.begin(), data.end());
	}
	w.end();

	w.begin(0x22);
	w.u32_(std::uint32_t(materialCount));
	w.pad32();
	for (std::size_t m = 0; m < materialCount; m++) {
		w.u16_(int(m)); // textura
		w.u16_(0);
		w.u16_(0); // se repite en S y T
		w.u16_(0);
		w.f32_(0.0f);
	}
	w.end();

	// Materiales sencillos (sin TEV propio): opaco o con recorte.
	w.begin(0x30);
	w.u32_(std::uint32_t(materialCount));
	w.u32_(0);
	w.pad32();
	for (std::size_t m = 0; m < materialCount; m++) {
		const bool cut = m < in.materials.size() && in.materials[m].alphaTest;
		w.u32_(cut ? 0x200u : 0x100u);
		w.u32_(std::uint32_t(m)); // atributo de textura
		w.u32_(0xFFFFFFFFu);      // color
	}
	w.end();

	w.begin(0x40); // una matriz: la articulación 0
	w.u32_(1);
	w.pad32();
	w.u16_(0);
	w.end();

	// Una malla por material: posición y normal (con su matriz), UV 0.
	std::vector<int> meshMaterial;
	w.begin(0x50);
	std::vector<std::vector<const MeshModTri*>> byMat(materialCount);
	for (const MeshModTri& t : tris) byMat[std::size_t(t.material)].push_back(&t);
	std::size_t meshes = 0;
	for (const auto& list : byMat) meshes += list.empty() ? 0 : 1;
	w.u32_(std::uint32_t(meshes));
	w.pad32();
	for (std::size_t m = 0; m < materialCount; m++) {
		const auto& list = byMat[m];
		if (list.empty()) continue;
		meshMaterial.push_back(int(m));
		Out dl;
		std::size_t i = 0;
		while (i < list.size()) {
			const std::size_t n = std::min<std::size_t>(list.size() - i, 0xFFFF / 3);
			dl.u8_(0x90);
			dl.u16_(int(n * 3));
			for (std::size_t t = i; t < i + n; t++) {
				for (int k = 0; k < 3; k++) {
					dl.u8_(0);
					dl.u16_(list[t]->pos[k]);
					dl.u16_(list[t]->nrm[k]);
					dl.u16_(list[t]->uv[k]);
				}
			}
			i += n;
		}
		dl.u8_(0);
		dl.pad32();
		w.u32_(0);     // articulación padre
		w.u32_(1 | 8); // formato: matriz + posición/normal, UV 0
		w.u32_(1);     // grupos de matrices
		w.u32_(1);     // dependencias
		w.u16_(0);
		w.u32_(1); // listas de dibujo
		w.u32_(0);
		w.u32_(std::uint32_t(list.size()));
		w.u32_(std::uint32_t(dl.b.size()));
		w.pad32();
		w.b.insert(w.b.end(), dl.b.begin(), dl.b.end());
	}
	w.end();

	w.begin(0x60);
	w.u32_(1);
	w.pad32();
	w.u32_(0xFFFFFFFFu); // sin padre
	w.u32_(0);
	w.vec(bmin);
	w.vec(bmax);
	w.f32_(0.0f);
	w.vec({ 1, 1, 1 });
	w.vec({ 0, 0, 0 });
	w.vec({ 0, 0, 0 });
	w.u32_(std::uint32_t(meshMaterial.size()));
	for (std::size_t i = 0; i < meshMaterial.size(); i++) {
		w.u16_(meshMaterial[i]);
		w.u16_(int(i));
	}
	w.end();

	w.begin(0xFFFF);
	w.end();
	out.swap(w.b);
	return true;
}

std::string cutAsset(const std::string& mod, const Vec3& a, const Vec3& b, bool objectOnly)
{
	char buf[160];
	std::snprintf(buf, sizeof(buf), "|%.0f,%.0f,%.0f,%.0f,%.0f,%.0f", a.x, a.y, a.z, b.x, b.y, b.z);
	return "cut:" + mod + buf + (objectOnly ? "|obj" : "");
}

bool parseCutAsset(const std::string& asset, std::string& mod, Vec3& a, Vec3& b, bool* objectOnly)
{
	if (objectOnly) *objectOnly = asset.size() > 4 && asset.compare(asset.size() - 4, 4, "|obj") == 0;
	if (asset.rfind("cut:", 0) != 0) return false;
	const std::size_t bar = asset.find('|');
	if (bar == std::string::npos) return false;
	mod = asset.substr(4, bar - 4);
	if (mod.empty() || mod.find("..") != std::string::npos) return false;
	if (std::sscanf(asset.c_str() + bar + 1, "%f,%f,%f,%f,%f,%f", &a.x, &a.y, &a.z, &b.x, &b.y, &b.z) != 6) return false;
	if (a.x > b.x) std::swap(a.x, b.x);
	if (a.y > b.y) std::swap(a.y, b.y);
	if (a.z > b.z) std::swap(a.z, b.z);
	return true;
}

bool extractModRegion(const Model& src, const Vec3& lo, const Vec3& hi, std::vector<std::uint8_t>& out, ModRegionInfo& info, std::string& error,
                      bool objectOnly)
{
	info = ModRegionInfo();
	// Solo el objeto: piezas unidas por sus vértices; se queda la que más
	// triángulos tiene dentro de la caja.
	std::vector<int> part;
	int keepPart = -1;
	if (objectOnly) {
		part.resize(src.positions.size());
		for (std::size_t i = 0; i < part.size(); i++) part[i] = int(i);
		auto root = [&](int x) {
			while (part[std::size_t(x)] != x) x = part[std::size_t(x)] = part[std::size_t(part[std::size_t(x)])];
			return x;
		};
		auto valid = [&](int p) { return p >= 0 && std::size_t(p) < part.size(); };
		for (const ModMesh& mesh : src.meshes)
			for (const ModMtxGroup& g : mesh.groups)
				for (const ModTriangle& t : g.tris)
					for (int k = 1; k < 3; k++)
						if (valid(t.v[0].pos) && valid(t.v[k].pos)) {
							const int a = root(t.v[0].pos), b = root(t.v[k].pos);
							if (a != b) part[std::size_t(a)] = b;
						}
		std::map<int, int> count;
		for (const ModMesh& mesh : src.meshes)
			for (const ModMtxGroup& g : mesh.groups)
				for (const ModTriangle& t : g.tris) {
					if (!valid(t.v[0].pos) || !valid(t.v[1].pos) || !valid(t.v[2].pos)) continue;
					const Vec3 &p0 = src.positions[std::size_t(t.v[0].pos)], &p1 = src.positions[std::size_t(t.v[1].pos)], &p2 = src.positions[std::size_t(t.v[2].pos)];
					const Vec3 c { (p0.x + p1.x + p2.x) / 3, (p0.y + p1.y + p2.y) / 3, (p0.z + p1.z + p2.z) / 3 };
					if (c.x < lo.x || c.x > hi.x || c.y < lo.y || c.y > hi.y || c.z < lo.z || c.z > hi.z) continue;
					count[root(t.v[0].pos)]++;
				}
		int best = 0;
		for (const auto& kv : count)
			if (kv.second > best) {
				best     = kv.second;
				keepPart = kv.first;
			}
		for (std::size_t i = 0; i < part.size(); i++) part[i] = root(int(i));
	}
	const Vec3 origin { (lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f };
	const std::vector<Mat34> pose = src.bindPose();
	const std::vector<int> mats   = src.meshMaterials();

	struct Tri {
		int v[3], n[3], c[3], uv[8][3];
	};
	struct MeshOut {
		int material;
		std::uint32_t flags;
		std::vector<Tri> tris;
	};
	std::vector<Vec3> positions, normals;
	std::vector<Rgba8> colours;
	std::vector<Vec2> uvs[8];
	std::map<std::pair<int, int>, int> posMap, nrmMap;
	std::map<int, int> colMap, uvMap[8];
	std::map<int, int> matMap; // material del origen -> de salida
	std::vector<int> matOrder;
	std::vector<MeshOut> meshes;

	for (std::size_t mi = 0; mi < src.meshes.size(); mi++) {
		const int mat = mi < mats.size() ? mats[mi] : -1;
		if (mat < 0 || std::size_t(mat) >= src.materials.size()) continue;
		const ModMesh& mesh = src.meshes[mi];
		MeshOut mo { mat, (mesh.flags & 0x7FDu) | 1u, {} };
		for (const ModMtxGroup& g : mesh.groups) {
			for (const ModTriangle& t : g.tris) {
				Vec3 world[3];
				int key[3];
				Mat34 m[3];
				for (int k = 0; k < 3; k++) {
					const ModVertex& v = t.v[k];
					key[k]             = -1;
					if (v.mtxSlot >= 0 && std::size_t(v.mtxSlot) < g.deps.size()) {
						const int dep = g.deps[std::size_t(v.mtxSlot)];
						if (dep >= 0 && std::size_t(dep) < src.vtxMatrices.size() && src.vtxMatrices[std::size_t(dep)].joint) {
							key[k] = src.vtxMatrices[std::size_t(dep)].index;
							if (std::size_t(key[k]) < pose.size()) m[k] = pose[std::size_t(key[k])];
						}
					}
					if (v.pos < 0 || std::size_t(v.pos) >= src.positions.size()) goto skip;
					world[k] = m[k].point(src.positions[std::size_t(v.pos)]);
				}
				{
					const Vec3 c { (world[0].x + world[1].x + world[2].x) / 3, (world[0].y + world[1].y + world[2].y) / 3,
						           (world[0].z + world[1].z + world[2].z) / 3 };
					if (c.x < lo.x || c.x > hi.x || c.y < lo.y || c.y > hi.y || c.z < lo.z || c.z > hi.z) continue;
					if (objectOnly && part[std::size_t(t.v[0].pos)] != keepPart) continue;
				}
				{
					Tri o {};
					for (int k = 0; k < 3; k++) {
						const ModVertex& v = t.v[k];
						auto p             = posMap.find({ v.pos, key[k] });
						if (p == posMap.end()) {
							p = posMap.emplace(std::make_pair(v.pos, key[k]), int(positions.size())).first;
							positions.push_back({ world[k].x - origin.x, world[k].y - origin.y, world[k].z - origin.z });
						}
						o.v[k] = p->second;
						if (v.nrm >= 0 && std::size_t(v.nrm) < src.normals.size()) {
							auto n = nrmMap.find({ v.nrm, key[k] });
							if (n == nrmMap.end()) {
								Vec3 d        = m[k].dir(src.normals[std::size_t(v.nrm)]);
								const float l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
								if (l > 0) d = { d.x / l, d.y / l, d.z / l };
								n = nrmMap.emplace(std::make_pair(v.nrm, key[k]), int(normals.size())).first;
								normals.push_back(d);
							}
							o.n[k] = n->second;
						} else {
							o.n[k] = -1;
						}
						if (mo.flags & 4) {
							const int ci = v.col >= 0 && std::size_t(v.col) < src.colours.size() ? v.col : -1;
							auto c       = colMap.find(ci);
							if (c == colMap.end()) {
								c = colMap.emplace(ci, int(colours.size())).first;
								colours.push_back(ci >= 0 ? src.colours[std::size_t(ci)] : Rgba8 {});
							}
							o.c[k] = c->second;
						}
						for (int ch = 0; ch < 8; ch++) {
							if (!(mo.flags & (1u << (3 + ch)))) continue;
							const int ui = v.uv[ch] >= 0 && std::size_t(v.uv[ch]) < src.uvs[ch].size() ? v.uv[ch] : -1;
							auto u       = uvMap[ch].find(ui);
							if (u == uvMap[ch].end()) {
								u = uvMap[ch].emplace(ui, int(uvs[ch].size())).first;
								uvs[ch].push_back(ui >= 0 ? src.uvs[ch][std::size_t(ui)] : Vec2 {});
							}
							o.uv[ch][k] = u->second;
						}
					}
					// Sin normal: la de la cara (sentido del juego).
					if (o.n[0] < 0 || o.n[1] < 0 || o.n[2] < 0) {
						const Vec3 &a = positions[std::size_t(o.v[0])], &b = positions[std::size_t(o.v[1])], &c = positions[std::size_t(o.v[2])];
						const Vec3 u { c.x - a.x, c.y - a.y, c.z - a.z }, w { b.x - a.x, b.y - a.y, b.z - a.z };
						Vec3 n { u.y * w.z - u.z * w.y, u.z * w.x - u.x * w.z, u.x * w.y - u.y * w.x };
						const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
						n             = l > 0 ? Vec3 { n.x / l, n.y / l, n.z / l } : Vec3 { 0, 1, 0 };
						const int ni  = int(normals.size());
						normals.push_back(n);
						for (int k = 0; k < 3; k++) {
							if (o.n[k] < 0) o.n[k] = ni;
						}
					}
					mo.tris.push_back(o);
				}
			skip:;
			}
		}
		if (mo.tris.empty()) continue;
		if (!matMap.count(mat)) {
			matMap[mat] = int(matOrder.size());
			matOrder.push_back(mat);
		}
		info.triangles += mo.tris.size();
		meshes.push_back(std::move(mo));
	}
	if (meshes.empty()) {
		error = "no hay nada del escenario dentro de la caja";
		return false;
	}
	// La colisión del escenario (con sus códigos: agua, roca, hierba...),
	// de los triángulos con el centro en la caja. Sus vértices van detrás.
	struct CollOut {
		std::uint32_t code;
		int v[3];
		Vec3 normal;
	};
	std::vector<CollOut> collision;
	if (!objectOnly) {
		std::map<int, int> collPos;
		for (const ModCollTri& t : src.collision) {
			Vec3 p[3];
			bool ok = true;
			for (int k = 0; k < 3 && ok; k++) {
				ok = t.v[k] >= 0 && std::size_t(t.v[k]) < src.positions.size();
				if (ok) p[k] = src.positions[std::size_t(t.v[k])];
			}
			if (!ok) continue;
			const Vec3 c { (p[0].x + p[1].x + p[2].x) / 3, (p[0].y + p[1].y + p[2].y) / 3, (p[0].z + p[1].z + p[2].z) / 3 };
			if (c.x < lo.x || c.x > hi.x || c.y < lo.y || c.y > hi.y || c.z < lo.z || c.z > hi.z) continue;
			CollOut o { t.code, {}, t.normal };
			for (int k = 0; k < 3; k++) {
				auto it = collPos.find(t.v[k]);
				if (it == collPos.end()) {
					it = collPos.emplace(t.v[k], int(positions.size())).first;
					positions.push_back({ p[k].x - origin.x, p[k].y - origin.y, p[k].z - origin.z });
				}
				o.v[k] = it->second;
			}
			collision.push_back(o);
		}
	}
	bool tooBig = positions.size() > 0xFFFE || normals.size() > 0xFFFE || colours.size() > 0xFFFE;
	for (const auto& u : uvs) tooBig = tooBig || u.size() > 0xFFFE;
	if (tooBig) {
		error = "el recorte es demasiado grande: hazlo más pequeño";
		return false;
	}

	// Tablas: solo lo que usan los materiales del recorte, con índices nuevos.
	std::map<int, int> attrMap, texMap, tevMap;
	std::vector<int> attrOrder, texOrder, tevOrder;
	auto remember = [](std::map<int, int>& map, std::vector<int>& order, int v) {
		auto it = map.find(v);
		if (it != map.end()) return it->second;
		map[v] = int(order.size());
		order.push_back(v);
		return int(order.size()) - 1;
	};
	auto be32 = [](const std::vector<std::uint8_t>& b, std::size_t at) {
		return std::int32_t((std::uint32_t(b[at]) << 24) | (std::uint32_t(b[at + 1]) << 16) | (std::uint32_t(b[at + 2]) << 8) | b[at + 3]);
	};
	auto put32 = [](std::vector<std::uint8_t>& b, std::size_t at, std::int32_t v) {
		for (int i = 0; i < 4; i++) b[at + std::size_t(i)] = std::uint8_t(std::uint32_t(v) >> (24 - 8 * i));
	};
	std::vector<std::vector<std::uint8_t>> matRaw;
	for (int mat : matOrder) {
		const ModMaterial& m       = src.materials[std::size_t(mat)];
		std::vector<std::uint8_t> raw = m.raw;
		for (std::size_t ref : m.texAttrRefs) {
			const int v = be32(raw, ref);
			if (v >= 0 && std::size_t(v) < src.texAttrs.size()) put32(raw, ref, remember(attrMap, attrOrder, v));
		}
		if (m.tevInfoRef >= 0) {
			const int v = be32(raw, std::size_t(m.tevInfoRef));
			if (v >= 0 && std::size_t(v) < src.tevInfos.size()) put32(raw, std::size_t(m.tevInfoRef), remember(tevMap, tevOrder, v));
		}
		matRaw.push_back(raw);
	}
	std::vector<std::vector<std::uint8_t>> attrRaw;
	for (int a : attrOrder) {
		std::vector<std::uint8_t> raw = src.texAttrs[std::size_t(a)].raw;
		const int tex                 = src.texAttrs[std::size_t(a)].texture;
		const int nt                  = tex >= 0 && std::size_t(tex) < src.textures.size() ? remember(texMap, texOrder, tex) : tex;
		raw[0]                        = std::uint8_t(nt >> 8);
		raw[1]                        = std::uint8_t(nt);
		attrRaw.push_back(raw);
	}
	info.materials = matOrder.size();
	info.textures  = texOrder.size();

	Vec3 bmin { 1e30f, 1e30f, 1e30f }, bmax { -1e30f, -1e30f, -1e30f };
	for (const Vec3& p : positions) {
		bmin = { std::min(bmin.x, p.x), std::min(bmin.y, p.y), std::min(bmin.z, p.z) };
		bmax = { std::max(bmax.x, p.x), std::max(bmax.y, p.y), std::max(bmax.z, p.z) };
	}

	Out w;
	w.begin(0x00);
	w.pad32();
	w.u32_(0);
	w.u32_(0);
	w.end();
	w.begin(0x10);
	w.u32_(std::uint32_t(positions.size()));
	w.pad32();
	for (const Vec3& p : positions) w.vec(p);
	w.end();
	w.begin(0x11);
	w.u32_(std::uint32_t(normals.size()));
	w.pad32();
	for (const Vec3& n : normals) w.vec(n);
	w.end();
	if (!colours.empty()) {
		w.begin(0x13);
		w.u32_(std::uint32_t(colours.size()));
		w.pad32();
		for (const Rgba8& c : colours) {
			w.u8_(c.r);
			w.u8_(c.g);
			w.u8_(c.b);
			w.u8_(c.a);
		}
		w.end();
	}
	for (int ch = 0; ch < 8; ch++) {
		if (uvs[ch].empty()) continue;
		w.begin(0x18 + std::uint32_t(ch));
		w.u32_(std::uint32_t(uvs[ch].size()));
		w.pad32();
		for (const Vec2& t : uvs[ch]) {
			w.f32_(t.x);
			w.f32_(t.y);
		}
		w.end();
	}
	w.begin(0x20);
	w.u32_(std::uint32_t(texOrder.size()));
	w.pad32();
	for (int t : texOrder) w.b.insert(w.b.end(), src.textures[std::size_t(t)].raw.begin(), src.textures[std::size_t(t)].raw.end());
	w.end();
	w.begin(0x22);
	w.u32_(std::uint32_t(attrRaw.size()));
	w.pad32();
	for (const auto& r : attrRaw) w.b.insert(w.b.end(), r.begin(), r.end());
	w.end();
	w.begin(0x30);
	w.u32_(std::uint32_t(matRaw.size()));
	w.u32_(std::uint32_t(tevOrder.size()));
	w.pad32();
	for (int t : tevOrder) w.b.insert(w.b.end(), src.tevInfos[std::size_t(t)].begin(), src.tevInfos[std::size_t(t)].end());
	for (const auto& r : matRaw) w.b.insert(w.b.end(), r.begin(), r.end());
	w.end();
	w.begin(0x40);
	w.u32_(1);
	w.pad32();
	w.u16_(0);
	w.end();

	w.begin(0x50);
	w.u32_(std::uint32_t(meshes.size()));
	w.pad32();
	for (const MeshOut& mo : meshes) {
		Out dl;
		std::size_t i = 0;
		while (i < mo.tris.size()) {
			const std::size_t n = std::min<std::size_t>(mo.tris.size() - i, 0xFFFF / 3);
			dl.u8_(0x90);
			dl.u16_(int(n * 3));
			for (std::size_t t = i; t < i + n; t++) {
				for (int k = 0; k < 3; k++) {
					dl.u8_(0);
					dl.u16_(mo.tris[t].v[k]);
					dl.u16_(mo.tris[t].n[k]);
					if (mo.flags & 4) dl.u16_(mo.tris[t].c[k]);
					for (int ch = 0; ch < 8; ch++) {
						if (mo.flags & (1u << (3 + ch))) dl.u16_(mo.tris[t].uv[ch][k]);
					}
				}
			}
			i += n;
		}
		dl.u8_(0);
		dl.pad32();
		w.u32_(0);
		w.u32_(mo.flags);
		w.u32_(1);
		w.u32_(1);
		w.u16_(0);
		w.u32_(1);
		w.u32_(0);
		w.u32_(std::uint32_t(mo.tris.size()));
		w.u32_(std::uint32_t(dl.b.size()));
		w.pad32();
		w.b.insert(w.b.end(), dl.b.begin(), dl.b.end());
	}
	w.end();

	w.begin(0x60);
	w.u32_(1);
	w.pad32();
	w.u32_(0xFFFFFFFFu);
	w.u32_(0);
	w.vec(bmin);
	w.vec(bmax);
	w.f32_(0.0f);
	w.vec({ 1, 1, 1 });
	w.vec({ 0, 0, 0 });
	w.vec({ 0, 0, 0 });
	w.u32_(std::uint32_t(meshes.size()));
	for (std::size_t i = 0; i < meshes.size(); i++) {
		w.u16_(matMap[meshes[i].material]);
		w.u16_(int(i));
	}
	w.end();
	if (!collision.empty()) {
		w.begin(0x100);
		w.u32_(std::uint32_t(collision.size()));
		w.u32_(1); // una sala
		w.pad32();
		w.u32_(0);
		w.pad32();
		for (const CollOut& c : collision) {
			w.u32_(c.code);
			for (int k = 0; k < 3; k++) w.u32_(std::uint32_t(c.v[k]));
			w.u16_(0);
			for (int k = 0; k < 3; k++) w.u16_(0xFFFF);
			w.vec(c.normal);
			const Vec3& a = positions[std::size_t(c.v[0])];
			w.f32_(c.normal.x * a.x + c.normal.y * a.y + c.normal.z * a.z);
		}
		w.end();
	}
	w.begin(0xFFFF);
	w.end();
	out.swap(w.b);
	return true;
}

} // namespace nectar
