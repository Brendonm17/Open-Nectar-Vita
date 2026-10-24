#include "rig.h"

#include "lang.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <unordered_map>

namespace mm {

namespace {

constexpr float kPi = 3.14159265358979f;

bool has(const std::string& s, const char* word) { return s.find(word) != std::string::npos; }

// Palabras de un nombre de hueso: "LeftUpLeg" -> left up leg,
// "J_Bip_L_UpperArm" -> j bip l upper arm, "upperarm.L" -> upperarm l.
std::vector<std::string> words(const std::string& name)
{
	std::vector<std::string> out;
	std::string cur;
	auto flush = [&] {
		if (!cur.empty()) out.push_back(cur);
		cur.clear();
	};
	for (std::size_t i = 0; i < name.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(name[i]);
		if (c >= 0x80) { // UTF-8 (nombres japoneses de MMD): se dejan juntos
			cur += char(c);
			continue;
		}
		if (!std::isalnum(c)) {
			flush();
			continue;
		}
		// Frontera camelCase: minúscula/dígito seguida de mayúscula.
		if (std::isupper(c) && !cur.empty() && (std::islower(static_cast<unsigned char>(cur.back())) || std::isdigit(static_cast<unsigned char>(cur.back())))) flush();
		cur += char(std::tolower(c));
	}
	flush();
	return out;
}

struct Parsed {
	BoneKind kind = B_NONE;
	int side      = 0;
};

Parsed parseHumanoid(const std::string& h)
{
	Parsed p;
	std::string s = h;
	if (s.rfind("left", 0) == 0) {
		p.side = 1;
		s      = s.substr(4);
	} else if (s.rfind("right", 0) == 0) {
		p.side = -1;
		s      = s.substr(5);
	}
	std::string l;
	for (char c : s) l += char(std::tolower(static_cast<unsigned char>(c)));
	if (l == "hips") p.kind = B_HIPS;
	else if (l == "spine" || l == "chest" || l == "upperchest") p.kind = B_SPINE;
	else if (l == "neck") p.kind = B_NECK;
	else if (l == "head" || l == "eye" || l == "jaw") p.kind = B_HEAD;
	else if (l == "shoulder") p.kind = B_SHOULDER;
	else if (l == "upperarm") p.kind = B_UPPERARM;
	else if (l == "lowerarm") p.kind = B_LOWERARM;
	else if (l == "hand" || has(l, "thumb") || has(l, "index") || has(l, "middle") || has(l, "ring") || has(l, "little")) p.kind = B_HAND;
	else if (l == "upperleg") p.kind = B_UPPERLEG;
	else if (l == "lowerleg") p.kind = B_LOWERLEG;
	else if (l == "foot" || l == "toes") p.kind = B_FOOT;
	return p;
}

Parsed parseName(const std::string& name)
{
	Parsed p;
	std::string joined;
	for (const std::string& w : words(name)) {
		if (w == "l" || w == "left" || w == "lft") p.side = 1;
		else if (w == "r" || w == "right" || w == "rgt") p.side = -1;
		else {
			std::string t = w;
			if (t.rfind("left", 0) == 0 && t.size() > 4) {
				p.side = 1;
				t      = t.substr(4);
			} else if (t.rfind("right", 0) == 0 && t.size() > 5) {
				p.side = -1;
				t      = t.substr(5);
			}
			if (t == "mixamorig" || t == "def" || t == "org" || t == "mch" || t == "bip" || t == "bip01" || t == "j" || t == "c" || t == "sec")
				continue;
			joined += t;
		}
	}
	if (has(name, "\xe5\xb7\xa6")) p.side = 1;  // 左
	if (has(name, "\xe5\x8f\xb3")) p.side = -1; // 右
	const std::string& j = joined;
	auto any             = [&](std::initializer_list<const char*> keys) {
        for (const char* k : keys)
            if (has(j, k) || has(name, k)) return true;
        return false;
	};
	// El hueso raíz de Blender ("Armature") no es un brazo.
	if (has(j, "armature") || j == "root" || j == "rootbone") return p;
	if (any({ "antenna", "antena", "happa", "ahoge", "stem", "tallo", "sprout" })) p.kind = B_ANTENNA;
	else if (any({ "thumb", "index", "middle", "ring", "pinky", "little", "finger", "\xe6\x8c\x87" })) p.kind = B_HAND; // 指
	else if (any({ "hand", "wrist", "palm", "\xe6\x89\x8b" })) p.kind = B_HAND;                                       // 手
	else if (any({ "forearm", "lowerarm", "elbow", "\xe3\x81\xb2\xe3\x81\x98" })) p.kind = B_LOWERARM;                 // ひじ
	else if (any({ "upperarm", "arm", "\xe8\x85\x95" })) p.kind = B_UPPERARM;                                          // 腕
	else if (any({ "shoulder", "clavicle", "collar", "\xe8\x82\xa9" })) p.kind = B_SHOULDER;                           // 肩
	else if (any({ "toe", "foot", "ankle", "\xe8\xb6\xb3\xe9\xa6\x96", "\xe3\x81\xa4\xe3\x81\xbe" })) p.kind = B_FOOT;  // 足首, つま先
	else if (any({ "upleg", "thigh", "upperleg", "\xe5\xa4\xaa" }) || (p.side != 0 && any({ "hip" }))) p.kind = B_UPPERLEG; // 太もも
	else if (any({ "calf", "shin", "knee", "lowerleg", "leg", "\xe3\x81\xb2\xe3\x81\x96", "\xe8\xb6\xb3" })) p.kind = B_LOWERLEG; // ひざ, 足
	else if (any({ "hips", "pelvis", "hip", "\xe8\x85\xb0" })) p.kind = B_HIPS;                                         // 腰
	else if (any({ "neck", "\xe9\xa6\x96" })) p.kind = B_NECK;                                                         // 首
	else if (any({ "head", "skull", "\xe9\xa0\xad" })) p.kind = B_HEAD;                                                // 頭
	else if (any({ "eye", "jaw", "hair", "ear", "face", "mouth", "tongue", "brow", "lid", "teeth", "nose", "cheek", "\xe7\x9b\xae", "\xe9\xab\xaa" }))
		p.kind = B_HEAD; // 目, 髪
	else if (any({ "spine", "chest", "torso", "abdomen", "belly", "waist", "body", "\xe4\xb8\x8a\xe5\x8d\x8a\xe8\xba\xab", "\xe4\xb8\x8b\xe5\x8d\x8a\xe8\xba\xab" }))
		p.kind = B_SPINE; // 上半身, 下半身
	if (p.kind == B_HIPS || p.kind == B_SPINE || p.kind == B_NECK || p.kind == B_HEAD) p.side = 0;
	return p;
}

// Huesos de un model.nhm del creador (las articulaciones de Olimar): se
// reconocen tal cual, con sus pivotes propios.
bool parsePackBone(const std::string& name, Parsed& p)
{
	static const struct {
		const char* name;
		BoneKind kind;
		int side;
	} kBones[] = {
		{ "kosinull", B_HIPS, 0 },      { "legcentre", B_HIPS, 0 },    { "sebonjnt", B_SPINE, 0 },   { "headjnt", B_NECK, 0 },
		{ "happajnt1", B_ANTENNA, 0 }, { "happajnt2", B_ANTENNA, 0 }, { "happajnt3", B_ANTENNA, 0 }, { "rhandjnt", B_UPPERARM, -1 },
		{ "lhandjnt", B_UPPERARM, 1 }, { "rlegjnt", B_UPPERLEG, -1 }, { "llegjnt", B_UPPERLEG, 1 },
	};
	for (const auto& b : kBones) {
		if (name == b.name) {
			p.kind = b.kind;
			p.side = b.side;
			return true;
		}
	}
	return false;
}

bool isArm(BoneKind k) { return k == B_UPPERARM || k == B_LOWERARM || k == B_HAND; }
bool isLeg(BoneKind k) { return k == B_UPPERLEG || k == B_LOWERLEG || k == B_FOOT; }

int jointFor(BoneKind k, int side)
{
	if (isArm(k)) return side > 0 ? J_LARM : J_RARM;
	if (isLeg(k)) return side > 0 ? J_LLEG : J_RLEG;
	if (k == B_NECK || k == B_HEAD) return J_HEAD;
	if (k == B_ANTENNA) return J_ANT1;
	return J_SPINE;
}

std::vector<Vec3> fittedPositions(const ImpModel& model, const Mat34& fit)
{
	std::vector<Vec3> out;
	out.reserve(model.verts.size());
	for (const SkinVertex& v : model.verts) out.push_back(fit.point(v.p));
	return out;
}

float segmentDistance(const Vec3& p, const Vec3& a, const Vec3& b)
{
	const Vec3 ab = b - a;
	const float l = dot(ab, ab);
	const float t = l > 1e-12f ? std::clamp(dot(p - a, ab) / l, 0.0f, 1.0f) : 0.0f;
	return length(p - (a + ab * t));
}

Mat34 orientation(const ImpModel& model, const FitSettings& fit)
{
	Mat34 r;
	if (fit.lyingDown) r = axisAngle({ 1, 0, 0 }, -kPi / 2); // Z hacia arriba -> Y
	const int quarter = ((fit.quarter + (model.facesMinusZ ? 2 : 0)) % 4 + 4) % 4;
	return axisAngle({ 0, 1, 0 }, float(quarter) * kPi / 2) * r;
}

// Unión de conjuntos para las piezas sueltas de la malla.
int findRoot(std::vector<int>& p, int i)
{
	while (p[std::size_t(i)] != i) i = p[std::size_t(i)] = p[std::size_t(p[std::size_t(i)])];
	return i;
}

} // namespace

const char* markerName(int marker)
{
	switch (marker) {
	case M_PELVIS: return L("Cadera", "Pelvis");
	case M_NECK: return L("Cuello", "Neck");
	case M_HEADTOP: return L("Coronilla", "Head top");
	case M_RSHOULDER: return L("Hombro der.", "R shoulder");
	case M_LSHOULDER: return L("Hombro izq.", "L shoulder");
	case M_RHAND: return L("Mano der.", "R hand");
	case M_LHAND: return L("Mano izq.", "L hand");
	case M_RHIP: return L("Pierna der.", "R hip");
	case M_LHIP: return L("Pierna izq.", "L hip");
	case M_RFOOT: return L("Pie der.", "R foot");
	case M_LFOOT: return L("Pie izq.", "L foot");
	case M_STEMTOP: return L("Punta del tallo", "Stem tip");
	default: return "?";
	}
}

int markerMirror(int marker)
{
	switch (marker) {
	case M_RSHOULDER: return M_LSHOULDER;
	case M_LSHOULDER: return M_RSHOULDER;
	case M_RHAND: return M_LHAND;
	case M_LHAND: return M_RHAND;
	case M_RHIP: return M_LHIP;
	case M_LHIP: return M_RHIP;
	case M_RFOOT: return M_LFOOT;
	case M_LFOOT: return M_RFOOT;
	default: return -1;
	}
}

Mat34 fitTransform(const ImpModel& model, const OlimarRig& olimar, const FitSettings& fit)
{
	if (fit.keepPlacement) return translation({ 0, fit.offsetY, fit.offsetZ }) * uniformScale(std::max(fit.scale, 0.05f));
	const Mat34 r = orientation(model, fit);
	if (model.verts.empty()) return r;
	Vec3 mn = r.point(model.verts[0].p), mx = mn;
	for (const SkinVertex& v : model.verts) {
		const Vec3 p = r.point(v.p);
		mn           = vmin(mn, p);
		mx           = vmax(mx, p);
	}
	const float height = std::max(mx.y - mn.y, 1e-6f);
	const float target = (olimar.bodyTop - olimar.min.y) * std::max(fit.scale, 0.05f);
	const float s      = target / height;
	const Vec3 centre { (mn.x + mx.x) * 0.5f, mn.y, (mn.z + mx.z) * 0.5f };
	const Vec3 t { -centre.x * s, olimar.min.y - centre.y * s + fit.offsetY, -centre.z * s + fit.offsetZ };
	return translation(t) * uniformScale(s) * r;
}

std::vector<BoneInfo> classifyBones(const ImpModel& model, const Mat34& fit)
{
	std::vector<BoneInfo> out(model.bones.size());
	std::vector<char> known(model.bones.size(), 0);
	for (std::size_t i = 0; i < model.bones.size(); ++i) {
		const ImpBone& b = model.bones[i];
		Parsed p;
		if (!parsePackBone(b.name, p)) p = b.humanoid.empty() ? parseName(b.name) : parseHumanoid(b.humanoid);
		if (p.kind == B_NONE && !b.humanoid.empty()) p = parseName(b.name);
		out[i].kind = p.kind;
		out[i].side = p.side;
		known[i]    = p.kind != B_NONE;
	}
	// Desconocidos (pelo, falda, accesorios): como su padre. Los padres van
	// antes en glTF casi siempre; si no, unas pasadas más lo resuelven.
	for (int pass = 0; pass < 4; ++pass) {
		for (std::size_t i = 0; i < model.bones.size(); ++i) {
			if (known[i]) continue;
			const int p = model.bones[i].parent;
			if (p >= 0 && out[std::size_t(p)].kind != B_NONE) {
				out[i].kind    = out[std::size_t(p)].kind == B_HIPS ? B_SPINE : out[std::size_t(p)].kind;
				out[i].side    = out[std::size_t(p)].side;
				out[i].guessed = true;
			}
		}
	}
	for (std::size_t i = 0; i < model.bones.size(); ++i) {
		BoneInfo& b = out[i];
		// Brazos y piernas sin lado en el nombre: por dónde están.
		if ((isArm(b.kind) || isLeg(b.kind)) && b.side == 0) b.side = fit.point(model.bones[i].head).x >= 0 ? 1 : -1;
		if (b.kind == B_NONE) b.guessed = true;
		b.joint = jointFor(b.kind, b.side);
		if (b.kind == B_ANTENNA) {
			int depth = 0;
			for (int p = model.bones[i].parent; p >= 0 && out[std::size_t(p)].kind == B_ANTENNA && depth < 2; p = model.bones[std::size_t(p)].parent) ++depth;
			b.joint = J_ANT1 + depth;
		}
	}
	return out;
}

std::array<Vec3, M_COUNT> estimateMarkers(const ImpModel& model, const OlimarRig& olimar, const FitSettings& fit)
{
	const Mat34 F                 = fitTransform(model, olimar, fit);
	const std::vector<Vec3> pts   = fittedPositions(model, F);
	std::array<Vec3, M_COUNT> m {};
	if (pts.empty()) return m;
	Vec3 mn = pts[0], mx = pts[0];
	for (const Vec3& p : pts) {
		mn = vmin(mn, p);
		mx = vmax(mx, p);
	}
	const float H = std::max(mx.y - mn.y, 1e-4f);
	auto slice    = [&](float y, float half, std::vector<const Vec3*>& out) {
        out.clear();
        for (const Vec3& p : pts)
            if (std::fabs(p.y - y) <= half) out.push_back(&p);
	};
	std::vector<const Vec3*> sl;

	// Entrepierna: lo más alto (desde abajo, sin cortes) donde la malla se
	// parte en dos alrededor de x = 0.
	float crotch = -1;
	for (float f = 0.04f; f < 0.65f; f += 0.01f) {
		slice(mn.y + f * H, H * 0.008f, sl);
		bool left = false, right = false, middle = false;
		for (const Vec3* p : sl) {
			if (std::fabs(p->x) < H * 0.012f) middle = true;
			else if (p->x > 0) left = true;
			else right = true;
		}
		if (left && right && !middle) crotch = mn.y + f * H;
		else if (crotch > 0) break;
	}
	const bool legsFound = crotch > 0;
	if (!legsFound) crotch = mn.y + 0.42f * H; // vestido o túnica
	float legX = 0, legZ = 0;
	slice(mn.y + (crotch - mn.y) * 0.5f, H * 0.02f, sl);
	{
		std::vector<float> xs;
		for (const Vec3* p : sl) {
			xs.push_back(std::fabs(p->x));
			legZ += p->z;
		}
		if (!xs.empty()) {
			std::nth_element(xs.begin(), xs.begin() + std::ptrdiff_t(xs.size() / 2), xs.end());
			legX = xs[xs.size() / 2];
			legZ /= float(sl.size());
		}
		if (!legsFound || legX < H * 0.03f) legX = std::max(legX * 0.6f, H * 0.08f);
	}

	// Brazos: en cruz si la malla es mucho más ancha que el cuerpo.
	float torsoHalf = H * 0.12f, torsoZ = 0;
	slice(crotch + (mx.y - crotch) * 0.35f, H * 0.02f, sl);
	{
		std::vector<float> xs;
		for (const Vec3* p : sl) {
			xs.push_back(std::fabs(p->x));
			torsoZ += p->z;
		}
		if (!xs.empty()) {
			std::sort(xs.begin(), xs.end());
			torsoHalf = std::max(xs[std::size_t(float(xs.size() - 1) * 0.6f)], H * 0.04f);
			torsoZ /= float(sl.size());
		}
	}
	const float width = mx.x - mn.x;
	float shoulderY, shoulderX, handX, handY;
	if (width > 0.75f * H) { // en cruz (T o A)
		const Vec3* far = &pts[0];
		for (const Vec3& p : pts)
			if (std::fabs(p.x) > std::fabs(far->x)) far = &p;
		handX     = std::fabs(far->x) * 0.88f;
		handY     = far->y;
		shoulderY = std::max(handY, crotch + (mx.y - crotch) * 0.55f);
		slice(shoulderY - H * 0.06f, H * 0.015f, sl);
		float w = 0;
		for (const Vec3* p : sl)
			if (std::fabs(p->x) < width * 0.25f) w = std::max(w, std::fabs(p->x));
		shoulderX = std::max(w * 0.85f, torsoHalf * 0.8f);
	} else { // brazos abajo
		shoulderY = crotch + (mx.y - crotch) * 0.64f;
		slice(shoulderY - H * 0.02f, H * 0.015f, sl);
		float w = torsoHalf;
		for (const Vec3* p : sl) w = std::max(w, std::fabs(p->x));
		shoulderX = w * 0.72f;
		handX     = w * 0.85f;
		handY     = crotch + (shoulderY - crotch) * 0.05f;
	}
	const float neckY = std::min(shoulderY + H * 0.07f, mx.y - H * 0.05f);

	m[M_PELVIS]    = { 0, crotch + H * 0.04f, torsoZ };
	m[M_NECK]      = { 0, neckY, torsoZ };
	m[M_HEADTOP]   = { 0, mx.y, torsoZ };
	m[M_LSHOULDER] = { shoulderX, shoulderY, torsoZ };
	m[M_RSHOULDER] = { -shoulderX, shoulderY, torsoZ };
	m[M_LHAND]     = { handX, handY, torsoZ };
	m[M_RHAND]     = { -handX, handY, torsoZ };
	m[M_LHIP]      = { legX, crotch, legZ };
	m[M_RHIP]      = { -legX, crotch, legZ };
	m[M_LFOOT]     = { legX, mn.y + H * 0.04f, legZ };
	m[M_RFOOT]     = { -legX, mn.y + H * 0.04f, legZ };
	// Punta del tallo: el del Pikmin de referencia, a la escala del modelo y
	// saliendo de la coronilla.
	{
		const float k     = H / std::max(olimar.bodyTop - olimar.min.y, 1e-3f);
		m[M_STEMTOP]      = m[M_HEADTOP] + (olimar.pivot(J_ANT3) - olimar.pivot(J_ANT1)) * k;
	}

	// Con esqueleto: las articulaciones de verdad.
	if (model.skinned && fit.useSkeleton) {
		const std::vector<BoneInfo> info = classifyBones(model, F);
		bool set[M_COUNT]                = {};
		auto put                         = [&](int marker, std::size_t bone) {
            if (set[marker]) return;
            m[std::size_t(marker)] = F.point(model.bones[bone].head);
            set[marker]            = true;
		};
		for (std::size_t i = 0; i < info.size(); ++i) {
			const BoneInfo& b = info[i];
			if (b.guessed) continue;
			const int parent      = model.bones[i].parent;
			const BoneKind pk     = parent >= 0 ? info[std::size_t(parent)].kind : B_NONE;
			const bool left       = b.side > 0;
			if (b.kind == B_HIPS) put(M_PELVIS, i);
			else if (b.kind == B_NECK) put(M_NECK, i);
			else if (isArm(b.kind) && b.kind != B_HAND && !isArm(pk)) put(left ? M_LSHOULDER : M_RSHOULDER, i);
			else if (b.kind == B_HAND && pk != B_HAND) put(left ? M_LHAND : M_RHAND, i);
			else if (isLeg(b.kind) && b.kind != B_FOOT && !isLeg(pk)) put(left ? M_LHIP : M_RHIP, i);
			else if (b.kind == B_FOOT && pk != B_FOOT) put(left ? M_LFOOT : M_RFOOT, i);
			else if (b.kind == B_ANTENNA && fit.pikmin) {
				// La punta: el último hueso del tallo (se queda el más alto).
				const Vec3 h = F.point(model.bones[i].head);
				if (!set[M_STEMTOP] || h.y > m[M_STEMTOP].y) m[M_STEMTOP] = h;
				set[M_STEMTOP] = true;
			}
		}
		if (!set[M_NECK])
			for (std::size_t i = 0; i < info.size(); ++i)
				if (!info[i].guessed && info[i].kind == B_HEAD) {
					put(M_NECK, i);
					break;
				}
	}
	return m;
}

void autoFit(const ImpModel& model, const OlimarRig& olimar, FitSettings& fit)
{
	if (fit.keepPlacement) {
		fit.scale = 1.0f, fit.offsetY = fit.offsetZ = 0, fit.quarter = 0, fit.lyingDown = false, fit.markersCustom = false;
		fit.markers = estimateMarkers(model, olimar, fit);
		return;
	}
	fit.scale         = 1.0f;
	fit.quarter       = 0;
	fit.lyingDown     = false;
	fit.offsetY       = 0;
	fit.offsetZ       = 0;
	fit.markersCustom = false;
	if (model.skinned) {
		// Tumbado: la cabeza no está encima de la cadera.
		const std::vector<BoneInfo> raw = classifyBones(model, orientation(model, fit));
		Vec3 head, hips;
		bool hasHead = false, hasHips = false;
		for (std::size_t i = 0; i < raw.size(); ++i) {
			if (raw[i].guessed) continue;
			if ((raw[i].kind == B_HEAD || raw[i].kind == B_NECK) && !hasHead) head = model.bones[i].head, hasHead = true;
			if (raw[i].kind == B_HIPS && !hasHips) hips = model.bones[i].head, hasHips = true;
		}
		if (hasHead && hasHips) {
			const Vec3 d = head - hips;
			if (std::fabs(d.z) > std::fabs(d.y) * 1.5f) fit.lyingDown = d.z > 0;
		}
		// De frente: la izquierda del personaje en +X (Olimar mira hacia +Z).
		const Mat34 o = orientation(model, fit);
		float leftX = 0;
		int count   = 0;
		for (std::size_t i = 0; i < raw.size(); ++i) {
			if (raw[i].guessed || raw[i].side == 0 || !(isArm(raw[i].kind) || isLeg(raw[i].kind))) continue;
			leftX += float(raw[i].side) * o.point(model.bones[i].head).x;
			++count;
		}
		if (count && leftX < 0) fit.quarter = 2;
	} else if (!model.verts.empty()) {
		Vec3 mn = model.verts[0].p, mx = mn;
		for (const SkinVertex& v : model.verts) {
			mn = vmin(mn, v.p);
			mx = vmax(mx, v.p);
		}
		if (mx.z - mn.z > 1.6f * (mx.y - mn.y)) fit.lyingDown = true;
	}
	fit.markers = estimateMarkers(model, olimar, fit);
}

RigResult buildRig(const ImpModel& model, const OlimarRig& olimar, const FitSettings& fit)
{
	RigResult r;
	const Mat34 F = fitTransform(model, olimar, fit);
	const std::array<Vec3, M_COUNT>& mk = fit.markers;
	r.verts.reserve(model.verts.size());
	for (const SkinVertex& v : model.verts) {
		SkinVertex o = v;
		o.p          = F.point(v.p);
		o.n          = normalize(F.dir(v.n));
		r.verts.push_back(o);
	}
	r.triMaterial = model.triMaterial;
	if (r.verts.empty()) return r;
	Vec3 mn = r.verts[0].p, mx = mn;
	for (const SkinVertex& v : r.verts) {
		mn = vmin(mn, v.p);
		mx = vmax(mx, v.p);
	}
	const float H = std::max(mx.y - mn.y, 1e-4f);

	// Pesos por articulación de Olimar (densos: son 12).
	const std::size_t n = r.verts.size();
	std::vector<std::array<float, kJoints>> w(n);
	std::vector<char> autoW(n, 1);
	r.bones = classifyBones(model, F);
	if (model.skinned && fit.useSkeleton) {
		r.fromSkeleton = true;
		float mapped = 0, total = 0;
		for (std::size_t i = 0; i < n; ++i) {
			const SkinVertex& v = model.verts[i];
			float sum           = 0;
			w[i].fill(0);
			for (int k = 0; k < 4; ++k) {
				if (v.weight[k] <= 0 || v.joint[k] < 0 || std::size_t(v.joint[k]) >= r.bones.size()) continue;
				const BoneInfo& b = r.bones[std::size_t(v.joint[k])];
				w[i][std::size_t(b.joint)] += v.weight[k];
				sum += v.weight[k];
				if (!b.guessed) mapped += v.weight[k];
			}
			total += sum;
			if (sum > 0) autoW[i] = 0;
		}
		r.mappedShare = total > 0 ? mapped / total : 0;
		int unknown   = 0;
		for (const BoneInfo& b : r.bones) unknown += b.kind == B_NONE;
		if (unknown) r.skeletonNote = std::to_string(unknown) + L(" huesos sin reconocer (van con el cuerpo)", " unrecognised bones (they follow the body)");
	}

	// Automáticos: cada vértice a los dos trozos de esqueleto más cercanos,
	// con límites de anatomía (un brazo no se lleva la tripa).
	// Cada trozo con su grosor: el cuerpo es ancho y los brazos finos, así que
	// el costado del cuerpo no se lo lleva el brazo que cuelga a su lado.
	struct Seg {
		int joint;
		Vec3 a, b;
		float radius;
	};
	const float torsoR = 0.7f * std::max(std::fabs(mk[M_LSHOULDER].x), std::fabs(mk[M_RSHOULDER].x));
	const float headR  = 0.4f * std::max(mk[M_HEADTOP].y - mk[M_NECK].y, 0.0f);
	const float legR   = 0.6f * std::max(std::fabs(mk[M_LHIP].x), std::fabs(mk[M_RHIP].x));
	const Vec3 armL = mk[M_LHAND] + (mk[M_LHAND] - mk[M_LSHOULDER]) * 0.25f;
	const Vec3 armR = mk[M_RHAND] + (mk[M_RHAND] - mk[M_RSHOULDER]) * 0.25f;
	const Seg segs[] = {
		{ J_SPINE, mk[M_PELVIS], mk[M_NECK], torsoR },
		{ J_HEAD, mk[M_NECK], mk[M_HEADTOP], headR },
		{ J_LARM, mk[M_LSHOULDER], armL, H * 0.035f },
		{ J_RARM, mk[M_RSHOULDER], armR, H * 0.035f },
		{ J_LLEG, mk[M_LHIP], mk[M_LFOOT] - Vec3 { 0, H * 0.06f, 0 }, legR },
		{ J_RLEG, mk[M_RHIP], mk[M_RFOOT] - Vec3 { 0, H * 0.06f, 0 }, legR },
		// Pikmin: el tallo, de la coronilla a la punta, en tres trozos.
		{ fit.pikmin ? J_ANT1 : -1, mk[M_HEADTOP], mk[M_HEADTOP] + (mk[M_STEMTOP] - mk[M_HEADTOP]) * 0.33f, H * 0.015f },
		{ fit.pikmin ? J_ANT2 : -1, mk[M_HEADTOP] + (mk[M_STEMTOP] - mk[M_HEADTOP]) * 0.33f, mk[M_HEADTOP] + (mk[M_STEMTOP] - mk[M_HEADTOP]) * 0.66f, H * 0.015f },
		{ fit.pikmin ? J_ANT3 : -1, mk[M_HEADTOP] + (mk[M_STEMTOP] - mk[M_HEADTOP]) * 0.66f, mk[M_STEMTOP], H * 0.015f },
	};
	const float hipTop = std::max(mk[M_LHIP].y, mk[M_RHIP].y) + H * 0.04f;
	for (std::size_t i = 0; i < n; ++i) {
		if (!autoW[i]) continue;
		++r.autoVertices;
		const Vec3& p = r.verts[i].p;
		float best[2] = { 0, 0 };
		int bj[2]     = { J_SPINE, J_SPINE };
		for (const Seg& s : segs) {
			if (s.joint < 0) continue;
			bool allowed = true;
			if (s.joint == J_LARM || s.joint == J_RARM) {
				const float side = s.joint == J_LARM ? 1.0f : -1.0f;
				allowed          = side * p.x > 0.6f * std::fabs(s.a.x);
			} else if (s.joint == J_LLEG || s.joint == J_RLEG) {
				const float side = s.joint == J_LLEG ? 1.0f : -1.0f;
				allowed          = p.y < hipTop && side * p.x > -H * 0.02f;
			} else if (s.joint == J_HEAD) {
				allowed = p.y > mk[M_NECK].y - H * 0.04f;
			} else if (s.joint == J_ANT1 || s.joint == J_ANT2 || s.joint == J_ANT3) {
				allowed = p.y > mk[M_HEADTOP].y - H * 0.05f; // solo lo que sale por encima de la cabeza
			}
			if (!allowed) continue;
			const float d = std::max(segmentDistance(p, s.a, s.b) - s.radius, 0.0f) + H * 0.01f;
			const float v = 1.0f / (d * d * d * d);
			if (v > best[0]) {
				best[1] = best[0], bj[1] = bj[0];
				best[0] = v, bj[0] = s.joint;
			} else if (v > best[1]) {
				best[1] = v, bj[1] = s.joint;
			}
		}
		w[i].fill(0);
		const float sum = best[0] + best[1];
		if (sum <= 0 || best[1] / sum < 0.2f) {
			w[i][std::size_t(bj[0])] = 1;
		} else {
			w[i][std::size_t(bj[0])] += best[0] / sum;
			w[i][std::size_t(bj[1])] += best[1] / sum;
		}
	}

	// Vértices soldados (misma posición) y sus vecinos: para suavizar los
	// automáticos y encontrar las piezas sueltas.
	if (r.autoVertices) {
		const float cell = H * 1e-4f;
		std::unordered_map<std::uint64_t, int> grid;
		std::vector<int> weld(n);
		auto key = [&](const Vec3& p) {
			const auto q = [&](float f) { return std::uint64_t(std::int64_t(std::llround(f / cell)) & 0x1fffff); };
			return q(p.x) | q(p.y) << 21 | q(p.z) << 42;
		};
		for (std::size_t i = 0; i < n; ++i) weld[i] = grid.emplace(key(r.verts[i].p), int(grid.size())).first->second;
		const std::size_t wn = grid.size();
		std::vector<std::vector<int>> nb(wn);
		for (std::size_t t = 0; t + 2 < n; t += 3) {
			for (int a = 0; a < 3; ++a) {
				const int x = weld[t + std::size_t(a)], y = weld[t + std::size_t((a + 1) % 3)];
				if (x == y) continue;
				nb[std::size_t(x)].push_back(y);
				nb[std::size_t(y)].push_back(x);
			}
		}
		for (auto& v : nb) {
			std::sort(v.begin(), v.end());
			v.erase(std::unique(v.begin(), v.end()), v.end());
		}
		std::vector<std::array<float, kJoints>> ww(wn);
		std::vector<char> wAuto(wn, 0);
		for (auto& a : ww) a.fill(0);
		for (std::size_t i = 0; i < n; ++i) {
			ww[std::size_t(weld[i])] = w[i];
			if (autoW[i]) wAuto[std::size_t(weld[i])] = 1;
		}
		for (int it = 0; it < 3; ++it) {
			std::vector<std::array<float, kJoints>> next = ww;
			for (std::size_t v = 0; v < wn; ++v) {
				if (!wAuto[v] || nb[v].empty()) continue;
				std::array<float, kJoints> avg {};
				for (int u : nb[v])
					for (int j = 0; j < kJoints; ++j) avg[std::size_t(j)] += ww[std::size_t(u)][std::size_t(j)];
				for (int j = 0; j < kJoints; ++j) next[v][std::size_t(j)] = 0.5f * ww[v][std::size_t(j)] + 0.5f * avg[std::size_t(j)] / float(nb[v].size());
			}
			ww.swap(next);
		}
		// Piezas pequeñas sueltas (botones, gafas, un sombrero): rígidas con
		// la articulación que más pesa en ellas.
		if (!r.fromSkeleton) {
			std::vector<int> parent(wn);
			std::iota(parent.begin(), parent.end(), 0);
			for (std::size_t v = 0; v < wn; ++v)
				for (int u : nb[v]) parent[std::size_t(findRoot(parent, int(v)))] = findRoot(parent, u);
			std::unordered_map<int, std::pair<Vec3, Vec3>> box;
			std::unordered_map<int, std::array<float, kJoints>> sum;
			for (std::size_t i = 0; i < n; ++i) {
				const int root = findRoot(parent, weld[i]);
				auto it        = box.find(root);
				if (it == box.end()) box.emplace(root, std::make_pair(r.verts[i].p, r.verts[i].p));
				else it->second = { vmin(it->second.first, r.verts[i].p), vmax(it->second.second, r.verts[i].p) };
				auto& s = sum[root];
				for (int j = 0; j < kJoints; ++j) s[std::size_t(j)] += ww[std::size_t(weld[i])][std::size_t(j)];
			}
			for (std::size_t v = 0; v < wn; ++v) {
				const int root = findRoot(parent, int(v));
				const auto& b  = box[root];
				if (length(b.second - b.first) > H * 0.12f) continue;
				const auto& s = sum[root];
				const int j   = int(std::max_element(s.begin(), s.end()) - s.begin());
				ww[v].fill(0);
				ww[v][std::size_t(j)] = 1;
			}
		}
		for (std::size_t i = 0; i < n; ++i)
			if (autoW[i]) w[i] = ww[std::size_t(weld[i])];
	}

	// Cuatro como mucho (el formato del pack), normalizados.
	for (std::size_t i = 0; i < n; ++i) {
		SkinVertex& v = r.verts[i];
		std::array<int, kJoints> order;
		std::iota(order.begin(), order.end(), 0);
		std::sort(order.begin(), order.end(), [&](int a, int b) { return w[i][std::size_t(a)] > w[i][std::size_t(b)]; });
		float sum = 0;
		for (int k = 0; k < 4; ++k) sum += w[i][std::size_t(order[std::size_t(k)])];
		for (int k = 0; k < 4; ++k) {
			const int j  = order[std::size_t(k)];
			v.joint[k]   = j == J_FACE ? J_HEAD : j;
			v.weight[k]  = sum > 0 ? w[i][std::size_t(j)] / sum : (k == 0 ? 1.0f : 0.0f);
			if (sum <= 0 && k == 0) v.joint[k] = J_SPINE;
		}
	}

	// Enderezar: brazos (y piernas) hacia donde apuntan los de Olimar en su
	// pose de reposo, para que las animaciones los pongan donde a él.
	r.unbent.reserve(r.verts.size());
	for (const SkinVertex& v : r.verts) r.unbent.push_back(v.p);
	std::array<Mat34, kJoints> pose;
	r.markers = mk;
	auto straighten = [&](int joint, int from, int to) {
		const Vec3 dirC = mk[std::size_t(to)] - mk[std::size_t(from)];
		const Vec3 dirO = olimar.jointCentroid(joint) - olimar.pivot(joint);
		if (length(dirC) < 1e-5f || length(dirO) < 1e-5f) return;
		pose[std::size_t(joint)]  = about(mk[std::size_t(from)], rotationBetween(dirC, dirO));
		r.markers[std::size_t(to)] = pose[std::size_t(joint)].point(mk[std::size_t(to)]);
	};
	if (fit.straightenArms) {
		straighten(J_LARM, M_LSHOULDER, M_LHAND);
		straighten(J_RARM, M_RSHOULDER, M_RHAND);
	}
	if (fit.straightenLegs) {
		straighten(J_LLEG, M_LHIP, M_LFOOT);
		straighten(J_RLEG, M_RHIP, M_RFOOT);
	}
	if (fit.straightenArms || fit.straightenLegs) {
		for (SkinVertex& v : r.verts) {
			Vec3 p, nn;
			for (int k = 0; k < 4; ++k) {
				if (v.weight[k] <= 0) continue;
				const Mat34& m = pose[std::size_t(v.joint[k])];
				p += m.point(v.p) * v.weight[k];
				nn += m.dir(v.n) * v.weight[k];
			}
			v.p = p;
			v.n = normalize(nn);
		}
	}

	// Pivotes propios. La antena y el ayudante de la cabeza siguen al cuello
	// con las distancias de Olimar a la escala del personaje.
	const float k = (mk[M_HEADTOP].y - mn.y) / std::max(olimar.bodyTop - olimar.min.y, 1e-3f);
	r.pivots[J_ROOT] = r.pivots[J_SPINE] = r.pivots[J_LEGS] = mk[M_PELVIS];
	r.pivots[J_HEAD]  = mk[M_NECK];
	r.pivots[J_FACE]  = mk[M_NECK] + (olimar.pivot(J_FACE) - olimar.pivot(J_HEAD)) * k;
	if (fit.pikmin) {
		// El tallo sale de la coronilla y acaba en la punta marcada; el codo
		// del medio, como en el Pikmin original, girado y escalado a su sitio.
		const Vec3 base = mk[M_HEADTOP], tip = mk[M_STEMTOP];
		const Vec3 o4 = olimar.pivot(J_ANT1), o5 = olimar.pivot(J_ANT2), o6 = olimar.pivot(J_ANT3);
		const float s = length(tip - base) / std::max(length(o6 - o4), 1e-4f);
		r.pivots[J_ANT1] = base;
		r.pivots[J_ANT2] = base + rotationBetween(o6 - o4, tip - base).dir(o5 - o4) * s;
		r.pivots[J_ANT3] = tip;
	} else {
		r.pivots[J_ANT1] = mk[M_NECK] + (olimar.pivot(J_ANT1) - olimar.pivot(J_HEAD)) * k;
		r.pivots[J_ANT2] = r.pivots[J_ANT1] + (olimar.pivot(J_ANT2) - olimar.pivot(J_ANT1)) * k;
		r.pivots[J_ANT3] = r.pivots[J_ANT2] + (olimar.pivot(J_ANT3) - olimar.pivot(J_ANT2)) * k;
	}
	r.pivots[J_RARM]  = mk[M_RSHOULDER];
	r.pivots[J_LARM]  = mk[M_LSHOULDER];
	r.pivots[J_RLEG]  = mk[M_RHIP];
	r.pivots[J_LLEG]  = mk[M_LHIP];

	// Pikmin sin tallo propio: el del original, llevado a los pivotes nuevos
	// (cada vértice con sus pesos, como lo movería el juego).
	if (fit.pikmin && fit.addStem && fit.stemMaterial >= 0) {
		bool hasStem = false;
		for (const SkinVertex& v : r.verts)
			for (int i = 0; i < 4 && !hasStem; ++i) hasStem = v.weight[i] > 0.3f && (v.joint[i] == J_ANT1 || v.joint[i] == J_ANT2 || v.joint[i] == J_ANT3);
		if (!hasStem) {
			std::array<Mat34, kJoints> move;
			for (int j = 0; j < kJoints; ++j) {
				Mat34 own = olimar.bind[std::size_t(j)];
				setTranslation(own, r.pivots[std::size_t(j)]);
				move[std::size_t(j)] = own * inverse(olimar.bind[std::size_t(j)]);
			}
			auto stemWeight = [](const SkinVertex& v) {
				float w = 0;
				for (int i = 0; i < 4; ++i)
					if (v.joint[i] == J_ANT1 || v.joint[i] == J_ANT2 || v.joint[i] == J_ANT3) w += v.weight[i];
				return w;
			};
			for (std::size_t t = 0; t + 2 < olimar.verts.size(); t += 3) {
				if (stemWeight(olimar.verts[t]) < 0.5f || stemWeight(olimar.verts[t + 1]) < 0.5f || stemWeight(olimar.verts[t + 2]) < 0.5f) continue;
				for (int k3 = 0; k3 < 3; ++k3) {
					SkinVertex v = olimar.verts[t + std::size_t(k3)];
					Vec3 p, n;
					for (int i = 0; i < 4; ++i) {
						if (v.weight[i] <= 0) continue;
						if (v.joint[i] == J_FACE) v.joint[i] = J_HEAD;
						p += move[std::size_t(v.joint[i])].point(v.p) * v.weight[i];
						n += move[std::size_t(v.joint[i])].dir(v.n) * v.weight[i];
					}
					v.p = p;
					v.n = normalize(n);
					r.verts.push_back(v);
					r.unbent.push_back(v.p);
				}
				r.triMaterial.push_back(fit.stemMaterial);
				++r.stemTriangles;
			}
		}
	}

	r.min = r.max = r.verts[0].p;
	for (const SkinVertex& v : r.verts) {
		r.min = vmin(r.min, v.p);
		r.max = vmax(r.max, v.p);
	}
	return r;
}

void retargetSkin(const OlimarRig& olimar, const std::array<Vec3, kJoints>& pivots, const std::vector<Mat34>& anim, std::vector<Mat34>& skin)
{
	const std::size_t count = olimar.bind.size();
	std::vector<Mat34> bind = olimar.bind, packBind = olimar.bind, packAnim(count);
	for (std::size_t j = 0; j < count; ++j)
		if (packBoneName(int(j))) setTranslation(packBind[j], pivots[j]);
	for (std::size_t j = 0; j < count; ++j) {
		const int parent = olimar.parent[j];
		if (parent < 0 || std::size_t(parent) >= j) {
			packAnim[j] = anim[j] * inverse(bind[j]) * packBind[j];
			continue;
		}
		Mat34 local            = inverse(anim[std::size_t(parent)]) * anim[j];
		const Mat34 bindLocal  = inverse(bind[std::size_t(parent)]) * bind[j];
		const Mat34 packLocal  = inverse(packBind[std::size_t(parent)]) * packBind[j];
		setTranslation(local, translationOf(local) + translationOf(packLocal) - translationOf(bindLocal));
		packAnim[j] = packAnim[std::size_t(parent)] * local;
	}
	skin.resize(count);
	for (std::size_t j = 0; j < count; ++j) skin[j] = packAnim[j] * inverse(packBind[j]);
}

} // namespace mm
