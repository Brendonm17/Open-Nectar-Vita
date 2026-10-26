// Mallas en Collada (.dae): lo que sacan muchos extractores de modelos de
// juegos y Blender. Se lee lo justo para pasarlo al mismo .mod que un .obj:
// la escena (nodos con sus transformaciones), sus mallas (triangles, polylist
// o polygons), una UV y la textura difusa de cada material.

#include "obj_import.h"

#include "nectar_mapcore/i18n.h"
#include "stb_image.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace nm {

using nectar::tr;
namespace fs = std::filesystem;

namespace {

// ── XML mínimo ──────────────────────────────────────────────────────────────

struct XmlNode {
	std::string name, text;
	std::map<std::string, std::string> attrs;
	std::vector<std::unique_ptr<XmlNode>> children;

	const XmlNode* child(const char* n) const
	{
		for (const auto& c : children) {
			if (c->name == n) return c.get();
		}
		return nullptr;
	}
	std::string attr(const char* n) const
	{
		const auto it = attrs.find(n);
		return it != attrs.end() ? it->second : std::string();
	}
};

std::string unescape(const std::string& s)
{
	if (s.find('&') == std::string::npos) return s;
	std::string out;
	for (std::size_t i = 0; i < s.size(); i++) {
		if (s[i] != '&') {
			out += s[i];
			continue;
		}
		const std::size_t semi = s.find(';', i);
		if (semi == std::string::npos) {
			out += s[i];
			continue;
		}
		const std::string e = s.substr(i + 1, semi - i - 1);
		if (e == "amp") out += '&';
		else if (e == "lt") out += '<';
		else if (e == "gt") out += '>';
		else if (e == "quot") out += '"';
		else if (e == "apos") out += '\'';
		else if (!e.empty() && e[0] == '#') out += char(std::strtol(e.c_str() + (e.size() > 1 && e[1] == 'x' ? 2 : 1), nullptr, e.size() > 1 && e[1] == 'x' ? 16 : 10));
		else out += "&" + e + ";";
		i = semi;
	}
	return out;
}

/// El árbol entero (sin espacios de nombres: "x:y" se queda en "y").
bool parseXml(const std::string& s, XmlNode& root)
{
	std::vector<XmlNode*> stack { &root };
	std::size_t i = 0;
	while (i < s.size()) {
		if (s[i] != '<') {
			const std::size_t next = s.find('<', i);
			stack.back()->text += unescape(s.substr(i, (next == std::string::npos ? s.size() : next) - i));
			i = next == std::string::npos ? s.size() : next;
			continue;
		}
		if (s.compare(i, 4, "<!--") == 0) {
			const std::size_t e = s.find("-->", i);
			i = e == std::string::npos ? s.size() : e + 3;
		} else if (s.compare(i, 9, "<![CDATA[") == 0) {
			const std::size_t e = s.find("]]>", i);
			stack.back()->text += s.substr(i + 9, (e == std::string::npos ? s.size() : e) - i - 9);
			i = e == std::string::npos ? s.size() : e + 3;
		} else if (s.compare(i, 2, "<?") == 0 || s.compare(i, 2, "<!") == 0) {
			const std::size_t e = s.find('>', i);
			i = e == std::string::npos ? s.size() : e + 1;
		} else if (s.compare(i, 2, "</") == 0) {
			const std::size_t e = s.find('>', i);
			if (stack.size() > 1) stack.pop_back();
			i = e == std::string::npos ? s.size() : e + 1;
		} else {
			std::size_t j = i + 1;
			while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j])) && s[j] != '>' && s[j] != '/') j++;
			auto node    = std::make_unique<XmlNode>();
			std::string name = s.substr(i + 1, j - i - 1);
			const std::size_t colon = name.find(':');
			node->name = colon == std::string::npos ? name : name.substr(colon + 1);
			bool selfClose = false;
			while (j < s.size()) {
				while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) j++;
				if (j >= s.size()) return false;
				if (s[j] == '>') {
					j++;
					break;
				}
				if (s[j] == '/') {
					selfClose = true;
					j++;
					continue;
				}
				std::size_t k = j;
				while (k < s.size() && s[k] != '=' && s[k] != '>' && !std::isspace(static_cast<unsigned char>(s[k]))) k++;
				const std::string key = s.substr(j, k - j);
				while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) k++;
				if (k < s.size() && s[k] == '=') {
					k++;
					while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) k++;
					if (k >= s.size()) return false;
					const char q        = s[k];
					const std::size_t e = s.find(q, k + 1);
					if (e == std::string::npos) return false;
					node->attrs[key] = unescape(s.substr(k + 1, e - k - 1));
					j                = e + 1;
				} else {
					j = k;
				}
			}
			XmlNode* raw = node.get();
			stack.back()->children.push_back(std::move(node));
			if (!selfClose) stack.push_back(raw);
			i = j;
		}
	}
	return true;
}

std::vector<float> floats(const std::string& s)
{
	std::vector<float> out;
	const char* p = s.c_str();
	char* end     = nullptr;
	while (*p) {
		const float v = std::strtof(p, &end);
		if (end == p) {
			p++;
			continue;
		}
		out.push_back(v);
		p = end;
	}
	return out;
}

std::vector<int> ints(const std::string& s)
{
	std::vector<int> out;
	const char* p = s.c_str();
	char* end     = nullptr;
	while (*p) {
		const long v = std::strtol(p, &end, 10);
		if (end == p) {
			p++;
			continue;
		}
		out.push_back(int(v));
		p = end;
	}
	return out;
}

// ── Transformaciones ────────────────────────────────────────────────────────

struct Mat4 {
	float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }; // por filas, como <matrix>

	Mat4 operator*(const Mat4& b) const
	{
		Mat4 r;
		for (int i = 0; i < 4; i++) {
			for (int j = 0; j < 4; j++) {
				float v = 0;
				for (int k = 0; k < 4; k++) v += m[i * 4 + k] * b.m[k * 4 + j];
				r.m[i * 4 + j] = v;
			}
		}
		return r;
	}
	nectar::Vec3 point(const nectar::Vec3& p) const
	{
		return { m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7], m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11] };
	}
	nectar::Vec3 dir(const nectar::Vec3& p) const
	{
		return { m[0] * p.x + m[1] * p.y + m[2] * p.z, m[4] * p.x + m[5] * p.y + m[6] * p.z, m[8] * p.x + m[9] * p.y + m[10] * p.z };
	}
	float det3() const
	{
		return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
	}
};

Mat4 nodeTransform(const XmlNode& node)
{
	Mat4 t;
	for (const auto& c : node.children) {
		const std::vector<float> v = floats(c->text);
		Mat4 s;
		if (c->name == "matrix" && v.size() >= 16) {
			std::copy(v.begin(), v.begin() + 16, s.m);
		} else if (c->name == "translate" && v.size() >= 3) {
			s.m[3] = v[0], s.m[7] = v[1], s.m[11] = v[2];
		} else if (c->name == "scale" && v.size() >= 3) {
			s.m[0] = v[0], s.m[5] = v[1], s.m[10] = v[2];
		} else if (c->name == "rotate" && v.size() >= 4) {
			float x = v[0], y = v[1], z = v[2];
			const float len = std::sqrt(x * x + y * y + z * z);
			if (len < 1e-6f) continue;
			x /= len, y /= len, z /= len;
			const float a = v[3] * 3.14159265f / 180.0f, c0 = std::cos(a), s0 = std::sin(a), k = 1 - c0;
			const float r[9] = { c0 + x * x * k,     x * y * k - z * s0, x * z * k + y * s0, y * x * k + z * s0, c0 + y * y * k,
				                 y * z * k - x * s0, z * x * k - y * s0, z * y * k + x * s0, c0 + z * z * k };
			for (int i = 0; i < 3; i++) {
				for (int j = 0; j < 3; j++) s.m[i * 4 + j] = r[i * 3 + j];
			}
		} else {
			continue;
		}
		t = t * s;
	}
	return t;
}

// ── Lectura ─────────────────────────────────────────────────────────────────

struct Source {
	std::vector<float> data;
	int stride = 1;
};

/// Ruta de <init_from>: puede ser "file:///..." y con %20; si no está donde
/// dice, se busca por su nombre junto al .dae.
fs::path imagePath(std::string ref, const fs::path& dir)
{
	if (ref.rfind("file://", 0) == 0) {
		ref = ref.substr(7);
		if (ref.size() > 2 && ref[0] == '/' && ref[2] == ':') ref = ref.substr(1); // file:///C:/...
	}
	std::string dec;
	for (std::size_t i = 0; i < ref.size(); i++) {
		if (ref[i] == '%' && i + 2 < ref.size() && std::isxdigit(static_cast<unsigned char>(ref[i + 1])) && std::isxdigit(static_cast<unsigned char>(ref[i + 2]))) {
			dec += char(std::strtol(ref.substr(i + 1, 2).c_str(), nullptr, 16));
			i += 2;
		} else {
			dec += ref[i];
		}
	}
	std::replace(dec.begin(), dec.end(), '\\', '/');
	fs::path p(dec);
	std::error_code ec;
	if (p.is_relative()) p = dir / p;
	if (fs::is_regular_file(p, ec)) return p;
	return dir / fs::path(dec).filename();
}

} // namespace

bool readDae(const std::string& daePath, float scale, nectar::MeshModInput& out, ObjInfo& info, std::string& error)
{
	out  = nectar::MeshModInput();
	info = ObjInfo();
	std::ifstream f(daePath, std::ios::binary);
	if (!f) {
		error = tr("no se puede abrir el archivo");
		return false;
	}
	std::stringstream ss;
	ss << f.rdbuf();
	XmlNode doc;
	if (!parseXml(ss.str(), doc) || !doc.child("COLLADA")) {
		error = tr("no es un archivo Collada (.dae) válido");
		return false;
	}
	const XmlNode& root = *doc.child("COLLADA");
	const fs::path dir  = fs::path(daePath).parent_path();

	std::map<std::string, const XmlNode*> ids;
	std::function<void(const XmlNode&)> index = [&](const XmlNode& n) {
		const std::string id = n.attr("id");
		if (!id.empty()) ids.emplace(id, &n);
		for (const auto& c : n.children) index(*c);
	};
	index(root);
	auto byUrl = [&](const std::string& url) -> const XmlNode* {
		const auto it = ids.find(!url.empty() && url[0] == '#' ? url.substr(1) : url);
		return it != ids.end() ? it->second : nullptr;
	};

	// Z hacia arriba (Blender, 3ds Max): se gira para que Y sea arriba.
	Mat4 axis;
	if (const XmlNode* a = root.child("asset")) {
		if (const XmlNode* up = a->child("up_axis")) {
			std::string u = up->text;
			u.erase(std::remove_if(u.begin(), u.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); }), u.end());
			if (u == "Z_UP") axis.m[5] = 0, axis.m[6] = 1, axis.m[9] = -1, axis.m[10] = 0;
			else if (u == "X_UP") axis.m[0] = 0, axis.m[1] = -1, axis.m[4] = 1, axis.m[5] = 0;
		}
	}
	Mat4 scaled;
	scaled.m[0] = scaled.m[5] = scaled.m[10] = scale;
	const Mat4 world = scaled * axis;

	// Material de Collada -> índice en la salida (por textura, o color si no tiene).
	std::map<std::string, int> materialIndex;
	std::map<std::string, int> byLook;
	auto materialFor = [&](const std::string& matUrl) {
		const auto known = materialIndex.find(matUrl);
		if (known != materialIndex.end()) return known->second;
		std::string texFile;
		float colour[4] = { 0.8f, 0.8f, 0.8f, 1.0f };
		const XmlNode* mat    = byUrl(matUrl);
		const XmlNode* ie     = mat ? mat->child("instance_effect") : nullptr;
		const XmlNode* effect = ie ? byUrl(ie->attr("url")) : nullptr;
		const XmlNode* profile = effect ? effect->child("profile_COMMON") : nullptr;
		if (profile) {
			// sid de newparam -> nodo (sampler2D o surface).
			std::map<std::string, const XmlNode*> params;
			for (const auto& c : profile->children) {
				if (c->name == "newparam" && !c->children.empty()) params[c->attr("sid")] = c->children.front().get();
			}
			const XmlNode* tech = profile->child("technique");
			const XmlNode* shading = nullptr;
			if (tech) {
				for (const char* k : { "blinn", "phong", "lambert", "constant" }) {
					if ((shading = tech->child(k))) break;
				}
			}
			const XmlNode* diffuse = shading ? shading->child("diffuse") : nullptr;
			if (!diffuse && shading) diffuse = shading->child("emission");
			if (diffuse) {
				if (const XmlNode* tex = diffuse->child("texture")) {
					std::string ref = tex->attr("texture");
					// sampler -> surface -> imagen (1.4), o directamente la imagen.
					for (int hop = 0; hop < 3 && params.count(ref); hop++) {
						const XmlNode* p = params[ref];
						const XmlNode* next = p->child("source") ? p->child("source") : p->child("init_from");
						if (!next) break;
						ref = next->text;
						ref.erase(std::remove_if(ref.begin(), ref.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); }), ref.end());
					}
					if (const XmlNode* img = byUrl(ref)) {
						if (const XmlNode* from = img->child("init_from")) {
							std::string t = from->child("ref") ? from->child("ref")->text : from->text; // 1.5: <init_from><ref>
							t.erase(0, t.find_first_not_of(" \t\r\n"));
							t.erase(t.find_last_not_of(" \t\r\n") + 1);
							texFile = imagePath(t, dir).string();
						}
					}
				} else if (const XmlNode* c = diffuse->child("color")) {
					const std::vector<float> v = floats(c->text);
					for (std::size_t k = 0; k < 4 && k < v.size(); k++) colour[k] = v[k];
				}
			}
		}
		char look[64];
		std::snprintf(look, sizeof(look), "#%.3f,%.3f,%.3f,%.3f", colour[0], colour[1], colour[2], colour[3]);
		const std::string key = texFile.empty() ? std::string(look) : texFile;
		const auto same       = byLook.find(key);
		if (same != byLook.end()) return materialIndex[matUrl] = same->second;
		nectar::MeshModMaterial m;
		if (!texFile.empty()) {
			int w = 0, h = 0, n = 0;
			if (unsigned char* px = stbi_load(texFile.c_str(), &w, &h, &n, 4)) {
				m.texture.resize(w, h);
				std::copy(px, px + std::size_t(w) * h * 4, m.texture.rgba.begin());
				stbi_image_free(px);
				info.textures++;
				for (std::size_t i = 3; i < m.texture.rgba.size() && !m.alphaTest; i += 4) m.alphaTest = m.texture.rgba[i] < 128;
			} else {
				info.warnings += tr("No se encuentra la textura ") + fs::path(texFile).filename().string() + "\n";
			}
		}
		if (m.texture.empty()) {
			m.texture.resize(8, 8);
			for (int i = 0; i < 64; i++) {
				for (int c = 0; c < 4; c++) m.texture.rgba[std::size_t(i) * 4 + std::size_t(c)] = std::uint8_t(std::clamp(colour[c], 0.0f, 1.0f) * 255.0f);
			}
		}
		const int i = int(out.materials.size());
		out.materials.push_back(m);
		byLook[key] = i;
		return materialIndex[matUrl] = i;
	};

	// Mallas repetidas en el mismo sitio (capas de sombra o de luz encima de
	// la de verdad, como en los niveles sacados de N64): solo la primera.
	std::set<std::vector<long>> placed;
	std::size_t repeated = 0, skipped = 0;

	auto addGeometry = [&](const XmlNode& geom, const Mat4& m, const std::map<std::string, std::string>& bind) {
		const XmlNode* mesh = geom.child("mesh");
		if (!mesh) return;
		std::map<std::string, Source> sources;
		for (const auto& c : mesh->children) {
			if (c->name != "source") continue;
			Source s;
			if (const XmlNode* fa = c->child("float_array")) s.data = floats(fa->text);
			if (const XmlNode* tc = c->child("technique_common")) {
				if (const XmlNode* acc = tc->child("accessor")) s.stride = std::max(1, std::atoi(acc->attr("stride").c_str()));
			}
			sources["#" + c->attr("id")] = std::move(s);
		}
		std::string posSrc, vNrmSrc, vUvSrc;
		const std::string verticesId = mesh->child("vertices") ? "#" + mesh->child("vertices")->attr("id") : std::string();
		if (const XmlNode* v = mesh->child("vertices")) {
			for (const auto& in : v->children) {
				if (in->name != "input") continue;
				const std::string sem = in->attr("semantic");
				if (sem == "POSITION") posSrc = in->attr("source");
				else if (sem == "NORMAL") vNrmSrc = in->attr("source");
				else if (sem == "TEXCOORD") vUvSrc = in->attr("source");
			}
		}
		const auto pit = sources.find(posSrc);
		if (pit == sources.end() || pit->second.stride < 3) return;
		const Source& pos = pit->second;
		const std::size_t nPos = pos.data.size() / std::size_t(pos.stride);

		// Huella de la malla ya colocada (posiciones redondeadas).
		std::vector<long> print;
		print.reserve(nPos * 3);
		std::vector<nectar::Vec3> worldPos(nPos);
		for (std::size_t i = 0; i < nPos; i++) {
			const float* p = &pos.data[i * std::size_t(pos.stride)];
			worldPos[i]    = m.point({ p[0], p[1], p[2] });
			for (float c : { worldPos[i].x, worldPos[i].y, worldPos[i].z }) print.push_back(std::lround(c * 100.0f));
		}
		if (!placed.insert(print).second) {
			repeated++;
			return;
		}
		const bool flip  = m.det3() < 0.0f;
		const int posOff = int(out.positions.size());
		for (const nectar::Vec3& p : worldPos) out.positions.push_back(p);
		// Normales y UV: se copian enteras de su source; el índice es el suyo + el desplazamiento.
		std::map<std::string, int> copied; // source -> desplazamiento en la salida
		auto copySource = [&](const std::string& id, bool normal) -> int {
			const auto it = copied.find(id);
			if (it != copied.end()) return it->second;
			const auto s = sources.find(id);
			if (s == sources.end()) return -1;
			const Source& src = s->second;
			const std::size_t n = src.data.size() / std::size_t(src.stride);
			int off             = -1;
			if (normal && src.stride >= 3) {
				off = int(out.normals.size());
				for (std::size_t i = 0; i < n; i++) {
					const float* p   = &src.data[i * std::size_t(src.stride)];
					nectar::Vec3 d   = m.dir({ p[0], p[1], p[2] });
					const float len  = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
					if (len > 1e-6f) d = { d.x / len, d.y / len, d.z / len };
					out.normals.push_back(d);
				}
			} else if (!normal && src.stride >= 2) {
				off = int(out.uvs.size());
				for (std::size_t i = 0; i < n; i++) {
					const float* p = &src.data[i * std::size_t(src.stride)];
					out.uvs.push_back({ p[0], 1.0f - p[1] }); // origen abajo, como en OBJ
				}
			}
			copied[id] = off;
			return off;
		};

		for (const auto& prim : mesh->children) {
			const std::string& kind = prim->name;
			if (kind != "triangles" && kind != "polylist" && kind != "polygons") continue;
			const std::string sym  = prim->attr("material");
			const auto b           = bind.find(sym);
			const int material     = materialFor(b != bind.end() ? b->second : "#" + sym);
			int stride = 0, vOff = -1, nOff = -1, tOff = -1;
			std::string nSrc, tSrc;
			bool firstUv = true;
			for (const auto& in : prim->children) {
				if (in->name != "input") continue;
				const int o         = std::atoi(in->attr("offset").c_str());
				stride              = std::max(stride, o + 1);
				const std::string s = in->attr("semantic");
				if (s == "VERTEX") vOff = o;
				else if (s == "NORMAL") nOff = o, nSrc = in->attr("source");
				else if (s == "TEXCOORD" && firstUv) tOff = o, tSrc = in->attr("source"), firstUv = false;
			}
			if (vOff < 0 || stride < 1) continue;
			// Normal o UV dentro de <vertices>: van con el índice del vértice.
			const bool nFromVertex = nOff < 0 && !vNrmSrc.empty(), tFromVertex = tOff < 0 && !vUvSrc.empty();
			const int nBase = nOff >= 0 ? copySource(nSrc, true) : nFromVertex ? copySource(vNrmSrc, true) : -1;
			const int tBase = tOff >= 0 ? copySource(tSrc, false) : tFromVertex ? copySource(vUvSrc, false) : -1;

			std::vector<std::vector<int>> polys; // índices crudos por polígono
			if (kind == "polygons") {
				for (const auto& p : prim->children) {
					if (p->name == "p") polys.push_back(ints(p->text));
				}
			} else if (const XmlNode* p = prim->child("p")) {
				const std::vector<int> all = ints(p->text);
				std::vector<int> counts;
				if (kind == "polylist" && prim->child("vcount")) counts = ints(prim->child("vcount")->text);
				std::size_t at = 0;
				for (std::size_t k = 0; at < all.size(); k++) {
					const int corners = kind == "triangles" ? 3 : (k < counts.size() ? counts[k] : 0);
					if (corners <= 0) break;
					const std::size_t len = std::size_t(corners) * std::size_t(stride);
					if (at + len > all.size()) break;
					polys.emplace_back(all.begin() + long(at), all.begin() + long(at + len));
					at += len;
				}
			}
			for (const std::vector<int>& poly : polys) {
				const int corners = int(poly.size()) / stride;
				if (corners < 3) {
					skipped++;
					continue;
				}
				auto corner = [&](int c, int& pi, int& ni, int& ti) {
					const int* idx = &poly[std::size_t(c) * std::size_t(stride)];
					const int v    = idx[vOff];
					pi             = v >= 0 && std::size_t(v) < nPos ? posOff + v : -1;
					ni = nBase < 0 ? -1 : nBase + (nOff >= 0 ? idx[nOff] : v);
					ti = tBase < 0 ? -1 : tBase + (tOff >= 0 ? idx[tOff] : v);
					if (ni >= int(out.normals.size()) || ni < 0) ni = -1;
					if (ti >= int(out.uvs.size()) || ti < 0) ti = -1;
				};
				for (int i = 1; i + 1 < corners; i++) {
					nectar::MeshModTri t;
					const int cs[3] = { 0, i, i + 1 };
					bool ok = true;
					for (int k = 0; k < 3; k++) {
						corner(cs[k], t.pos[k], t.nrm[k], t.uv[k]);
						ok = ok && t.pos[k] >= 0;
					}
					// El juego dibuja la cara de delante al revés que Collada
					// (salvo si el nodo está reflejado, que ya le da la vuelta).
					if (!flip) {
						std::swap(t.pos[1], t.pos[2]);
						std::swap(t.uv[1], t.uv[2]);
						std::swap(t.nrm[1], t.nrm[2]);
					}
					t.material = material;
					if (ok) out.tris.push_back(t);
					else skipped++;
				}
			}
		}
	};

	std::function<void(const XmlNode&, const Mat4&, int)> walk = [&](const XmlNode& node, const Mat4& parent, int depth) {
		if (depth > 64) return;
		const Mat4 m = parent * nodeTransform(node);
		for (const auto& c : node.children) {
			if (c->name == "node") {
				walk(*c, m, depth + 1);
			} else if (c->name == "instance_node") {
				if (const XmlNode* n = byUrl(c->attr("url"))) walk(*n, m, depth + 1);
			} else if (c->name == "instance_geometry") {
				std::map<std::string, std::string> bind;
				if (const XmlNode* bm = c->child("bind_material")) {
					if (const XmlNode* tc = bm->child("technique_common")) {
						for (const auto& im : tc->children) {
							if (im->name == "instance_material") bind[im->attr("symbol")] = im->attr("target");
						}
					}
				}
				if (const XmlNode* g = byUrl(c->attr("url"))) addGeometry(*g, m, bind);
			}
		}
	};
	const XmlNode* sceneRef = root.child("scene") ? root.child("scene")->child("instance_visual_scene") : nullptr;
	const XmlNode* scene    = sceneRef ? byUrl(sceneRef->attr("url")) : nullptr;
	if (!scene) {
		if (const XmlNode* lib = root.child("library_visual_scenes")) scene = lib->child("visual_scene");
	}
	if (scene) {
		walk(*scene, world, 0);
	} else if (const XmlNode* lib = root.child("library_geometries")) {
		// Sin escena: todas las mallas tal cual.
		for (const auto& g : lib->children) {
			if (g->name == "geometry") addGeometry(*g, world, {});
		}
	}

	if (out.tris.empty()) {
		error = tr("el archivo no tiene caras (¿es un .dae con mallas?)");
		return false;
	}
	if (repeated) info.warnings += tr("Capas repetidas que se han quitado (sombras encima de la malla): ") + std::to_string(repeated) + "\n";
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

} // namespace nm
