#include "obj_import.h"

#include "nectar_mapcore/i18n.h"
#include "stb_image.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace nm {

using nectar::tr;
namespace fs = std::filesystem;

namespace {

struct ObjMaterial {
	float kd[3]     = { 0.8f, 0.8f, 0.8f };
	float alpha     = 1.0f;
	std::string map; ///< map_Kd
};

std::string trim(const std::string& s)
{
	std::size_t a = 0, b = s.size();
	while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
	return s.substr(a, b - a);
}

/// Lo que va tras la palabra clave (las rutas pueden llevar espacios; las
/// opciones "-o 1 1 1" de map_Kd se saltan y se queda lo último).
std::string restOf(const std::string& line, std::size_t keyLen)
{
	std::string rest = trim(line.substr(std::min(line.size(), keyLen)));
	while (!rest.empty() && rest[0] == '-') {
		std::istringstream in(rest);
		std::string opt, v;
		in >> opt;
		const int args = (opt == "-o" || opt == "-s" || opt == "-t") ? 3 : (opt == "-mm") ? 2 : 1;
		for (int k = 0; k < args; k++) in >> v;
		std::getline(in, rest);
		rest = trim(rest);
	}
	return rest;
}

void readMtl(const fs::path& path, std::map<std::string, ObjMaterial>& out)
{
	std::ifstream f(path);
	std::string line;
	ObjMaterial* cur = nullptr;
	while (std::getline(f, line)) {
		line = trim(line);
		if (line.rfind("newmtl", 0) == 0) {
			cur = &out[trim(line.substr(6))];
		} else if (!cur) {
			continue;
		} else if (line.rfind("Kd ", 0) == 0) {
			std::istringstream in(line.substr(3));
			in >> cur->kd[0] >> cur->kd[1] >> cur->kd[2];
		} else if (line.rfind("d ", 0) == 0) {
			cur->alpha = std::strtof(line.c_str() + 2, nullptr);
		} else if (line.rfind("Tr ", 0) == 0) {
			cur->alpha = 1.0f - std::strtof(line.c_str() + 3, nullptr);
		} else if (line.rfind("map_Kd", 0) == 0) {
			cur->map = restOf(line, 6);
		}
	}
}

nectar::Image solid(const float (&kd)[3], float alpha)
{
	nectar::Image img;
	img.resize(8, 8);
	for (int i = 0; i < 64; i++) {
		for (int c = 0; c < 3; c++) img.rgba[std::size_t(i) * 4 + std::size_t(c)] = std::uint8_t(std::clamp(kd[c], 0.0f, 1.0f) * 255.0f);
		img.rgba[std::size_t(i) * 4 + 3] = std::uint8_t(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
	}
	return img;
}

/// Índice de OBJ (desde 1; negativo = desde el final) a índice desde 0.
int objIndex(const std::string& s, std::size_t count)
{
	if (s.empty()) return -1;
	const long v = std::strtol(s.c_str(), nullptr, 10);
	if (v > 0) return int(v - 1);
	if (v < 0) return int(long(count) + v);
	return -1;
}

std::string safeName(const std::string& stem)
{
	std::string out;
	for (char c : stem) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? char(std::tolower(static_cast<unsigned char>(c))) : '_';
	return out.empty() ? "malla" : out;
}

} // namespace

bool readObj(const std::string& objPath, float scale, nectar::MeshModInput& out, ObjInfo& info, std::string& error)
{
	out  = nectar::MeshModInput();
	info = ObjInfo();
	std::ifstream f(objPath);
	if (!f) {
		error = tr("no se puede abrir el archivo");
		return false;
	}
	const fs::path dir = fs::path(objPath).parent_path();
	std::map<std::string, ObjMaterial> mtl;
	std::map<std::string, int> used; // material del OBJ -> índice en la salida
	std::vector<std::string> usedNames;
	int current = -1;
	auto materialIndex = [&](const std::string& name) {
		auto it = used.find(name);
		if (it != used.end()) return it->second;
		const int i = int(usedNames.size());
		used[name]  = i;
		usedNames.push_back(name);
		return i;
	};
	std::string line;
	std::size_t skipped = 0;
	while (std::getline(f, line)) {
		line = trim(line);
		if (line.empty() || line[0] == '#') continue;
		if (line.rfind("v ", 0) == 0) {
			nectar::Vec3 p;
			std::istringstream in(line.substr(2));
			in >> p.x >> p.y >> p.z;
			out.positions.push_back({ p.x * scale, p.y * scale, p.z * scale });
		} else if (line.rfind("vn ", 0) == 0) {
			nectar::Vec3 n;
			std::istringstream in(line.substr(3));
			in >> n.x >> n.y >> n.z;
			out.normals.push_back(n);
		} else if (line.rfind("vt ", 0) == 0) {
			nectar::Vec2 t;
			std::istringstream in(line.substr(3));
			in >> t.x >> t.y;
			out.uvs.push_back({ t.x, 1.0f - t.y }); // OBJ: origen abajo
		} else if (line.rfind("mtllib", 0) == 0) {
			readMtl(dir / restOf(line, 6), mtl);
		} else if (line.rfind("usemtl", 0) == 0) {
			current = materialIndex(trim(line.substr(6)));
		} else if (line.rfind("f ", 0) == 0) {
			if (current < 0) current = materialIndex("");
			std::istringstream in(line.substr(2));
			std::string corner;
			std::vector<std::array<int, 3>> poly;
			while (in >> corner) {
				std::string parts[3];
				int k = 0;
				for (char c : corner) {
					if (c == '/') {
						if (++k > 2) break;
					} else {
						parts[k] += c;
					}
				}
				poly.push_back({ objIndex(parts[0], out.positions.size()), objIndex(parts[1], out.uvs.size()), objIndex(parts[2], out.normals.size()) });
			}
			if (poly.size() < 3) {
				skipped++;
				continue;
			}
			// Abanico (vale para polígonos convexos, que es lo normal).
			for (std::size_t i = 1; i + 1 < poly.size(); i++) {
				nectar::MeshModTri t;
				const std::array<int, 3>* c[3] = { &poly[0], &poly[i], &poly[i + 1] };
				bool ok = true;
				for (int k = 0; k < 3; k++) {
					t.pos[k] = (*c[k])[0];
					t.uv[k]  = (*c[k])[1];
					t.nrm[k] = (*c[k])[2];
					ok       = ok && t.pos[k] >= 0 && std::size_t(t.pos[k]) < out.positions.size();
				}
				t.material = current;
				if (ok) out.tris.push_back(t);
				else skipped++;
			}
		}
	}
	if (out.tris.empty()) {
		error = tr("el archivo no tiene caras (¿es un .obj?)");
		return false;
	}
	// El juego dibuja la cara de delante en el sentido contrario al del OBJ.
	for (nectar::MeshModTri& t : out.tris) {
		std::swap(t.pos[1], t.pos[2]);
		std::swap(t.uv[1], t.uv[2]);
		std::swap(t.nrm[1], t.nrm[2]);
	}
	for (const std::string& name : usedNames) {
		nectar::MeshModMaterial m;
		const auto it        = mtl.find(name);
		const ObjMaterial om = it != mtl.end() ? it->second : ObjMaterial();
		if (!om.map.empty()) {
			int w = 0, h = 0, n = 0;
			const std::string file = (dir / om.map).string();
			if (unsigned char* px = stbi_load(file.c_str(), &w, &h, &n, 4)) {
				m.texture.resize(w, h);
				std::copy(px, px + std::size_t(w) * h * 4, m.texture.rgba.begin());
				stbi_image_free(px);
				info.textures++;
				for (std::size_t i = 3; i < m.texture.rgba.size() && !m.alphaTest; i += 4) m.alphaTest = m.texture.rgba[i] < 128;
			} else {
				info.warnings += tr("No se encuentra la textura ") + om.map + "\n";
			}
		}
		if (m.texture.empty()) m.texture = solid(om.kd, om.alpha);
		out.materials.push_back(m);
	}
	if (skipped) info.warnings += tr("Caras que no se pudieron leer: ") + std::to_string(skipped) + "\n";
	info.triangles = out.tris.size();
	info.vertices  = out.positions.size();
	info.materials = out.materials.size();
	info.min       = { 1e30f, 1e30f, 1e30f };
	info.max       = { -1e30f, -1e30f, -1e30f };
	for (const nectar::Vec3& p : out.positions) {
		info.min = { std::min(info.min.x, p.x), std::min(info.min.y, p.y), std::min(info.min.z, p.z) };
		info.max = { std::max(info.max.x, p.x), std::max(info.max.y, p.y), std::max(info.max.z, p.z) };
	}
	return true;
}

bool importObj(const std::string& objPath, const std::string& mapDir, float scale, std::string& asset, ObjInfo& info, std::string& error)
{
	nectar::MeshModInput mesh;
	if (!readObj(objPath, scale, mesh, info, error)) return false;
	std::vector<std::uint8_t> bytes;
	if (!nectar::writeMeshMod(mesh, bytes, error)) return false;
	std::error_code ec;
	const fs::path dir = fs::path(mapDir) / "meshes";
	fs::create_directories(dir, ec);
	const std::string name = safeName(fs::path(objPath).stem().string());
	const fs::path file    = dir / (name + ".mod");
	std::ofstream o(file, std::ios::binary);
	if (!o.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()))) {
		error = tr("no se puede guardar ") + file.string();
		return false;
	}
	asset = "map:meshes/" + name + ".mod";
	return true;
}

} // namespace nm
