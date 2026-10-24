// glTF 2.0 / GLB / VRM: lo justo para un personaje (mallas con piel, huesos,
// color base con su textura). Sin animaciones ni morph targets: el juego pone
// las suyas.

#include "import.h"

#include "lang.h"

#include "stb_image.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>

namespace mm {

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace {

// Lo más grande que se guarda de una textura al importar; al exportar se
// reduce otra vez (ver export_nhm.cpp).
constexpr int kImportTextureMax = 2048;

std::string lower(std::string s)
{
	for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

bool readAll(const fs::path& path, std::vector<std::uint8_t>& out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	out.assign(std::istreambuf_iterator<char>(f), {});
	return true;
}

std::vector<std::uint8_t> base64(const std::string& s)
{
	auto value = [](char c) -> int {
		if (c >= 'A' && c <= 'Z') return c - 'A';
		if (c >= 'a' && c <= 'z') return c - 'a' + 26;
		if (c >= '0' && c <= '9') return c - '0' + 52;
		if (c == '+' || c == '-') return 62;
		if (c == '/' || c == '_') return 63;
		return -1;
	};
	std::vector<std::uint8_t> out;
	unsigned buf = 0;
	int bits     = 0;
	for (char c : s) {
		const int v = value(c);
		if (v < 0) continue;
		buf = (buf << 6) | unsigned(v);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back(std::uint8_t((buf >> bits) & 0xff));
		}
	}
	return out;
}

std::string uriDecode(const std::string& s)
{
	std::string out;
	for (std::size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '%' && i + 2 < s.size()) {
			out += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
			i += 2;
		} else {
			out += s[i];
		}
	}
	return out;
}

// Matriz de un nodo (TRS o "matrix", por columnas en el archivo).
Mat34 nodeLocal(const json& node)
{
	Mat34 m;
	if (node.contains("matrix") && node["matrix"].size() == 16) {
		const json& a = node["matrix"];
		for (int row = 0; row < 3; ++row)
			for (int col = 0; col < 4; ++col) m.m[row][col] = a[std::size_t(col * 4 + row)].get<float>();
		return m;
	}
	Vec3 t, s { 1, 1, 1 };
	float q[4] = { 0, 0, 0, 1 };
	if (node.contains("translation")) t = { node["translation"][0].get<float>(), node["translation"][1].get<float>(), node["translation"][2].get<float>() };
	if (node.contains("scale")) s = { node["scale"][0].get<float>(), node["scale"][1].get<float>(), node["scale"][2].get<float>() };
	if (node.contains("rotation"))
		for (int i = 0; i < 4; ++i) q[i] = node["rotation"][std::size_t(i)].get<float>();
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	const float r[3][3] = {
		{ 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
		{ 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
		{ 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) },
	};
	const float sv[3] = { s.x, s.y, s.z };
	for (int row = 0; row < 3; ++row)
		for (int col = 0; col < 3; ++col) m.m[row][col] = r[row][col] * sv[col];
	setTranslation(m, t);
	return m;
}

struct Gltf {
	json doc;
	fs::path dir;
	std::vector<std::string>* files = nullptr;
	bool readExternal(const fs::path& p, std::vector<std::uint8_t>& out)
	{
		if (!readAll(p, out)) return false;
		if (files) files->push_back(p.u8string());
		return true;
	}
	std::vector<std::vector<std::uint8_t>> buffers;
	std::string warnings;

	void warn(const std::string& w)
	{
		if (warnings.find(w) == std::string::npos) warnings += w + "\n";
	}

	bool loadBuffers(const std::vector<std::uint8_t>& glbBin, std::string& error)
	{
		if (!doc.contains("buffers")) return true;
		for (const json& b : doc["buffers"]) {
			std::vector<std::uint8_t> data;
			if (!b.contains("uri")) {
				data = glbBin;
			} else {
				const std::string uri = b["uri"].get<std::string>();
				if (uri.rfind("data:", 0) == 0) {
					const std::size_t comma = uri.find(',');
					data                    = base64(comma == std::string::npos ? "" : uri.substr(comma + 1));
				} else if (!readExternal(dir / fs::u8path(uriDecode(uri)), data)) {
					error = std::string(L("falta el archivo de datos: ", "missing buffer file: ")) + uri;
					return false;
				}
			}
			buffers.push_back(std::move(data));
		}
		return true;
	}

	// Bytes de un bufferView (para imágenes).
	bool viewBytes(int index, std::vector<std::uint8_t>& out) const
	{
		if (!doc.contains("bufferViews") || index < 0 || std::size_t(index) >= doc["bufferViews"].size()) return false;
		const json& v           = doc["bufferViews"][std::size_t(index)];
		const std::size_t buf   = v.value("buffer", 0);
		const std::size_t off   = v.value("byteOffset", 0);
		const std::size_t len   = v.value("byteLength", 0);
		if (buf >= buffers.size() || off + len > buffers[buf].size()) return false;
		out.assign(buffers[buf].begin() + std::ptrdiff_t(off), buffers[buf].begin() + std::ptrdiff_t(off + len));
		return true;
	}

	// Un accessor como floats (`comps` por elemento). `normalizeInts` para los
	// enteros normalizados (UV, color, pesos); JOINTS va sin normalizar.
	bool accessor(int index, std::vector<float>& out, int& comps) const
	{
		out.clear();
		comps = 0;
		if (!doc.contains("accessors") || index < 0 || std::size_t(index) >= doc["accessors"].size()) return false;
		const json& a          = doc["accessors"][std::size_t(index)];
		const std::string type = a.value("type", "SCALAR");
		comps                  = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : type == "MAT4" ? 16 : 0;
		const int ct           = a.value("componentType", 5126);
		const std::size_t cnt  = a.value("count", 0);
		const bool norm        = a.value("normalized", false);
		const int csize        = (ct == 5120 || ct == 5121) ? 1 : (ct == 5122 || ct == 5123) ? 2 : 4;
		if (!comps) return false;
		out.assign(cnt * std::size_t(comps), 0.0f);
		if (!a.contains("bufferView")) return true; // todo ceros (o sparse, que no se usa en personajes)
		const json& v         = doc["bufferViews"][a["bufferView"].get<std::size_t>()];
		const std::size_t buf = v.value("buffer", 0);
		if (buf >= buffers.size()) return false;
		const std::vector<std::uint8_t>& b = buffers[buf];
		const std::size_t base = std::size_t(v.value("byteOffset", 0)) + std::size_t(a.value("byteOffset", 0));
		const std::size_t stride = v.value("byteStride", 0) ? std::size_t(v.value("byteStride", 0)) : std::size_t(csize * comps);
		for (std::size_t i = 0; i < cnt; ++i) {
			for (int c = 0; c < comps; ++c) {
				const std::size_t at = base + i * stride + std::size_t(c * csize);
				if (at + std::size_t(csize) > b.size()) return false;
				const std::uint8_t* p = &b[at];
				float f               = 0;
				switch (ct) {
				case 5126: std::memcpy(&f, p, 4); break;
				case 5121: f = norm ? p[0] / 255.0f : p[0]; break;
				case 5120: f = norm ? std::max(std::int8_t(p[0]) / 127.0f, -1.0f) : std::int8_t(p[0]); break;
				case 5123: {
					std::uint16_t u;
					std::memcpy(&u, p, 2);
					f = norm ? u / 65535.0f : u;
					break;
				}
				case 5122: {
					std::int16_t s;
					std::memcpy(&s, p, 2);
					f = norm ? std::max(s / 32767.0f, -1.0f) : s;
					break;
				}
				case 5125: {
					std::uint32_t u;
					std::memcpy(&u, p, 4);
					f = float(u);
					break;
				}
				default: return false;
				}
				out[i * std::size_t(comps) + std::size_t(c)] = f;
			}
		}
		return true;
	}

	nectar::Image image(int index)
	{
		nectar::Image img;
		if (!doc.contains("images") || index < 0 || std::size_t(index) >= doc["images"].size()) return img;
		const json& im = doc["images"][std::size_t(index)];
		std::vector<std::uint8_t> bytes;
		if (im.contains("bufferView")) {
			viewBytes(im["bufferView"].get<int>(), bytes);
		} else if (im.contains("uri")) {
			const std::string uri = im["uri"].get<std::string>();
			if (uri.rfind("data:", 0) == 0) {
				const std::size_t comma = uri.find(',');
				bytes                   = base64(comma == std::string::npos ? "" : uri.substr(comma + 1));
			} else if (!readExternal(dir / fs::u8path(uriDecode(uri)), bytes)) {
				warn(std::string(L("falta la textura: ", "missing texture: ")) + uri);
				return img;
			}
		}
		int w = 0, h = 0, n = 0;
		unsigned char* px = bytes.empty() ? nullptr : stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
		if (!px) {
			warn(std::string(L("textura ilegible (solo PNG, JPG, TGA, BMP): ", "unreadable texture (only PNG, JPG, TGA, BMP): ")) + im.value("name", im.value("uri", std::string("#") + std::to_string(index))));
			return img;
		}
		img.resize(w, h);
		std::memcpy(img.rgba.data(), px, std::size_t(w) * h * 4);
		stbi_image_free(px);
		if (w > kImportTextureMax || h > kImportTextureMax) img = nectar::shrinkToFit(img, kImportTextureMax, kImportTextureMax);
		return img;
	}
};

} // namespace

bool importGltf(const std::string& path, ImpModel& out, std::string& error)
{
	Gltf g;
	g.dir   = fs::u8path(path).parent_path();
	g.files = &out.files;
	out.files.push_back(path);
	std::vector<std::uint8_t> file, bin;
	if (!readAll(fs::u8path(path), file)) {
		error = L("no se puede leer el archivo", "cannot read the file");
		return false;
	}
	try {
		if (file.size() >= 12 && std::memcmp(file.data(), "glTF", 4) == 0) {
			std::size_t at = 12;
			std::string text;
			while (at + 8 <= file.size()) {
				std::uint32_t len, type;
				std::memcpy(&len, &file[at], 4);
				std::memcpy(&type, &file[at + 4], 4);
				at += 8;
				if (at + len > file.size()) break;
				if (type == 0x4E4F534A) text.assign(reinterpret_cast<const char*>(&file[at]), len);
				else if (type == 0x004E4942) bin.assign(file.begin() + std::ptrdiff_t(at), file.begin() + std::ptrdiff_t(at + len));
				at += (len + 3) & ~3u;
			}
			g.doc = json::parse(text);
		} else {
			g.doc = json::parse(file.begin(), file.end());
		}
	} catch (const std::exception& e) {
		error = std::string(L("no es un glTF válido (", "not a valid glTF file (")) + e.what() + ")";
		return false;
	}
	if (!g.loadBuffers(bin, error)) return false;
	const json& doc = g.doc;
	if (!doc.contains("nodes") || !doc.contains("meshes")) {
		error = L("el archivo no tiene mallas", "the file has no meshes");
		return false;
	}
	const std::size_t nodeCount = doc["nodes"].size();

	// Jerarquía y pose de reposo (la que tienen los nodos en el archivo).
	std::vector<int> parent(nodeCount, -1);
	for (std::size_t i = 0; i < nodeCount; ++i)
		for (const json& c : doc["nodes"][i].value("children", json::array()))
			if (c.get<std::size_t>() < nodeCount) parent[c.get<std::size_t>()] = int(i);
	std::vector<Mat34> world(nodeCount);
	std::vector<char> done(nodeCount, 0);
	std::function<void(std::size_t)> solve = [&](std::size_t n) {
		if (done[n]) return;
		done[n]            = 1;
		const Mat34 local  = nodeLocal(doc["nodes"][n]);
		const int p        = parent[n];
		if (p >= 0 && done[std::size_t(p)] != 1) {
			solve(std::size_t(p));
			world[n] = world[std::size_t(p)] * local;
		} else {
			world[n] = local;
		}
		done[n] = 2;
	};
	for (std::size_t i = 0; i < nodeCount; ++i) solve(i);

	// Huesos: todos los nodos que usa alguna piel.
	std::vector<int> boneOfNode(nodeCount, -1);
	if (doc.contains("skins")) {
		for (const json& skin : doc["skins"]) {
			for (const json& j : skin.value("joints", json::array())) {
				const std::size_t n = j.get<std::size_t>();
				if (n >= nodeCount || boneOfNode[n] >= 0) continue;
				boneOfNode[n] = int(out.bones.size());
				ImpBone b;
				b.name = doc["nodes"][n].value("name", "bone" + std::to_string(n));
				b.head = translationOf(world[n]);
				out.bones.push_back(b);
			}
		}
		for (std::size_t n = 0; n < nodeCount; ++n) {
			if (boneOfNode[n] < 0) continue;
			for (int p = parent[n]; p >= 0; p = parent[std::size_t(p)]) {
				if (boneOfNode[std::size_t(p)] >= 0) {
					out.bones[std::size_t(boneOfNode[n])].parent = boneOfNode[std::size_t(p)];
					break;
				}
			}
		}
	}
	// Huesos humanoides del VRM (0.x y 1.0): mapeo exacto, sin adivinar nombres.
	if (doc.contains("extensions")) {
		const json& ext = doc["extensions"];
		if (ext.contains("VRM")) {
			out.facesMinusZ = true; // VRM 0.x mira hacia -Z
			for (const json& hb : ext["VRM"].value("humanoid", json::object()).value("humanBones", json::array())) {
				const int n = hb.value("node", -1);
				if (n >= 0 && std::size_t(n) < nodeCount && boneOfNode[std::size_t(n)] >= 0)
					out.bones[std::size_t(boneOfNode[std::size_t(n)])].humanoid = hb.value("bone", "");
			}
			out.format = "VRM 0";
		} else if (ext.contains("VRMC_vrm")) {
			const json humanBones = ext["VRMC_vrm"].value("humanoid", json::object()).value("humanBones", json::object());
			for (const auto& [name, hb] : humanBones.items()) {
				const int n = hb.value("node", -1);
				if (n >= 0 && std::size_t(n) < nodeCount && boneOfNode[std::size_t(n)] >= 0) out.bones[std::size_t(boneOfNode[std::size_t(n)])].humanoid = name;
			}
			out.format = "VRM 1";
		}
	}
	if (out.format.empty()) out.format = file.size() >= 4 && std::memcmp(file.data(), "glTF", 4) == 0 ? "GLB" : "glTF";

	// Materiales.
	std::map<int, nectar::Image> images;
	auto textureImage = [&](int texIndex) -> nectar::Image {
		if (!doc.contains("textures") || texIndex < 0 || std::size_t(texIndex) >= doc["textures"].size()) return {};
		const json& t = doc["textures"][std::size_t(texIndex)];
		int src       = t.value("source", -1);
		if (src < 0 && t.contains("extensions")) {
			for (const char* e : { "EXT_texture_webp", "KHR_texture_basisu", "MSFT_texture_dds" })
				if (t["extensions"].contains(e)) g.warn(std::string(L("formato de textura no soportado (", "unsupported texture format (")) + e + L("): pasa las texturas a PNG", "): convert textures to PNG"));
		}
		if (src < 0) return {};
		auto it = images.find(src);
		if (it == images.end()) it = images.emplace(src, g.image(src)).first;
		return it->second;
	};
	std::vector<int> materialTexCoord;
	for (const json& m : doc.value("materials", json::array())) {
		ImpMaterial mat;
		mat.name        = m.value("name", "material" + std::to_string(out.materials.size()));
		mat.doubleSided = m.value("doubleSided", false);
		const std::string mode = m.value("alphaMode", "OPAQUE");
		mat.alpha              = mode == "BLEND" ? ImpMaterial::Blend : mode == "MASK" ? ImpMaterial::Mask : ImpMaterial::Opaque;
		mat.cutoff             = m.value("alphaCutoff", 0.5f);
		int texCoord           = 0;
		const json pbr         = m.value("pbrMetallicRoughness", json::object());
		const json sg          = m.contains("extensions") ? m["extensions"].value("KHR_materials_pbrSpecularGlossiness", json::object()) : json::object();
		const json factor      = pbr.contains("baseColorFactor") ? pbr["baseColorFactor"] : sg.value("diffuseFactor", json::array());
		if (factor.size() == 4)
			for (int i = 0; i < 4; ++i) mat.color[i] = factor[std::size_t(i)].get<float>();
		const json tex  = pbr.contains("baseColorTexture") ? pbr["baseColorTexture"] : sg.value("diffuseTexture", json::object());
		if (tex.contains("index")) {
			mat.texture = textureImage(tex["index"].get<int>());
			texCoord    = tex.value("texCoord", 0);
		}
		out.materials.push_back(std::move(mat));
		materialTexCoord.push_back(texCoord);
	}
	const int defaultMaterial = int(out.materials.size());
	out.materials.push_back(ImpMaterial { "default" });
	materialTexCoord.push_back(0);

	// Mallas de los nodos de la escena.
	std::vector<std::size_t> roots;
	const int sceneIndex = doc.value("scene", 0);
	if (doc.contains("scenes") && std::size_t(sceneIndex) < doc["scenes"].size()) {
		for (const json& n : doc["scenes"][std::size_t(sceneIndex)].value("nodes", json::array())) roots.push_back(n.get<std::size_t>());
	} else {
		for (std::size_t n = 0; n < nodeCount; ++n)
			if (parent[n] < 0) roots.push_back(n);
	}
	std::vector<std::size_t> meshNodes;
	std::function<void(std::size_t)> walk = [&](std::size_t n) {
		if (n >= nodeCount) return;
		if (doc["nodes"][n].contains("mesh")) meshNodes.push_back(n);
		for (const json& c : doc["nodes"][n].value("children", json::array())) walk(c.get<std::size_t>());
	};
	for (std::size_t r : roots) walk(r);

	std::vector<float> pos, nrm, uv, col, jnt, wgt, idx, ibm;
	for (std::size_t n : meshNodes) {
		const json& node    = doc["nodes"][n];
		const std::size_t m = node["mesh"].get<std::size_t>();
		if (m >= doc["meshes"].size()) continue;
		// Piel: matriz de cada hueso (mundo * inversa de reposo).
		std::vector<Mat34> jointMtx;
		std::vector<int> jointBone;
		if (node.contains("skin") && doc.contains("skins") && node["skin"].get<std::size_t>() < doc["skins"].size()) {
			const json& skin = doc["skins"][node["skin"].get<std::size_t>()];
			int comps        = 0;
			if (skin.contains("inverseBindMatrices")) g.accessor(skin["inverseBindMatrices"].get<int>(), ibm, comps);
			const json& joints = skin.value("joints", json::array());
			for (std::size_t i = 0; i < joints.size(); ++i) {
				const std::size_t jn = joints[i].get<std::size_t>();
				Mat34 inv;
				if (comps == 16 && (i + 1) * 16 <= ibm.size())
					for (int row = 0; row < 3; ++row)
						for (int c = 0; c < 4; ++c) inv.m[row][c] = ibm[i * 16 + std::size_t(c * 4 + row)];
				jointMtx.push_back(jn < nodeCount ? world[jn] * inv : inv);
				jointBone.push_back(jn < nodeCount ? boneOfNode[jn] : -1);
			}
		}
		// Malla rígida colgada de un hueso (gafas, sombrero...): va con él.
		int rigidBone = -1;
		for (int p = int(n); p >= 0 && rigidBone < 0; p = parent[std::size_t(p)]) rigidBone = boneOfNode[std::size_t(p)];

		for (const json& prim : doc["meshes"][m].value("primitives", json::array())) {
			const int mode = prim.value("mode", 4);
			if (mode != 4 && mode != 5 && mode != 6) continue;
			const json& at = prim.value("attributes", json::object());
			int pc = 0, nc = 0, uc = 0, cc = 0, jc = 0, wc = 0, ic = 0;
			if (!at.contains("POSITION") || !g.accessor(at["POSITION"].get<int>(), pos, pc) || pc != 3) continue;
			const std::size_t vcount = pos.size() / 3;
			int material             = prim.value("material", defaultMaterial);
			if (material < 0 || material >= int(out.materials.size())) material = defaultMaterial;
			const std::string uvName = "TEXCOORD_" + std::to_string(materialTexCoord[std::size_t(material)]);
			if (!at.contains("NORMAL") || !g.accessor(at["NORMAL"].get<int>(), nrm, nc) || nc != 3) nrm.clear();
			if (!at.contains(uvName) || !g.accessor(at[uvName].get<int>(), uv, uc) || uc != 2) uv.clear();
			if (!at.contains("COLOR_0") || !g.accessor(at["COLOR_0"].get<int>(), col, cc) || cc < 3) col.clear();
			const bool skinned = !jointMtx.empty() && at.contains("JOINTS_0") && at.contains("WEIGHTS_0")
			                  && g.accessor(at["JOINTS_0"].get<int>(), jnt, jc) && g.accessor(at["WEIGHTS_0"].get<int>(), wgt, wc) && jc == 4
			                  && wc == 4;
			if (!col.empty()) out.materials[std::size_t(material)].vertexColors = true;

			std::vector<std::uint32_t> order;
			if (prim.contains("indices") && g.accessor(prim["indices"].get<int>(), idx, ic)) {
				for (float f : idx) order.push_back(std::uint32_t(f));
			} else {
				for (std::size_t i = 0; i < vcount; ++i) order.push_back(std::uint32_t(i));
			}
			std::vector<std::uint32_t> tris;
			if (mode == 4) {
				tris = order;
			} else {
				for (std::size_t i = 2; i < order.size(); ++i) {
					if (mode == 5) {
						const bool odd = i & 1;
						tris.insert(tris.end(), { order[i - 2 + (odd ? 1 : 0)], order[i - 1 - (odd ? 1 : 0)], order[i] });
					} else {
						tris.insert(tris.end(), { order[0], order[i - 1], order[i] });
					}
				}
			}
			const Mat34& nodeWorld = world[n];
			for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
				SkinVertex tv[3];
				bool ok = true;
				for (int k = 0; k < 3; ++k) {
					const std::size_t v = tris[t + std::size_t(k)];
					if (v >= vcount) {
						ok = false;
						break;
					}
					SkinVertex& o = tv[k];
					const Vec3 p { pos[v * 3], pos[v * 3 + 1], pos[v * 3 + 2] };
					const Vec3 nn = nrm.empty() ? Vec3 {} : Vec3 { nrm[v * 3], nrm[v * 3 + 1], nrm[v * 3 + 2] };
					if (!uv.empty()) o.uv = { uv[v * 2], uv[v * 2 + 1] };
					if (!col.empty())
						for (int c = 0; c < cc && c < 4; ++c) o.col[c] = col[v * std::size_t(cc) + std::size_t(c)];
					o.weight[0] = 0;
					if (skinned) {
						Vec3 sp, sn;
						float total = 0;
						int used    = 0;
						for (int i = 0; i < 4; ++i) {
							const float w = wgt[v * 4 + std::size_t(i)];
							const int ji  = int(jnt[v * 4 + std::size_t(i)]);
							if (w <= 0 || ji < 0 || std::size_t(ji) >= jointMtx.size()) continue;
							sp += jointMtx[std::size_t(ji)].point(p) * w;
							sn += jointMtx[std::size_t(ji)].dir(nn) * w;
							total += w;
							if (jointBone[std::size_t(ji)] >= 0) {
								o.joint[used]  = jointBone[std::size_t(ji)];
								o.weight[used] = w;
								++used;
							}
						}
						if (total > 0) {
							o.p = sp * (1.0f / total);
							o.n = normalize(sn);
						} else {
							o.p = nodeWorld.point(p);
							o.n = normalize(nodeWorld.dir(nn));
						}
					} else {
						o.p = nodeWorld.point(p);
						o.n = normalize(nodeWorld.dir(nn));
						if (rigidBone >= 0) {
							o.joint[0]  = rigidBone;
							o.weight[0] = 1;
						}
					}
				}
				if (!ok) continue;
				// Escala negativa en el nodo: la cara queda del revés.
				const Mat34& wm = nodeWorld;
				const float det = wm.m[0][0] * (wm.m[1][1] * wm.m[2][2] - wm.m[1][2] * wm.m[2][1])
				                - wm.m[0][1] * (wm.m[1][0] * wm.m[2][2] - wm.m[1][2] * wm.m[2][0])
				                + wm.m[0][2] * (wm.m[1][0] * wm.m[2][1] - wm.m[1][1] * wm.m[2][0]);
				if (!skinned && det < 0) std::swap(tv[1], tv[2]);
				if (nrm.empty()) {
					const Vec3 fn = normalize(cross(tv[1].p - tv[0].p, tv[2].p - tv[0].p));
					tv[0].n = tv[1].n = tv[2].n = fn;
				}
				out.verts.insert(out.verts.end(), tv, tv + 3);
				out.triMaterial.push_back(material);
			}
		}
	}
	if (doc.contains("extensionsUsed"))
		for (const json& e : doc["extensionsUsed"])
			if (e.get<std::string>() == "KHR_draco_mesh_compression") g.warn(L("las mallas comprimidas con Draco no van: exporta sin compresión", "Draco-compressed meshes are not supported: export without compression"));
	out.skinned  = !out.bones.empty();
	out.warnings = g.warnings;
	if (out.verts.empty()) {
		error = L("el archivo no tiene triángulos", "no triangles found in the file");
		return false;
	}
	return true;
}

bool importModel(const std::string& path, ImpModel& out, std::string& error)
{
	out      = ImpModel();
	out.path = path;
	const std::string ext = lower(fs::u8path(path).extension().string());
	if (ext == ".gltf" || ext == ".glb" || ext == ".vrm") return importGltf(path, out, error);
	if (ext == ".obj") return importObjModel(path, out, error);
	if (ext == ".fbx") return importFbx(path, out, error);
	if (ext == ".nhm") return importNhm(path, out, error);
	if (ext == ".dae" || ext == ".blend" || ext == ".pmx" || ext == ".3ds") {
		error = L("DAE/BLEND/PMX: ábrelo en Blender y usa Archivo > Exportar > glTF 2.0 (.glb)", "DAE/BLEND/PMX: open it in Blender and use File > Export > glTF 2.0 (.glb)");
		return false;
	}
	error = L("formato desconocido (usa .glb, .gltf, .vrm, .fbx u .obj)", "unknown format (use .glb, .gltf, .vrm, .fbx or .obj)");
	return false;
}

} // namespace mm
