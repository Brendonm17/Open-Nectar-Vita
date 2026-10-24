// OBJ con el lector del editor de mapas (obj_import.cpp): sin huesos, así que
// los pesos los pone el creador de modelos.

#include "import.h"

#include "lang.h"
#include "obj_import.h"

#include <cctype>
#include <filesystem>

namespace mm {

bool importObjModel(const std::string& path, ImpModel& out, std::string& error)
{
	nectar::MeshModInput in;
	nm::ObjInfo info;
	if (!nm::readObj(path, 1.0f, in, info, error)) return false;
	out.format = "OBJ";
	// El lector no dice qué ha abierto: el .obj y lo que hay a su lado que
	// pueda ser suyo (.mtl e imágenes).
	{
		namespace fs = std::filesystem;
		out.files.push_back(path);
		std::error_code ec;
		for (const fs::directory_entry& e : fs::directory_iterator(fs::u8path(path).parent_path(), ec)) {
			if (!e.is_regular_file(ec) || e.path() == fs::u8path(path)) continue;
			std::string ext = e.path().extension().u8string();
			for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
			if (ext == ".mtl" || ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") out.files.push_back(e.path().u8string());
		}
	}
	for (const nectar::MeshModMaterial& m : in.materials) {
		ImpMaterial mat;
		mat.name    = "material" + std::to_string(out.materials.size());
		mat.texture = m.texture;
		mat.alpha   = m.alphaTest ? ImpMaterial::Mask : ImpMaterial::Opaque;
		out.materials.push_back(std::move(mat));
	}
	if (out.materials.empty()) out.materials.push_back(ImpMaterial { "default" });
	for (const nectar::MeshModTri& t : in.tris) {
		SkinVertex tv[3];
		bool ok = true;
		// El lector deja la cara de delante en sentido horario (como el juego);
		// aquí va antihoraria, como en glTF.
		const int order[3] = { 0, 2, 1 };
		for (int k = 0; k < 3; ++k) {
			const int i   = order[k];
			SkinVertex& o = tv[k];
			if (t.pos[i] < 0 || std::size_t(t.pos[i]) >= in.positions.size()) {
				ok = false;
				break;
			}
			o.p = in.positions[std::size_t(t.pos[i])];
			if (t.nrm[i] >= 0 && std::size_t(t.nrm[i]) < in.normals.size()) o.n = normalize(in.normals[std::size_t(t.nrm[i])]);
			if (t.uv[i] >= 0 && std::size_t(t.uv[i]) < in.uvs.size()) o.uv = in.uvs[std::size_t(t.uv[i])];
			o.weight[0] = 0;
		}
		if (!ok) continue;
		if (t.nrm[0] < 0) tv[0].n = tv[1].n = tv[2].n = normalize(cross(tv[1].p - tv[0].p, tv[2].p - tv[0].p));
		out.verts.insert(out.verts.end(), tv, tv + 3);
		out.triMaterial.push_back(t.material >= 0 && std::size_t(t.material) < out.materials.size() ? t.material : 0);
	}
	out.warnings = info.warnings;
	if (out.verts.empty()) {
		error = L("el OBJ no tiene triángulos", "the OBJ has no triangles");
		return false;
	}
	return true;
}

} // namespace mm
