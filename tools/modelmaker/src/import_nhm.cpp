// Un pack ya hecho (model.nhm, ver export_nhm.cpp): sus huesos son las
// articulaciones de Olimar con sus pivotes propios, así que al volver a
// abrirlo se reconocen tal cual (rig.cpp, parseName) y no se pierde nada de lo
// que se ajustó, salvo el modelo original.

#include "import.h"

#include "lang.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace mm {

namespace {

// Padre de cada articulación de Olimar (nv3Model.mod / redModel.mod).
const int kParents[kJoints] = { -1, 0, 1, 2, 2, 4, 5, 1, 1, 0, 9, 9 };

struct Reader {
	const std::vector<std::uint8_t>& d;
	std::size_t at = 0;
	bool bad       = false;
	template <typename T>
	T get()
	{
		T v {};
		if (at + sizeof(T) > d.size()) {
			bad = true;
			return v;
		}
		std::memcpy(&v, &d[at], sizeof(T));
		at += sizeof(T);
		return v;
	}
};

} // namespace

bool importNhm(const std::string& path, ImpModel& out, std::string& error)
{
	std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
	std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(f)), {});
	Reader r { data };
	if (data.size() < 16 || std::memcmp(data.data(), "NHM1", 4) != 0) {
		error = L("no es un model.nhm", "not a model.nhm");
		return false;
	}
	r.at                 = 4;
	const unsigned version = r.get<std::uint32_t>(), boneCount = r.get<std::uint32_t>(), partCount = r.get<std::uint32_t>();
	if (version < 1 || version > 3 || boneCount == 0 || boneCount > 64 || partCount == 0 || partCount > 8) {
		error = L("model.nhm de una versión desconocida", "model.nhm of an unknown version");
		return false;
	}
	if (version >= 3) r.get<std::uint32_t>(); // flags del modelo
	out.format = "NHM";
	out.files.push_back(path);
	std::vector<int> jointOf;
	for (unsigned b = 0; b < boneCount && !r.bad; ++b) {
		const std::uint8_t len = r.get<std::uint8_t>();
		if (r.at + len > data.size()) {
			r.bad = true;
			break;
		}
		ImpBone bone;
		bone.name.assign(reinterpret_cast<const char*>(&data[r.at]), len);
		r.at += len;
		Mat34 inv;
		float m[16];
		for (float& v : m) v = r.get<float>();
		for (int row = 0; row < 3; ++row)
			for (int col = 0; col < 4; ++col) inv.m[row][col] = m[row * 4 + col];
		bone.head = translationOf(inverse(inv));
		int joint = -1;
		for (int j = 0; j < kJoints; ++j)
			if (packBoneName(j) && bone.name == packBoneName(j)) joint = j;
		jointOf.push_back(joint);
		out.bones.push_back(bone);
	}
	// Padres por la jerarquía de Olimar.
	for (std::size_t b = 0; b < out.bones.size(); ++b) {
		for (int p = jointOf[b] >= 0 ? kParents[jointOf[b]] : -1; p >= 0 && out.bones[b].parent < 0; p = kParents[p])
			for (std::size_t o = 0; o < out.bones.size(); ++o)
				if (jointOf[o] == p) out.bones[b].parent = int(o);
	}
	for (unsigned p = 0; p < partCount && !r.bad; ++p) {
		const unsigned verts = r.get<std::uint32_t>(), w = r.get<std::uint32_t>(), h = r.get<std::uint32_t>(), bytes = r.get<std::uint32_t>();
		const unsigned flags = version >= 2 ? r.get<std::uint32_t>() : 0;
		if (r.bad || std::size_t(bytes) != std::size_t(w) * h * 4 || r.at + std::size_t(verts) * 52 + bytes > data.size()) {
			r.bad = true;
			break;
		}
		const int material = int(out.materials.size());
		for (unsigned v = 0; v < verts; ++v) {
			SkinVertex sv;
			float fv[8];
			for (float& x : fv) x = r.get<float>();
			sv.p  = { fv[0], fv[1], fv[2] };
			sv.n  = { fv[3], fv[4], fv[5] };
			sv.uv = { fv[6], fv[7] };
			std::uint8_t j[4];
			for (std::uint8_t& x : j) x = r.get<std::uint8_t>();
			for (int k = 0; k < 4; ++k) {
				sv.joint[k]  = j[k] < out.bones.size() ? j[k] : 0;
				sv.weight[k] = r.get<float>();
			}
			out.verts.push_back(sv);
		}
		out.verts.resize(out.verts.size() - out.verts.size() % 3);
		while (out.triMaterial.size() < out.verts.size() / 3) out.triMaterial.push_back(material);
		ImpMaterial mat;
		mat.name = std::string(L("pieza ", "part ")) + std::to_string(p + 1);
		mat.texture.resize(int(w), int(h));
		std::memcpy(mat.texture.rgba.data(), &data[r.at], bytes);
		r.at += bytes;
		std::size_t partial = 0;
		for (std::size_t i = 3; i < mat.texture.rgba.size(); i += 4) partial += mat.texture.rgba[i] < 240;
		if (partial * 20 > mat.texture.rgba.size() / 4) mat.alpha = ImpMaterial::Blend;
		mat.doubleSided = (flags & 2) != 0;
		out.materials.push_back(std::move(mat));
	}
	if (r.bad || out.verts.empty()) {
		error = L("model.nhm dañado", "damaged model.nhm");
		return false;
	}
	out.skinned = true;
	return true;
}

} // namespace mm
