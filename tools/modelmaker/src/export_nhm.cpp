#include "export_nhm.h"

#include "lang.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

namespace mm {

namespace fs = std::filesystem;

namespace {

constexpr unsigned kFlagRepeat = 1, kFlagNoCull = 2, kFlagNoTint = 4;
constexpr unsigned kModelFlagRetarget = 1;
constexpr std::size_t kMaxPartTris    = 65535 / 3; // pc_gfx_begin cuenta los vértices con 16 bits
constexpr int kMaxParts               = 8;

nectar::Image solid(const float c[4])
{
	nectar::Image img;
	img.resize(4, 4);
	for (int i = 0; i < 16; ++i)
		for (int k = 0; k < 4; ++k) img.rgba[std::size_t(i * 4 + k)] = std::uint8_t(std::clamp(c[k], 0.0f, 1.0f) * 255.0f + 0.5f);
	return img;
}

// Textura del material ya lista: color base multiplicado y alfa según el modo.
nectar::Image materialImage(const ImpMaterial& m, int maxSide)
{
	nectar::Image img = m.texture.empty() ? solid(m.color) : nectar::shrinkToFit(m.texture, maxSide, maxSide);
	if (!m.texture.empty()) {
		for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4)
			for (int k = 0; k < 4; ++k) img.rgba[i + std::size_t(k)] = std::uint8_t(std::clamp(img.rgba[i + std::size_t(k)] * m.color[k], 0.0f, 255.0f));
	}
	for (std::size_t i = 3; i < img.rgba.size(); i += 4) {
		if (m.alpha == ImpMaterial::Opaque) img.rgba[i] = 255;
		else if (m.alpha == ImpMaterial::Mask) img.rgba[i] = img.rgba[i] >= std::uint8_t(m.cutoff * 255.0f) ? 255 : 0;
	}
	return img;
}

void blit(nectar::Image& dst, const nectar::Image& src, int x0, int y0, int cell)
{
	// Vecino más cercano a la celda (las celdas son pequeñas: se ve igual).
	for (int y = 0; y < cell; ++y) {
		for (int x = 0; x < cell; ++x) {
			const int sx = std::min(src.width - 1, x * src.width / cell);
			const int sy = std::min(src.height - 1, y * src.height / cell);
			std::memcpy(dst.at(x0 + x, y0 + y), src.at(sx, sy), 4);
		}
	}
}

template <typename T>
void put(std::vector<std::uint8_t>& out, const T& v)
{
	const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
	out.insert(out.end(), p, p + sizeof(T));
}

} // namespace

ExportResult buildParts(const ImpModel& model, const RigResult& rig, const OlimarRig& olimar, const FitSettings& fit, const ExportOptions& opt)
{
	ExportResult res;
	const std::size_t tris = rig.verts.size() / 3;
	res.triangles          = tris;
	if (!tris) {
		res.checks.push_back({ Check::Bad, L("No hay modelo.", "No model loaded.") });
		return res;
	}

	// Triángulos de cada material. Sin textura pero con color de vértice: una
	// paleta (color medio de cada triángulo en su propio texel).
	struct Group {
		int material = 0;
		std::vector<std::size_t> tris;
		nectar::Image texture;
		bool repeat = false;
		std::vector<Vec2> paletteUv; ///< por triángulo, si es paleta
	};
	std::map<int, Group> byMaterial;
	for (std::size_t t = 0; t < tris; ++t) {
		const int m = t < rig.triMaterial.size() ? rig.triMaterial[t] : 0;
		byMaterial[m].material = m;
		byMaterial[m].tris.push_back(t);
	}
	std::vector<Group> groups;
	for (auto& [m, g] : byMaterial) {
		const ImpMaterial& mat = model.materials[std::size_t(std::clamp(m, 0, int(model.materials.size()) - 1))];
		if (mat.texture.empty() && mat.vertexColors) {
			std::map<std::uint32_t, int> palette;
			std::vector<int> index;
			for (std::size_t t : g.tris) {
				float c[4] = {};
				for (int k = 0; k < 3; ++k)
					for (int i = 0; i < 4; ++i) c[i] += rig.verts[t * 3 + std::size_t(k)].col[i] * mat.color[i] / 3.0f;
				std::uint32_t key = 0;
				for (int i = 0; i < 4; ++i) key = (key << 8) | (std::uint32_t(std::clamp(c[i], 0.0f, 1.0f) * 255.0f) & 0xf8);
				index.push_back(palette.emplace(key, int(palette.size())).first->second);
			}
			const int side = std::max(4, int(std::ceil(std::sqrt(double(palette.size())))));
			g.texture.resize(side, side);
			for (const auto& [key, i] : palette) {
				std::uint8_t* px = g.texture.at(i % side, i / side);
				px[0] = std::uint8_t(key >> 24), px[1] = std::uint8_t(key >> 16), px[2] = std::uint8_t(key >> 8);
				px[3] = mat.alpha == ImpMaterial::Opaque ? 255 : std::uint8_t(key);
			}
			for (int i : index) g.paletteUv.push_back({ (float(i % side) + 0.5f) / float(side), (float(i / side) + 0.5f) / float(side) });
		} else {
			g.texture = materialImage(mat, opt.textureMax);
		}
		for (std::size_t t : g.tris)
			for (int k = 0; k < 3; ++k) {
				const Vec2& uv = rig.verts[t * 3 + std::size_t(k)].uv;
				if (uv.x < -0.01f || uv.x > 1.01f || uv.y < -0.01f || uv.y > 1.01f) g.repeat = true;
			}
		if (!g.paletteUv.empty()) g.repeat = false;
		groups.push_back(std::move(g));
	}

	auto partsFor = [](std::size_t triCount) { return int((triCount + kMaxPartTris - 1) / kMaxPartTris); };
	int partCount = 0;
	for (const Group& g : groups) partCount += partsFor(g.tris.size());

	// Más de 8 piezas: los materiales pequeños comparten un atlas.
	std::vector<Group> atlasGroups;
	if (partCount > kMaxParts) {
		std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.tris.size() < b.tris.size(); });
		std::size_t atlasTris = 0;
		auto total            = [&] {
            int count = partsFor(atlasTris);
            for (const Group& g : groups) count += partsFor(g.tris.size());
            return count;
		};
		while (total() > kMaxParts && !groups.empty()) {
			atlasTris += groups.front().tris.size();
			atlasGroups.push_back(std::move(groups.front()));
			groups.erase(groups.begin());
		}
		// Uno solo no ahorra nada: vuelve a su sitio (y el semáforo dirá por qué no cabe).
		if (atlasGroups.size() == 1) {
			groups.push_back(std::move(atlasGroups.front()));
			atlasGroups.clear();
		}
		res.atlas = !atlasGroups.empty();
	}

	auto vertexOf = [&](const Group& g, std::size_t gi, std::size_t t, int k) {
		SkinVertex v = rig.verts[t * 3 + std::size_t(k)];
		if (!g.paletteUv.empty()) v.uv = g.paletteUv[gi];
		return v;
	};
	auto flagsFor = [&](const Group& g) {
		const ImpMaterial& mat = model.materials[std::size_t(std::clamp(g.material, 0, int(model.materials.size()) - 1))];
		unsigned f             = 0;
		if (g.repeat) f |= kFlagRepeat;
		if (mat.doubleSided) f |= kFlagNoCull;
		if (std::size_t(g.material) < opt.noTint.size() && opt.noTint[std::size_t(g.material)]) f |= kFlagNoTint;
		return f;
	};
	for (const Group& g : groups) {
		for (std::size_t start = 0; start < g.tris.size(); start += kMaxPartTris) {
			NhmPart p;
			p.texture = g.texture;
			p.flags   = flagsFor(g);
			p.materials.push_back(g.material);
			for (std::size_t gi = start; gi < std::min(g.tris.size(), start + kMaxPartTris); ++gi)
				for (int k = 0; k < 3; ++k) p.verts.push_back(vertexOf(g, gi, g.tris[gi], k));
			res.parts.push_back(std::move(p));
		}
	}
	if (!atlasGroups.empty()) {
		const int count = int(atlasGroups.size());
		const int cols  = int(std::ceil(std::sqrt(double(count))));
		const int cell  = std::max(16, std::min(512, 2048 / cols));
		nectar::Image atlas;
		atlas.resize(cols * cell, ((count + cols - 1) / cols) * cell);
		std::vector<NhmPart> pieces(1);
		unsigned flags = 0;
		for (int i = 0; i < count; ++i) {
			const Group& g = atlasGroups[std::size_t(i)];
			const int cx = (i % cols) * cell, cy = (i / cols) * cell;
			blit(atlas, g.texture, cx, cy, cell);
			flags |= flagsFor(g) & ~kFlagRepeat;
			for (std::size_t gi = 0; gi < g.tris.size(); ++gi) {
				if (pieces.back().verts.size() / 3 >= kMaxPartTris) pieces.emplace_back();
				// Repetidas: se lleva el triángulo a [0,1] entero antes de recortar.
				float fu = 0, fv = 0;
				if (g.repeat) {
					float mu = 1e9f, mv = 1e9f;
					for (int k = 0; k < 3; ++k) {
						mu = std::min(mu, rig.verts[g.tris[gi] * 3 + std::size_t(k)].uv.x);
						mv = std::min(mv, rig.verts[g.tris[gi] * 3 + std::size_t(k)].uv.y);
					}
					fu = std::floor(mu), fv = std::floor(mv);
				}
				for (int k = 0; k < 3; ++k) {
					SkinVertex v = vertexOf(g, gi, g.tris[gi], k);
					const float u = std::clamp(v.uv.x - fu, 0.0f, 1.0f), w = std::clamp(v.uv.y - fv, 0.0f, 1.0f);
					v.uv = { (float(cx) + 0.5f + u * float(cell - 1)) / float(atlas.width), (float(cy) + 0.5f + w * float(cell - 1)) / float(atlas.height) };
					pieces.back().verts.push_back(v);
				}
			}
			pieces.back().materials.push_back(g.material);
		}
		for (NhmPart& p : pieces) {
			p.texture = atlas;
			p.flags   = flags;
			res.parts.push_back(std::move(p));
		}
		res.checks.push_back({ Check::Warn, std::to_string(count) + L(" materiales pequeños van juntos en una textura (el juego admite 8 piezas).",
		                                                             " small materials share one texture (the game takes 8 parts).") });
	}

	// Semáforo.
	if (opt.pikmin && int(res.parts.size()) <= kMaxParts) {
		// Puede haber 100 a la vez (el original tiene unos 300 triángulos).
		if (tris > 12000)
			res.checks.push_back({ Check::Bad, std::to_string(tris) + L(" triángulos: demasiado para 100 Pikmin. Baja el número en el paso 1 (Aplicar).",
			                                                           " triangles: too many for 100 Pikmin. Lower the count in step 1 (Apply).") });
		else if (tris > 5000)
			res.checks.push_back({ Check::Warn, std::to_string(tris) + L(" triángulos: con muchos Pikmin irá lento (mejor menos de 2.000).",
			                                                            " triangles: slow with many Pikmin (under 2,000 is best).") });
		else if (tris > 2000)
			res.checks.push_back({ Check::Warn, std::to_string(tris) + L(" triángulos: bien en PC; en Android mejor menos de 2.000.",
			                                                            " triangles: fine on PC; under 2,000 is best on Android.") });
		else
			res.checks.push_back({ Check::Ok, std::to_string(tris) + L(" triángulos: perfecto para 100 Pikmin.", " triangles: perfect for 100 Pikmin.") });
	} else if (int(res.parts.size()) > kMaxParts) {
		res.checks.push_back({ Check::Bad, L("Demasiados triángulos: el juego admite unos 170.000. Simplifica el modelo (Blender: Decimate).",
		                                     "Too many triangles: the game takes about 170,000. Simplify the model (Blender: Decimate).") });
	} else if (tris > 60000) {
		res.checks.push_back({ Check::Warn, std::to_string(tris) + L(" triángulos: en Android puede ir lento (mejor menos de 20.000).",
		                                                            " triangles: may be slow on Android (under 20,000 is best).") });
	} else if (tris > 20000) {
		res.checks.push_back({ Check::Warn, std::to_string(tris) + L(" triángulos: bien en PC; en Android mejor menos de 20.000.",
		                                                            " triangles: fine on PC; under 20,000 is best on Android.") });
	} else {
		res.checks.push_back({ Check::Ok, std::to_string(tris) + L(" triángulos: perfecto.", " triangles: perfect.") });
	}
	if (rig.fromSkeleton) {
		if (rig.mappedShare >= 0.8f)
			res.checks.push_back({ Check::Ok, L("Esqueleto reconocido: los pesos son los del modelo.", "Skeleton recognised: the model's own weights are used.") });
		else
			res.checks.push_back({ Check::Warn, L("Solo parte del esqueleto se reconoce; prueba \"Pesos automáticos\" si algo se mueve raro.",
			                                      "Only part of the skeleton is recognised; try \"Automatic weights\" if something moves oddly.") });
		if (!rig.skeletonNote.empty()) res.checks.push_back({ Check::Ok, rig.skeletonNote });
	} else {
		res.checks.push_back({ Check::Ok, L("Pesos automáticos: mira las animaciones y mueve los puntos si algo se dobla mal.",
		                                    "Automatic weights: watch the animations and move the dots if something bends wrong.") });
	}
	const float olimarH = olimar.bodyTop - olimar.min.y;
	const float ratio   = (rig.max.y - rig.min.y) / std::max(olimarH, 1e-3f);
	if (ratio > 2.2f || ratio < 0.45f)
		res.checks.push_back({ Check::Warn, L("Muy distinto de Olimar en tamaño: el juego sigue chocando y recogiendo como Olimar.",
		                                      "Very different size from Olimar: the game still collides and picks up like Olimar.") });
	const Vec3 sh = rig.markers[M_LSHOULDER] - rig.markers[M_RSHOULDER];
	if (sh.x <= 0)
		res.checks.push_back({ Check::Bad, L("Los hombros están cruzados: gira el modelo 180° o recoloca los puntos.",
		                                     "The shoulders are crossed: turn the model 180° or move the dots.") });
	if (rig.markers[M_NECK].y <= rig.markers[M_PELVIS].y)
		res.checks.push_back({ Check::Bad, L("El cuello está por debajo de la cadera: ¿el modelo está tumbado?", "The neck is below the pelvis: is the model lying down?") });
	for (std::size_t i = 0, n = 0; i < model.warnings.size(); i = n + 1) {
		n = model.warnings.find('\n', i);
		if (n == std::string::npos) n = model.warnings.size();
		if (n > i) res.checks.push_back({ Check::Warn, model.warnings.substr(i, n - i) });
	}
	(void)fit;

	res.bytes = 20;
	for (const NhmPart& p : res.parts) res.bytes += 20 + p.verts.size() * 52 + p.texture.rgba.size();
	return res;
}

bool buildNhm(const ExportResult& result, const RigResult& rig, const OlimarRig& olimar, std::vector<std::uint8_t>& out, std::string& error)
{
	if (!result.ok()) {
		error = L("el modelo tiene errores (en rojo)", "the model has errors (in red)");
		return false;
	}
	out.clear();
	// Huesos del pack: las articulaciones con nombre, en orden.
	std::vector<int> joints;
	int boneOf[kJoints];
	for (int j = 0; j < kJoints; ++j) {
		boneOf[j] = -1;
		if (packBoneName(j)) {
			boneOf[j] = int(joints.size());
			joints.push_back(j);
		}
	}
	out.insert(out.end(), { 'N', 'H', 'M', '1' });
	put(out, std::uint32_t(3));
	put(out, std::uint32_t(joints.size()));
	put(out, std::uint32_t(result.parts.size()));
	put(out, std::uint32_t(kModelFlagRetarget));
	for (int j : joints) {
		const std::string name = packBoneName(j);
		out.push_back(std::uint8_t(name.size()));
		out.insert(out.end(), name.begin(), name.end());
		// Reposo propio: el giro de Olimar con el pivote del personaje.
		Mat34 bind = olimar.bind[std::size_t(j)];
		setTranslation(bind, rig.pivots[std::size_t(j)]);
		const Mat34 inv = inverse(bind);
		for (int row = 0; row < 4; ++row)
			for (int col = 0; col < 4; ++col) put(out, row < 3 ? inv.m[row][col] : (col == 3 ? 1.0f : 0.0f));
	}
	for (const NhmPart& p : result.parts) {
		put(out, std::uint32_t(p.verts.size()));
		put(out, std::uint32_t(p.texture.width));
		put(out, std::uint32_t(p.texture.height));
		put(out, std::uint32_t(p.texture.rgba.size()));
		put(out, std::uint32_t(p.flags));
		for (const SkinVertex& v : p.verts) {
			const float f[8] = { v.p.x, v.p.y, v.p.z, v.n.x, v.n.y, v.n.z, v.uv.x, v.uv.y };
			for (float x : f) put(out, x);
			for (int k = 0; k < 4; ++k) {
				const int b = v.weight[k] > 0 && v.joint[k] >= 0 && v.joint[k] < kJoints ? boneOf[v.joint[k]] : -1;
				out.push_back(std::uint8_t(b < 0 ? 0 : b));
			}
			for (int k = 0; k < 4; ++k) {
				const int b = v.joint[k] >= 0 && v.joint[k] < kJoints ? boneOf[v.joint[k]] : -1;
				put(out, b < 0 ? 0.0f : v.weight[k]);
			}
		}
		out.insert(out.end(), p.texture.rgba.begin(), p.texture.rgba.end());
	}
	return true;
}

bool writeNhm(const std::string& path, const ExportResult& result, const RigResult& rig, const OlimarRig& olimar, std::string& error)
{
	std::vector<std::uint8_t> out;
	if (!buildNhm(result, rig, olimar, out, error)) return false;
	std::error_code ec;
	const fs::path target = fs::u8path(path);
	fs::create_directories(target.parent_path(), ec);
	// Se escribe aparte y se cambia de nombre: el juego nunca lee uno a medias.
	const fs::path tmp = target.string() + ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f || !f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()))) {
			error = std::string(L("no se puede escribir ", "cannot write ")) + tmp.string();
			return false;
		}
	}
	fs::rename(tmp, target, ec);
	if (ec) {
		fs::remove(target, ec);
		fs::rename(tmp, target, ec);
		if (ec) {
			error = ec.message();
			return false;
		}
	}
	return true;
}

} // namespace mm
