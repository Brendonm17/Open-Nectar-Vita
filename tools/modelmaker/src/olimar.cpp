#include "olimar.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

namespace mm {

namespace {

constexpr float kPi = 3.14159265358979f;

const char* const kPackBones[kJoints] = {
	"kosinull", "sebonjnt", "headjnt", nullptr, "happajnt1", "happajnt2", "happajnt3", "rhandjnt", "lhandjnt", "legcentre", "rlegjnt", "llegjnt",
};

// Lector big endian (los archivos son de GameCube).
struct Reader {
	const std::vector<std::uint8_t>& d;
	std::size_t at = 0;
	bool bad       = false;
	std::uint32_t u32()
	{
		if (at + 4 > d.size()) {
			bad = true;
			at  = d.size();
			return 0;
		}
		const std::uint32_t v = std::uint32_t(d[at]) << 24 | std::uint32_t(d[at + 1]) << 16 | std::uint32_t(d[at + 2]) << 8 | d[at + 3];
		at += 4;
		return v;
	}
	int s32() { return int(u32()); }
	float f32()
	{
		const std::uint32_t v = u32();
		float f;
		std::memcpy(&f, &v, 4);
		return f;
	}
	std::string str()
	{
		const int n = s32();
		if (n < 0 || at + std::size_t(n) > d.size()) {
			bad = true;
			return "";
		}
		std::string s(reinterpret_cast<const char*>(&d[at]), std::size_t(n));
		at += std::size_t(n);
		return s;
	}
	void floats(std::vector<float>& out)
	{
		const int n = s32();
		if (n < 0 || at + std::size_t(n) * 4 > d.size()) {
			bad = true;
			return;
		}
		out.resize(std::size_t(n));
		for (float& f : out) f = f32();
	}
};

float wrapAngle(float d)
{
	if (d > kPi) return d - 2 * kPi;
	if (d < -kPi) return d + 2 * kPi;
	return d;
}

// pcSampleDca (sysCommon/shapeBase.cpp): Catmull-Rom entre fotogramas.
float sampleDca(const std::vector<float>& data, const AnimParam& p, float time, bool angle, float fallback)
{
	if (p.count <= 0 || p.offset < 0 || std::size_t(p.offset + p.count) > data.size()) return fallback;
	const int last = p.count - 1;
	const int i0   = int(time);
	if (i0 >= last) return data[std::size_t(p.offset + last)];
	const float* k = data.data() + p.offset;
	const float p1 = k[i0];
	float d0       = p1 - k[i0 > 0 ? i0 - 1 : i0];
	float d1       = k[i0 + 1] - p1;
	float d2       = k[i0 + 2 <= last ? i0 + 2 : last] - k[i0 + 1];
	if (angle) {
		d0 = wrapAngle(d0);
		d1 = wrapAngle(d1);
		d2 = wrapAngle(d2);
	}
	const float q0 = p1 - d0, q2 = p1 + d1, q3 = q2 + d2;
	const float t = time - float(i0), t2 = t * t, t3 = t2 * t;
	return 0.5f * ((2 * p1) + (q2 - q0) * t + (2 * q0 - 5 * p1 + 4 * q2 - q3) * t2 + (3 * p1 - q0 - 3 * q2 + q3) * t3);
}

// extract() de shapeBase.cpp: Hermite entre claves (tiempo, valor, tangente(s)).
float sampleDck(const std::vector<float>& data, const AnimParam& p, float time, float fallback)
{
	if (p.count <= 0) return fallback;
	const int size = p.flags == 0 ? 3 : 4;
	if (p.offset < 0 || std::size_t(p.offset + size * p.count) > data.size()) return fallback;
	if (p.count == 1) return data[std::size_t(p.offset)];
	int off     = p.offset;
	bool active = false;
	for (int i = 0; i < p.count - 1; ++i) {
		if (data[std::size_t(off)] <= time && data[std::size_t(off + size)] >= time) {
			active = true;
			break;
		}
		off += size;
	}
	if (!active) return data[std::size_t(size * (p.count - 1) + p.offset + 1)];
	const float t0 = data[std::size_t(off)], v0 = data[std::size_t(off + 1)];
	const float g0 = data[std::size_t(off + (size == 3 ? 2 : 3))];
	const float t1 = data[std::size_t(off + size)], v1 = data[std::size_t(off + size + 1)], g1 = data[std::size_t(off + size + 2)];
	if (t1 <= t0) return v0;
	const float fps = 30.0f;
	const float t   = (time - t0) / fps;
	const float fd = fps / (t1 - t0), t2 = t * t, d2 = fd * fd, t3 = t2 * t, d3 = d2 * fd;
	return (2 * t3 * d3 - 3 * t2 * d2 + 1) * v0 + (-2 * t3 * d3 + 3 * t2 * d2) * v1 + (t3 * d2 - 2 * t2 * fd + t) * g0 + (t3 * d2 - t2 * fd) * g1;
}

bool readFile(const std::string& path, std::vector<std::uint8_t>& out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	out.assign(std::istreambuf_iterator<char>(f), {});
	return true;
}

std::string displayName(const std::string& path)
{
	std::string n = path;
	const std::size_t slash = n.find_last_of("/\\");
	if (slash != std::string::npos) n = n.substr(slash + 1);
	const std::size_t dot = n.find_last_of('.');
	if (dot != std::string::npos) n = n.substr(0, dot);
	return n;
}

} // namespace

const char* packBoneName(int joint) { return joint >= 0 && joint < kJoints ? kPackBones[joint] : nullptr; }

bool LeafModel::load(const std::string& path, std::string& error)
{
	nectar::Model mod;
	if (!nectar::loadMod(path, mod, error)) return false;
	verts.clear();
	triMaterial.clear();
	materialTex.assign(mod.materials.size(), {});
	materialColour.assign(mod.materials.size(), {});
	for (std::size_t i = 0; i < mod.materials.size(); ++i) {
		const nectar::ModMaterial& m = mod.materials[i];
		materialColour[i]            = m.colour;
		if (m.texAttr >= 0 && std::size_t(m.texAttr) < mod.texAttrs.size()) {
			const int t = mod.texAttrs[std::size_t(m.texAttr)].texture;
			if (t >= 0 && std::size_t(t) < mod.textures.size()) materialTex[i] = mod.textures[std::size_t(t)].decode();
		}
	}
	for (const nectar::BakedTri& t : nectar::bakeModel(mod)) {
		for (int k = 0; k < 3; ++k) {
			SkinVertex v;
			v.p  = t.p[k];
			v.n  = t.n[k];
			v.uv = t.uv[k];
			v.col[0] = t.col[k].r / 255.0f, v.col[1] = t.col[k].g / 255.0f, v.col[2] = t.col[k].b / 255.0f, v.col[3] = t.col[k].a / 255.0f;
			verts.push_back(v);
		}
		triMaterial.push_back(t.material);
	}
	return !verts.empty();
}

bool OlimarRig::load(const std::string& dataDir, const std::string& modName, std::string& error)
{
	nectar::Model mod;
	if (!nectar::loadMod(dataDir + "/" + modName, mod, error)) {
		error = modName + ": " + error;
		return false;
	}
	if (mod.joints.size() != std::size_t(kJoints)) {
		error = modName + ": " + std::to_string(mod.joints.size()) + " joints (expected 12)";
		return false;
	}
	bind = mod.bindPose();
	parent.clear();
	for (const nectar::ModJoint& j : mod.joints) parent.push_back(j.parent);

	// Materiales: textura base y color (sin TEV; es para tener a Olimar al lado).
	materialTex.assign(mod.materials.size(), {});
	materialColour.assign(mod.materials.size(), {});
	for (std::size_t i = 0; i < mod.materials.size(); ++i) {
		const nectar::ModMaterial& m = mod.materials[i];
		materialColour[i]            = m.colour;
		if (m.texAttr >= 0 && std::size_t(m.texAttr) < mod.texAttrs.size()) {
			const int t = mod.texAttrs[std::size_t(m.texAttr)].texture;
			if (t >= 0 && std::size_t(t) < mod.textures.size()) materialTex[i] = mod.textures[std::size_t(t)].decode();
		}
	}

	// Como bakeModel, pero guardando los pesos de cada vértice.
	verts.clear();
	triMaterial.clear();
	const std::vector<int> mats = mod.meshMaterials();
	struct Slot {
		Mat34 m;
		std::vector<std::pair<int, float>> w;
	};
	std::vector<Slot> slots(16);
	for (std::size_t mi = 0; mi < mod.meshes.size(); ++mi) {
		const int mat = mats[mi];
		if (mat < 0 || std::size_t(mat) >= mod.materials.size()) continue;
		if (mod.materials[std::size_t(mat)].flags & nectar::MOD_MAT_SKIP) continue;
		const nectar::ModMesh& mesh = mod.meshes[mi];
		for (const nectar::ModMtxGroup& g : mesh.groups) {
			for (std::size_t sl = 0; sl < g.deps.size() && sl < slots.size(); ++sl) {
				const int dep = g.deps[sl];
				if (dep < 0 || std::size_t(dep) >= mod.vtxMatrices.size()) continue;
				const nectar::ModVtxMatrix& vm = mod.vtxMatrices[std::size_t(dep)];
				Slot& s                        = slots[sl];
				if (vm.joint && vm.index >= 0 && vm.index < kJoints) {
					s.m = bind[std::size_t(vm.index)];
					s.w = { { vm.index, 1.0f } };
				} else if (!vm.joint && std::size_t(vm.index) < mod.envelopes.size()) {
					s.m = Mat34();
					s.w = mod.envelopes[std::size_t(vm.index)].weights;
				}
			}
			for (const nectar::ModTriangle& t : g.tris) {
				SkinVertex tri[3];
				bool ok = true;
				for (int k = 0; k < 3 && ok; ++k) {
					const nectar::ModVertex& v = t.v[k];
					SkinVertex& out            = tri[k];
					Slot s;
					const int slot = v.mtxSlot >= 0 ? v.mtxSlot : 0;
					if (!g.deps.empty() && std::size_t(slot) < slots.size()) {
						s = slots[std::size_t(slot)];
					} else {
						const int j = mesh.parentJoint >= 0 && mesh.parentJoint < kJoints ? mesh.parentJoint : 0;
						s.m         = bind[std::size_t(j)];
						s.w         = { { j, 1.0f } };
					}
					if (v.pos < 0 || std::size_t(v.pos) >= mod.positions.size() || s.w.empty()) {
						ok = false;
						break;
					}
					out.p = s.m.point(mod.positions[std::size_t(v.pos)]);
					if (v.nrm >= 0 && std::size_t(v.nrm) < mod.normals.size()) out.n = normalize(s.m.dir(mod.normals[std::size_t(v.nrm)]));
					if (v.uv[0] >= 0 && std::size_t(v.uv[0]) < mod.uvs[0].size()) out.uv = mod.uvs[0][std::size_t(v.uv[0])];
					if (v.col >= 0 && std::size_t(v.col) < mod.colours.size()) {
						const nectar::Rgba8 c = mod.colours[std::size_t(v.col)];
						out.col[0] = c.r / 255.0f, out.col[1] = c.g / 255.0f, out.col[2] = c.b / 255.0f, out.col[3] = c.a / 255.0f;
					}
					for (int i = 0; i < 4; ++i) {
						out.joint[i]  = i < int(s.w.size()) ? s.w[std::size_t(i)].first : 0;
						out.weight[i] = i < int(s.w.size()) ? s.w[std::size_t(i)].second : 0.0f;
					}
				}
				if (!ok) continue;
				// Sin normales: la de la cara.
				if (t.v[0].nrm < 0) tri[0].n = tri[1].n = tri[2].n = normalize(cross(tri[1].p - tri[0].p, tri[2].p - tri[0].p));
				verts.insert(verts.end(), tri, tri + 3);
				triMaterial.push_back(mat);
			}
		}
	}
	if (verts.empty()) {
		error = modName + ": no triangles";
		return false;
	}
	min = max = verts[0].p;
	bodyTop   = 0;
	for (const SkinVertex& v : verts) {
		min = vmin(min, v.p);
		max = vmax(max, v.p);
		int main = 0;
		for (int i = 1; i < 4; ++i)
			if (v.weight[i] > v.weight[main]) main = i;
		const int j = v.joint[main];
		if (j != J_ANT1 && j != J_ANT2 && j != J_ANT3) bodyTop = std::max(bodyTop, v.p.y);
	}
	return true;
}

Vec3 OlimarRig::jointCentroid(int joint) const
{
	Vec3 sum;
	float total = 0;
	for (const SkinVertex& v : verts) {
		for (int i = 0; i < 4; ++i) {
			if (v.joint[i] == joint && v.weight[i] > 0) {
				sum += v.p * v.weight[i];
				total += v.weight[i];
			}
		}
	}
	return total > 0 ? sum * (1.0f / total) : pivot(joint);
}

void Anim::pose(float frame, const std::vector<int>& parent, std::vector<Mat34>& world) const
{
	world.assign(parent.size(), Mat34());
	if (frames > 0) frame = std::fmod(std::max(frame, 0.0f), float(frames));
	for (std::size_t j = 0; j < parent.size(); ++j) {
		Vec3 s { 1, 1, 1 }, r, t;
		if (j < joints.size()) {
			const AnimJoint& a = joints[j];
			float* sv[3]       = { &s.x, &s.y, &s.z };
			float* rv[3]       = { &r.x, &r.y, &r.z };
			float* tv[3]       = { &t.x, &t.y, &t.z };
			for (int i = 0; i < 3; ++i) {
				if (keyed) {
					*sv[i] = sampleDck(sData, a.s[i], frame, 1.0f);
					*rv[i] = sampleDck(rData, a.r[i], frame, 0.0f);
					*tv[i] = sampleDck(tData, a.t[i], frame, 0.0f);
				} else {
					*sv[i] = sampleDca(sData, a.s[i], frame, false, 1.0f);
					*rv[i] = sampleDca(rData, a.r[i], frame, true, 0.0f);
					*tv[i] = sampleDca(tData, a.t[i], frame, false, 0.0f);
				}
			}
		}
		const Mat34 local = Mat34::srt(s, r, t);
		const int p       = parent[j];
		world[j]          = p >= 0 && std::size_t(p) < j ? world[std::size_t(p)] * local : local;
	}
}

bool loadAnimBundle(const std::string& path, std::vector<Anim>& out, std::string& error)
{
	std::vector<std::uint8_t> data;
	if (!readFile(path, data)) {
		error = path + ": not found";
		return false;
	}
	out.clear();
	Reader r { data };
	const int files = r.s32();
	for (int f = 0; f < files && !r.bad; ++f) {
		const int type = r.s32();
		const int size = r.s32();
		Anim a;
		a.path                  = r.str();
		const std::size_t start = r.at;
		if (type == 2 || type == 3) { // DCA, DCK (StdSystem::loadBundle)
			a.keyed      = type == 3;
			const int jc = r.s32();
			a.frames     = r.s32();
			r.floats(a.sData);
			r.floats(a.rData);
			r.floats(a.tData);
			if (jc < 0 || jc > 256) r.bad = true;
			for (int j = 0; j < jc && !r.bad; ++j) {
				AnimJoint aj;
				r.s32(); // grupo
				r.s32(); // padre (el de la forma manda)
				for (AnimParam* set : { aj.s, aj.r, aj.t }) {
					for (int i = 0; i < 3; ++i) {
						set[i].count  = r.s32();
						set[i].offset = r.s32();
						if (a.keyed) set[i].flags = r.s32();
					}
				}
				a.joints.push_back(aj);
			}
			a.name = displayName(a.path);
			if (!r.bad && jc == kJoints && a.frames > 0) out.push_back(std::move(a));
		} else {
			r.at = std::min(data.size(), start + std::size_t(std::max(size, 0)));
		}
	}
	if (out.empty()) {
		error = path + ": no animations";
		return false;
	}
	return true;
}

} // namespace mm
