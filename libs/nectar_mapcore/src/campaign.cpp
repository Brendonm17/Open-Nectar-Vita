#include "nectar_mapcore/campaign.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>

#include "nectar_mapcore/i18n.h"

namespace nectar {

using json = nlohmann::json;

namespace {

const char* const kColorNames[] = { "blue", "red", "yellow" };
const char* const kRuleTypes[]  = { "parts", "part", "pikmin", "onion", "zone", "visited", "day", "medal" };

int colorOf(const std::string& s)
{
	for (int i = 0; i < 3; i++) {
		if (s == kColorNames[i]) return i;
	}
	throw std::runtime_error(tr("color desconocido: ") + s);
}

bool safeId(const std::string& id)
{
	if (id.empty() || id.size() > 64) return false;
	for (char c : id) {
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
		if (!ok) return false;
	}
	return true;
}

bool safeFile(const std::string& f) { return !f.empty() && f.find("..") == std::string::npos && f[0] != '/' && f.find('\\') == std::string::npos; }

UnlockRule parseRule(const json& j)
{
	UnlockRule r;
	r.type = j.at("type").get<std::string>();
	if (std::find(std::begin(kRuleTypes), std::end(kRuleTypes), r.type) == std::end(kRuleTypes))
		throw std::runtime_error(tr("regla de desbloqueo desconocida: ") + r.type);
	r.count = j.value("count", 1);
	r.id    = j.value("id", std::string());
	if (j.contains("color")) r.color = colorOf(j.at("color").get<std::string>());
	return r;
}

json ruleJson(const UnlockRule& r)
{
	json j = { { "type", r.type } };
	if (r.type == "parts" || r.type == "pikmin" || r.type == "day" || r.type == "medal") j["count"] = r.count;
	if (!r.id.empty()) j["id"] = r.id;
	if (r.color >= 0 && r.color < 3) j["color"] = kColorNames[r.color];
	return j;
}

bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

bool ruleMet(const UnlockRule& r, const CampaignProgress& p)
{
	if (r.type == "parts") return int(p.parts.size()) >= r.count;
	if (r.type == "part") return has(p.parts, r.id);
	if (r.type == "pikmin") return (r.color >= 0 && r.color < 3 ? p.pikmin[r.color] : p.pikmin[0] + p.pikmin[1] + p.pikmin[2]) >= r.count;
	if (r.type == "onion") return r.color >= 0 && r.color < 3 && p.onions[r.color];
	if (r.type == "zone") return has(p.completed, r.id);
	if (r.type == "visited") return has(p.visited, r.id) || has(p.completed, r.id);
	if (r.type == "day") return p.day >= r.count;
	if (r.type == "medal") {
		const auto it = p.medals.find(r.id);
		return it != p.medals.end() && it->second >= r.count;
	}
	return false;
}

std::string partName(std::uint32_t id)
{
	std::string s;
	for (int b = 3; b >= 0; b--) s += char((id >> (8 * b)) & 0xFF);
	return s;
}

} // namespace

int CampaignDoc::find(const std::string& id) const
{
	for (size_t i = 0; i < zones.size(); i++) {
		if (zones[i].id == id) return int(i);
	}
	return -1;
}

int CampaignDoc::startZone() const
{
	const int z = find(start.zone);
	return z >= 0 ? z : (zones.empty() ? -1 : 0);
}

bool parseCampaignDoc(const std::string& text, CampaignDoc& out, std::string& error)
{
	try {
		const json root = json::parse(text);
		if (root.value("format", 1) != 1) throw std::runtime_error(tr("versión de formato no soportada"));
		CampaignDoc d;
		d.zones.clear();
		const json& meta = root.at("meta");
		d.name           = meta.at("name").get<std::string>();
		d.author         = meta.value("author", std::string());
		d.version        = meta.value("version", std::string("1.0"));
		d.worldmap       = root.value("worldmap", std::string("game"));
		d.intro          = root.value("intro", std::string());
		d.nameEn         = meta.value("name_en", std::string());
		d.introEn        = root.value("intro_en", std::string());
		d.introImage     = root.value("introImage", std::string());
		for (const json& j : root.value("diary", json::array())) {
			CampaignDiary e;
			e.day    = j.value("day", 0);
			e.zone   = j.value("zone", std::string());
			e.text   = j.value("text", std::string());
			e.textEn = j.value("text_en", std::string());
			d.diary.push_back(e);
		}
		for (const json& j : root.value("pois", json::array())) {
			CampaignPoi p;
			if (j.contains("pos")) {
				p.x = j["pos"].at(0).get<double>();
				p.y = j["pos"].at(1).get<double>();
			}
			p.name   = j.value("name", std::string());
			p.nameEn = j.value("name_en", std::string());
			p.text   = j.value("text", std::string());
			p.textEn = j.value("text_en", std::string());
			d.pois.push_back(p);
		}
		for (const json& zj : root.at("zones")) {
			CampaignZone z;
			z.id   = zj.at("id").get<std::string>();
			z.name   = zj.value("name", z.id);
			z.nameEn = zj.value("name_en", std::string());
			if (zj.contains("pos")) {
				z.x = zj["pos"].at(0).get<double>();
				z.y = zj["pos"].at(1).get<double>();
			}
			z.photo = zj.value("photo", std::string());
			z.music = zj.value("music", std::string());
			if (zj.contains("unlock")) {
				const json& u = zj.at("unlock");
				for (const json& r : u.value("all", json::array())) z.unlock.all.push_back(parseRule(r));
				for (const json& r : u.value("any", json::array())) z.unlock.any.push_back(parseRule(r));
			}
			d.zones.push_back(z);
		}
		for (const json& p : root.value("paths", json::array())) d.paths.push_back({ p.at(0).get<std::string>(), p.at(1).get<std::string>() });
		if (root.contains("start")) {
			const json& s = root.at("start");
			d.start.zone  = s.value("zone", std::string());
			if (s.contains("onions")) {
				for (bool& o : d.start.onions) o = false;
				for (const json& c : s.at("onions")) d.start.onions[colorOf(c.get<std::string>())] = true;
			}
			if (s.contains("pikmin")) {
				for (int c = 0; c < 3; c++) d.start.pikmin[c] = s["pikmin"].value(kColorNames[c], 0);
			}
		}
		if (root.contains("ending")) {
			const json& e    = root.at("ending");
			d.ending.type     = e.value("type", std::string("parts"));
			d.ending.count    = e.value("count", 0);
			d.ending.zone     = e.value("zone", std::string());
			d.ending.dayLimit = e.value("dayLimit", 0);
			d.ending.goodText = e.value("good", std::string());
			d.ending.badText  = e.value("bad", std::string());
			d.ending.goodTextEn = e.value("good_en", std::string());
			d.ending.badTextEn  = e.value("bad_en", std::string());
			d.ending.image      = e.value("image", std::string());
			if (d.ending.type != "parts" && d.ending.type != "zone") throw std::runtime_error(tr("final desconocido: ") + d.ending.type);
		}
		out = std::move(d);
		return true;
	} catch (const std::exception& e) {
		error = e.what();
		return false;
	}
}

std::string writeCampaignDoc(const CampaignDoc& d)
{
	json zones = json::array();
	for (const CampaignZone& z : d.zones) {
		json zj = { { "id", z.id }, { "name", z.name }, { "pos", { z.x, z.y } } };
		if (!z.nameEn.empty()) zj["name_en"] = z.nameEn;
		if (!z.photo.empty()) zj["photo"] = z.photo;
		if (!z.music.empty()) zj["music"] = z.music;
		if (!z.unlock.empty()) {
			json all = json::array(), any = json::array();
			for (const UnlockRule& r : z.unlock.all) all.push_back(ruleJson(r));
			for (const UnlockRule& r : z.unlock.any) any.push_back(ruleJson(r));
			zj["unlock"] = json::object();
			if (!all.empty()) zj["unlock"]["all"] = all;
			if (!any.empty()) zj["unlock"]["any"] = any;
		}
		zones.push_back(zj);
	}
	json paths = json::array();
	for (const auto& p : d.paths) paths.push_back({ p.first, p.second });
	json onions = json::array(), pikmin = json::object();
	for (int c = 0; c < 3; c++) {
		if (d.start.onions[c]) onions.push_back(kColorNames[c]);
		pikmin[kColorNames[c]] = d.start.pikmin[c];
	}
	json root;
	root["format"]   = 1;
	root["meta"]     = { { "name", d.name }, { "author", d.author }, { "version", d.version } };
	root["worldmap"] = d.worldmap;
	if (!d.intro.empty()) root["intro"] = d.intro;
	if (!d.nameEn.empty()) root["meta"]["name_en"] = d.nameEn;
	if (!d.introEn.empty()) root["intro_en"] = d.introEn;
	if (!d.introImage.empty()) root["introImage"] = d.introImage;
	if (!d.diary.empty()) {
		json diary = json::array();
		for (const CampaignDiary& e : d.diary) {
			json j = { { "day", e.day }, { "text", e.text } };
			if (!e.zone.empty()) j["zone"] = e.zone;
			if (!e.textEn.empty()) j["text_en"] = e.textEn;
			diary.push_back(j);
		}
		root["diary"] = diary;
	}
	if (!d.pois.empty()) {
		json pois = json::array();
		for (const CampaignPoi& p : d.pois) {
			json j = { { "pos", { p.x, p.y } }, { "name", p.name }, { "text", p.text } };
			if (!p.nameEn.empty()) j["name_en"] = p.nameEn;
			if (!p.textEn.empty()) j["text_en"] = p.textEn;
			pois.push_back(j);
		}
		root["pois"] = pois;
	}
	root["zones"] = zones;
	root["paths"] = paths;
	root["start"] = { { "zone", d.start.zone }, { "onions", onions }, { "pikmin", pikmin } };
	json ending   = { { "type", d.ending.type }, { "count", d.ending.count }, { "dayLimit", d.ending.dayLimit } };
	if (!d.ending.zone.empty()) ending["zone"] = d.ending.zone;
	if (!d.ending.goodText.empty()) ending["good"] = d.ending.goodText;
	if (!d.ending.badText.empty()) ending["bad"] = d.ending.badText;
	if (!d.ending.goodTextEn.empty()) ending["good_en"] = d.ending.goodTextEn;
	if (!d.ending.badTextEn.empty()) ending["bad_en"] = d.ending.badTextEn;
	if (!d.ending.image.empty()) ending["image"] = d.ending.image;
	root["ending"] = ending;
	return root.dump(2) + "\n";
}

bool unlockMet(const UnlockBlock& b, const CampaignProgress& p)
{
	for (const UnlockRule& r : b.all) {
		if (!ruleMet(r, p)) return false;
	}
	if (b.any.empty()) return true;
	for (const UnlockRule& r : b.any) {
		if (ruleMet(r, p)) return true;
	}
	return false;
}

bool zoneOpen(const CampaignDoc& doc, int zone, const CampaignProgress& p)
{
	if (zone < 0 || zone >= int(doc.zones.size())) return false;
	return zone == doc.startZone() || unlockMet(doc.zones[size_t(zone)].unlock, p);
}

int campaignPartTotal(const std::vector<const MapData*>& maps)
{
	int n = 0;
	for (const MapData* m : maps) {
		if (m) n += int(m->story.parts.size());
	}
	return n;
}

int campaignEnding(const CampaignDoc& d, const CampaignProgress& p, int partTotal)
{
	const bool done = d.ending.type == "zone" ? has(p.completed, d.ending.zone)
	                                          : int(p.parts.size()) >= (d.ending.count > 0 ? d.ending.count : partTotal);
	if (done && (d.ending.type == "zone" || partTotal > 0)) return 1;
	if (d.ending.dayLimit > 0 && p.day > d.ending.dayLimit) return 0;
	return -1;
}

bool validateCampaign(const CampaignDoc& d, const std::vector<const MapData*>& maps, bool& coop, std::string& error)
{
	try {
		coop = true;
		if (d.zones.empty()) throw std::runtime_error(tr("la campaña no tiene zonas"));
		if (d.zones.size() > 30) throw std::runtime_error(tr("demasiadas zonas (máximo 30)"));
		if (maps.size() != d.zones.size()) throw std::runtime_error(tr("faltan niveles de zonas"));
		if (d.worldmap != "game" && !safeFile(d.worldmap)) throw std::runtime_error(tr("imagen del mapa no válida: ") + d.worldmap);
		for (const std::string* img : { &d.introImage, &d.ending.image }) {
			if (!img->empty() && !safeFile(*img)) throw std::runtime_error(tr("imagen no válida: ") + *img);
		}
		if (d.diary.size() > 200) throw std::runtime_error(tr("el diario tiene demasiadas entradas (máximo 200)"));
		for (const CampaignDiary& e : d.diary) {
			if (!e.zone.empty() && d.find(e.zone) < 0) throw std::runtime_error(tr("diario con una zona que no existe: ") + e.zone);
			if (e.day < 0) throw std::runtime_error(tr("diario: día no válido"));
		}
		if (d.pois.size() > 32) throw std::runtime_error(tr("demasiados puntos de interés (máximo 32)"));
		for (const CampaignPoi& p : d.pois) {
			if (p.x < 0 || p.x > 1 || p.y < 0 || p.y > 1) throw std::runtime_error(tr("punto de interés fuera del mapa: ") + p.name);
		}
		for (size_t i = 0; i < d.zones.size(); i++) {
			const CampaignZone& z = d.zones[i];
			if (!safeId(z.id)) throw std::runtime_error(tr("id de zona no válido (solo letras, números, _ y -): ") + z.id);
			for (size_t k = 0; k < i; k++) {
				if (d.zones[k].id == z.id) throw std::runtime_error(tr("zona repetida: ") + z.id);
			}
			if (z.x < 0 || z.x > 1 || z.y < 0 || z.y > 1) throw std::runtime_error(tr("zona fuera del mapa: ") + z.id);
			if (!z.photo.empty() && z.photo.rfind("game:", 0) != 0 && !safeFile(z.photo)) throw std::runtime_error(tr("foto no válida: ") + z.photo);
			if (!maps[i]) throw std::runtime_error(tr("la zona no se puede leer: ") + z.id);
			if (!(maps[i]->modes & MAPMODE_SOLO)) throw std::runtime_error(tr("la zona no es para solo: ") + z.id);
			if (!(maps[i]->modes & MAPMODE_COOP)) coop = false;
			for (const std::vector<UnlockRule>* list : { &z.unlock.all, &z.unlock.any }) {
				for (const UnlockRule& r : *list) {
					if ((r.type == "zone" || r.type == "visited" || r.type == "medal") && d.find(r.id) < 0)
						throw std::runtime_error(tr("regla con una zona que no existe: ") + r.id);
					if (r.type == "part") {
						bool known = false;
						for (const ShipPartInfo& sp : shipParts()) known = known || r.id == sp.id;
						if (!known) throw std::runtime_error(tr("regla con una pieza que no existe: ") + r.id);
					}
					if (r.type == "onion" && (r.color < 0 || r.color > 2)) throw std::runtime_error(tr("regla de cebolla sin color"));
				}
			}
		}
		if (!d.start.zone.empty() && d.find(d.start.zone) < 0) throw std::runtime_error(tr("la zona de salida no existe: ") + d.start.zone);
		for (const auto& p : d.paths) {
			if (d.find(p.first) < 0 || d.find(p.second) < 0) throw std::runtime_error(tr("camino con una zona que no existe"));
		}
		// Cada pieza, una vez en toda la campaña.
		std::vector<std::uint32_t> seen;
		for (size_t i = 0; i < maps.size(); i++) {
			for (const MapShipPart& part : maps[i]->story.parts) {
				if (std::find(seen.begin(), seen.end(), part.id) != seen.end())
					throw std::runtime_error(tr("la pieza ") + partName(part.id) + tr(" está en más de una zona"));
				seen.push_back(part.id);
			}
		}
		if (d.ending.type == "zone" && d.find(d.ending.zone) < 0) throw std::runtime_error(tr("el final pide una zona que no existe: ") + d.ending.zone);
		if (d.ending.type == "parts" && d.ending.count > int(seen.size())) throw std::runtime_error(tr("el final pide más piezas de las que hay"));
		// Toda zona se puede abrir: se juega «a lo bruto» con todo lo de las
		// zonas abiertas (sus piezas, sus cebollas, medalla de oro, Pikmin y
		// días sin fin) hasta que no se abre ninguna más.
		CampaignProgress p;
		p.day = 1000000;
		for (int c = 0; c < 3; c++) p.onions[c] = d.start.onions[c];
		std::vector<char> open(d.zones.size(), 0);
		for (bool grew = true; grew;) {
			grew = false;
			for (size_t i = 0; i < d.zones.size(); i++) {
				if (open[i] || !zoneOpen(d, int(i), p)) continue;
				open[i] = grew = true;
				const CampaignZone& z = d.zones[i];
				p.completed.push_back(z.id);
				p.visited.push_back(z.id);
				p.medals[z.id] = 2;
				for (const MapShipPart& part : maps[i]->story.parts) p.parts.push_back(partName(part.id));
				for (const MapStoryOnion& o : maps[i]->story.onions) p.onions[o.color] = true;
				for (int c = 0; c < 3; c++) p.pikmin[c] = p.onions[c] ? 1000000 : 0;
			}
		}
		for (size_t i = 0; i < d.zones.size(); i++) {
			if (!open[i]) throw std::runtime_error(tr("la zona no se puede abrir nunca: ") + d.zones[i].id);
		}
		return true;
	} catch (const std::exception& e) {
		error = e.what();
		return false;
	}
}

} // namespace nectar
