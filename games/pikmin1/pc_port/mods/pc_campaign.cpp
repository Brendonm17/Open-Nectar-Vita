#include "pc_campaign.h"
#include "pc_map_catalog.h"
#include "pc_mission.h"
#include "pc_vs_arena.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

#include "settings/pc_settings.h"

namespace {

namespace fs = std::filesystem;

constexpr const char* kPacks = "user/campaigns";
constexpr const char* kSaves = "user/saves/campaigns";

std::vector<PcCampaignPack> sPacks;
bool sScanned = false;

// Partida en curso.
struct ZoneState {
	bool open = false, visited = false, completed = false;
	int medal                 = -1;
	unsigned long long hash   = 0; // del map.json con el que se jugó
	std::string run;               // pc_mission_run_text de la última visita
};
struct Slot {
	int players = 1, day = 1;
	std::string zone; // la última zona elegida
	bool onions[3] = { false, false, false };
	int pikmin[3]  = { 0, 0, 0 };
	int stages[3][3] = {}; // por color y fase (hoja, capullo, flor)
	bool hasStages   = false;
	bool started   = false; // ya se dieron los Pikmin del principio
	std::vector<std::string> parts;
	int ending = -1;
	PcMissionMods mods;
	std::string diary; // entradas ya leídas ('1' / '0', en el orden del paquete)
	std::map<std::string, ZoneState> zones;
};
bool sOpen = false;
bool sTest = false; // prueba desde el editor: no se guarda en ninguna ranura
// --campaign: 1 pedido, 2 en el título, 3 hecho; y la zona ya preparada.
int sDirect = 0;
std::string sDirectDir, sDirectZone;
bool sDirectEntered = false;
PcCampaignPack sPack; // copia: la lista se puede volver a leer
int sSlotIndex = 0;
Slot sSlot;
nectar::CampaignProgress sProgress;
std::vector<int> sNew;
std::string sDiaryText;
std::string sDirectState;

unsigned long long fnv1a(const std::string& s)
{
	unsigned long long h = 1469598103934665603ull;
	for (unsigned char c : s) {
		h ^= c;
		h *= 1099511628211ull;
	}
	return h;
}

std::string partName(std::uint32_t id)
{
	std::string s;
	for (int b = 3; b >= 0; b--) s += char((id >> (8 * b)) & 0xFF);
	return s;
}

bool readText(const std::string& path, std::string& out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

std::string slotPath(const std::string& pack, int slot) { return std::string(kSaves) + "/" + pack + "/slot" + std::to_string(slot + 1) + ".txt"; }

std::string join(const std::vector<std::string>& v)
{
	std::string s;
	for (const std::string& x : v) s += (s.empty() ? "" : ",") + x;
	return s;
}

std::vector<std::string> split(const std::string& s)
{
	std::vector<std::string> out;
	std::string item;
	std::istringstream in(s);
	while (std::getline(in, item, ',')) {
		if (!item.empty()) out.push_back(item);
	}
	return out;
}

bool readSlot(const std::string& path, Slot& out)
{
	std::ifstream f(path);
	if (!f) return false;
	Slot s;
	std::string line;
	while (std::getline(f, line)) {
		const size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
		if (key == "players") s.players = atoi(val.c_str()) == 2 ? 2 : 1;
		else if (key == "day") s.day = std::max(1, atoi(val.c_str()));
		else if (key == "zone") s.zone = val;
		else if (key == "onions") {
			int o[3] = { 0, 0, 0 };
			sscanf(val.c_str(), "%d %d %d", &o[0], &o[1], &o[2]);
			for (int c = 0; c < 3; c++) s.onions[c] = o[c] != 0;
		} else if (key == "pikmin") sscanf(val.c_str(), "%d %d %d", &s.pikmin[0], &s.pikmin[1], &s.pikmin[2]);
		else if (key == "pikminst") {
			int* v      = &s.stages[0][0];
			s.hasStages = sscanf(val.c_str(), "%d %d %d %d %d %d %d %d %d", v, v + 1, v + 2, v + 3, v + 4, v + 5, v + 6, v + 7, v + 8) == 9;
		}
		else if (key == "started") s.started = atoi(val.c_str()) != 0;
		else if (key == "parts") s.parts = split(val);
		else if (key == "ending") s.ending = atoi(val.c_str());
		else if (key == "diary") s.diary = val;
		else if (key == "mods") {
			int noLosses = 0;
			sscanf(val.c_str(), "%d %d %d %d", &s.mods.tekiHealth, &s.mods.extraDays, &noLosses, &s.mods.carry);
			s.mods.noLosses = false; // en campaña no se usa
		}
		else if (key.rfind("z.", 0) == 0) {
			ZoneState& z = s.zones[key.substr(2)];
			int open = 0, visited = 0, completed = 0;
			sscanf(val.c_str(), "%d %d %d %d %llx", &open, &visited, &completed, &z.medal, &z.hash);
			z.open = open != 0, z.visited = visited != 0, z.completed = completed != 0;
		} else if (key.rfind("r.", 0) == 0) {
			s.zones[key.substr(2)].run = val;
		}
	}
	// Guardados de antes: todos en hoja.
	if (!s.hasStages) {
		for (int c = 0; c < 3; c++) s.stages[c][0] = s.pikmin[c];
	}
	out = s;
	return true;
}

void writeSlot(const std::string& path, const Slot& s)
{
	if (sTest) return;
	std::error_code ec;
	fs::create_directories(fs::path(path).parent_path(), ec);
	std::ofstream f(path, std::ios::trunc);
	if (!f) {
		fprintf(stderr, "[campaign] no se puede guardar en %s\n", path.c_str());
		return;
	}
	f << "players=" << s.players << "\n";
	f << "day=" << s.day << "\n";
	f << "zone=" << s.zone << "\n";
	f << "onions=" << int(s.onions[0]) << " " << int(s.onions[1]) << " " << int(s.onions[2]) << "\n";
	f << "pikmin=" << s.pikmin[0] << " " << s.pikmin[1] << " " << s.pikmin[2] << "\n";
	f << "pikminst=";
	for (int c = 0; c < 3; c++) f << s.stages[c][0] << " " << s.stages[c][1] << " " << s.stages[c][2] << (c < 2 ? " " : "\n");
	f << "started=" << int(s.started) << "\n";
	f << "parts=" << join(s.parts) << "\n";
	f << "ending=" << s.ending << "\n";
	f << "diary=" << s.diary << "\n";
	f << "mods=" << s.mods.tekiHealth << " " << s.mods.extraDays << " 0 " << s.mods.carry << "\n";
	for (const auto& kv : s.zones) {
		char line[96];
		snprintf(line, sizeof(line), "%d %d %d %d %016llx", int(kv.second.open), int(kv.second.visited), int(kv.second.completed), kv.second.medal,
		         kv.second.hash);
		f << "z." << kv.first << "=" << line << "\n";
		if (!kv.second.run.empty()) f << "r." << kv.first << "=" << kv.second.run << "\n";
	}
}

nectar::CampaignProgress progressOf(const Slot& s)
{
	nectar::CampaignProgress p;
	p.day = s.day;
	for (int c = 0; c < 3; c++) {
		p.onions[c] = s.onions[c];
		p.pikmin[c] = s.pikmin[c];
	}
	p.parts = s.parts;
	for (const auto& kv : s.zones) {
		if (kv.second.completed) p.completed.push_back(kv.first);
		if (kv.second.visited) p.visited.push_back(kv.first);
		if (kv.second.medal >= 0) p.medals[kv.first] = kv.second.medal;
	}
	return p;
}

/// Marca abiertas las zonas que ya cumplen sus reglas; devuelve las nuevas.
std::vector<int> openZones(const PcCampaignPack& pack, Slot& s)
{
	std::vector<int> fresh;
	const nectar::CampaignProgress p = progressOf(s);
	for (size_t z = 0; z < pack.doc.zones.size(); z++) {
		ZoneState& zs = s.zones[pack.doc.zones[z].id];
		if (!zs.open && nectar::zoneOpen(pack.doc, int(z), p)) {
			zs.open = true;
			fresh.push_back(int(z));
		}
	}
	return fresh;
}

const char* colorName(int c, bool es)
{
	static const char* const kEs[] = { "azul", "roja", "amarilla" }, *const kEn[] = { "blue", "red", "yellow" };
	return c >= 0 && c < 3 ? (es ? kEs[c] : kEn[c]) : "";
}

std::string zoneName(const PcCampaignPack& pack, const std::string& id)
{
	const int z = pack.doc.find(id);
	return z >= 0 ? pack.doc.zones[size_t(z)].name : id;
}

std::string ruleText(const PcCampaignPack& pack, const nectar::UnlockRule& r, bool es)
{
	char buf[160];
	if (r.type == "parts") snprintf(buf, sizeof(buf), es ? "Consigue %d piezas de la nave" : "Collect %d ship parts", r.count);
	else if (r.type == "part") snprintf(buf, sizeof(buf), es ? "Consigue la pieza %s" : "Collect the part %s", r.id.c_str());
	else if (r.type == "pikmin") {
		if (r.color >= 0) {
			static const char* const kEs[] = { "azules", "rojos", "amarillos" }, *const kEn[] = { "blue", "red", "yellow" };
			snprintf(buf, sizeof(buf), es ? "Ten %d Pikmin %s" : "Have %d %s Pikmin", r.count, es ? kEs[r.color] : kEn[r.color]);
		} else {
			snprintf(buf, sizeof(buf), es ? "Ten %d Pikmin" : "Have %d Pikmin", r.count);
		}
	} else if (r.type == "onion") snprintf(buf, sizeof(buf), es ? "Encuentra la cebolla %s" : "Find the %s Onion", colorName(r.color, es));
	else if (r.type == "zone") snprintf(buf, sizeof(buf), es ? "Completa %s" : "Complete %s", zoneName(pack, r.id).c_str());
	else if (r.type == "visited") snprintf(buf, sizeof(buf), es ? "Visita %s" : "Visit %s", zoneName(pack, r.id).c_str());
	else if (r.type == "day") snprintf(buf, sizeof(buf), es ? "Desde el día %d" : "From day %d", r.count);
	else if (r.type == "medal")
		snprintf(buf, sizeof(buf), es ? "Medalla de %s en %s" : "%s medal in %s", pc_mission_medal_name(std::clamp(r.count, 0, 2), es),
		         zoneName(pack, r.id).c_str());
	else snprintf(buf, sizeof(buf), "%s", r.type.c_str());
	return buf;
}

// Lee un paquete (su campaign.json y los niveles de sus zonas) y lo comprueba.
// false si no hay campaign.json.
bool loadPack(const fs::path& dir, PcCampaignPack& pack)
{
	std::error_code ec;
	pack     = PcCampaignPack();
	pack.id  = dir.filename().string();
	pack.dir = dir.string();
	std::string text;
	if (!readText((dir / "campaign.json").string(), text)) return false;
	if (fs::exists(dir / "thumb.png", ec)) pack.thumb = (dir / "thumb.png").string();
	if (!nectar::parseCampaignDoc(text, pack.doc, pack.error)) {
		pack.doc.name = pack.id;
		return true;
	}
	// Los niveles de sus zonas.
	std::vector<std::unique_ptr<nectar::MapData>> maps;
	std::vector<const nectar::MapData*> ptrs;
	std::string firstError;
	for (const nectar::CampaignZone& z : pack.doc.zones) {
		PcCampaignZoneInfo info;
		const fs::path zdir = dir / z.folder();
		info.file           = (zdir / "map.json").string();
		if (fs::exists(zdir / "thumb.png", ec)) info.thumb = (zdir / "thumb.png").string();
		auto map = std::make_unique<nectar::MapData>();
		std::string source, err;
		if (pc_map_catalog_load(info.file, *map, err, &source)) {
			info.hash    = fnv1a(source);
			info.hasGoal = map->story.objective != nectar::MAPGOAL_Survive;
			for (const nectar::MapShipPart& part : map->story.parts) info.parts.push_back(partName(part.id));
			ptrs.push_back(map.get());
			maps.push_back(std::move(map));
		} else {
			ptrs.push_back(nullptr);
			if (firstError.empty()) firstError = z.id + ": " + err;
		}
		pack.zones.push_back(info);
	}
	pack.partTotal = nectar::campaignPartTotal(ptrs);
	if (!firstError.empty()) pack.error = firstError;
	else nectar::validateCampaign(pack.doc, ptrs, pack.coop, pack.error);
	if (!pack.error.empty()) fprintf(stderr, "[campaign] %s: %s\n", pack.id.c_str(), pack.error.c_str());
	return true;
}

void scan()
{
	sPacks.clear();
	std::error_code ec;
	for (fs::directory_iterator it(kPacks, ec), end; !ec && it != end; it.increment(ec)) {
		if (!it->is_directory(ec)) continue;
		PcCampaignPack pack;
		if (loadPack(it->path(), pack)) sPacks.push_back(std::move(pack));
	}
	std::sort(sPacks.begin(), sPacks.end(), [](const PcCampaignPack& a, const PcCampaignPack& b) { return a.id < b.id; });
}

int currentZone()
{
	const int z = sPack.doc.find(sSlot.zone);
	return z >= 0 ? z : sPack.doc.startZone();
}

} // namespace

const std::vector<PcCampaignPack>& pc_campaign_packs(bool rescan)
{
	if (!sScanned || rescan) {
		scan();
		sScanned = true;
	}
	return sPacks;
}

PcCampaignSlotInfo pc_campaign_slot_info(const PcCampaignPack& pack, int slot)
{
	PcCampaignSlotInfo info;
	Slot s;
	if (!readSlot(slotPath(pack.id, slot), s)) return info;
	info.used    = true;
	info.players = s.players;
	info.day     = s.day;
	info.parts   = int(s.parts.size());
	info.pikmin  = s.pikmin[0] + s.pikmin[1] + s.pikmin[2];
	info.ending  = s.ending;
	for (const auto& kv : s.zones) {
		info.zonesOpen += kv.second.open ? 1 : 0;
		info.zonesDone += kv.second.completed ? 1 : 0;
	}
	return info;
}

void pc_campaign_slot_delete(const PcCampaignPack& pack, int slot)
{
	std::error_code ec;
	fs::remove(slotPath(pack.id, slot), ec);
}

bool pc_campaign_slot_copy(const PcCampaignPack& pack, int from, int to)
{
	Slot s;
	if (from == to || !readSlot(slotPath(pack.id, from), s)) return false;
	writeSlot(slotPath(pack.id, to), s);
	return true;
}

bool pc_campaign_open(const PcCampaignPack& pack, int slot, int newPlayers, const PcMissionMods& mods)
{
	if (!pack.error.empty() || pack.doc.zones.empty()) return false;
	Slot s;
	if (newPlayers) {
		s.players = newPlayers == 2 && pack.coop ? 2 : 1;
		s.zone    = pack.doc.zones[size_t(pack.doc.startZone())].id;
		for (int c = 0; c < 3; c++) s.onions[c] = pack.doc.start.onions[c];
		s.mods          = mods;
		s.mods.noLosses = false;
	} else if (!readSlot(slotPath(pack.id, slot), s)) {
		return false;
	}
	// Zonas cuyo nivel ha cambiado desde que se jugaron: lo hecho en ellas no
	// vale (lo demás de la partida se queda).
	for (size_t z = 0; z < pack.doc.zones.size(); z++) {
		auto it = s.zones.find(pack.doc.zones[z].id);
		if (it != s.zones.end() && it->second.hash && it->second.hash != pack.zones[z].hash) {
			fprintf(stderr, "[campaign] %s: la zona %s ha cambiado; empieza de cero\n", pack.id.c_str(), it->first.c_str());
			it->second.run.clear();
			it->second.hash = 0;
		}
	}
	openZones(pack, s);
	sTest      = false;
	sPack      = pack;
	sSlotIndex = slot;
	sSlot      = s;
	sProgress  = progressOf(sSlot);
	sOpen      = true;
	sNew.clear();
	if (newPlayers) writeSlot(slotPath(pack.id, slot), sSlot);
	fprintf(stderr, "[campaign] %s, ranura %d: día %d, %zu piezas\n", pack.id.c_str(), slot + 1, sSlot.day, sSlot.parts.size());
	return true;
}

bool pc_campaign_active(void) { return sOpen; }
void pc_campaign_close(void) { sOpen = false; }
const PcCampaignPack* pc_campaign_pack(void) { return sOpen ? &sPack : nullptr; }
int pc_campaign_slot(void) { return sSlotIndex; }
int pc_campaign_players(void) { return sSlot.players; }
int pc_campaign_day(void) { return sSlot.day; }
int pc_campaign_current_zone(void) { return sOpen ? currentZone() : -1; }
int pc_campaign_ending(void) { return sSlot.ending; }
int pc_campaign_day_limit(void) { return sPack.doc.ending.dayLimit > 0 ? sPack.doc.ending.dayLimit + sSlot.mods.extraDays : 0; }
const nectar::CampaignProgress& pc_campaign_progress(void) { return sProgress; }

static const ZoneState* zoneState(int zone)
{
	if (!sOpen || zone < 0 || zone >= int(sPack.doc.zones.size())) return nullptr;
	auto it = sSlot.zones.find(sPack.doc.zones[size_t(zone)].id);
	return it == sSlot.zones.end() ? nullptr : &it->second;
}

bool pc_campaign_zone_open(int zone)
{
	const ZoneState* z = zoneState(zone);
	return z && z->open;
}

bool pc_campaign_zone_completed(int zone)
{
	const ZoneState* z = zoneState(zone);
	return z && z->completed;
}

bool pc_campaign_zone_visited(int zone)
{
	const ZoneState* z = zoneState(zone);
	return z && z->visited;
}

int pc_campaign_zone_medal(int zone)
{
	const ZoneState* z = zoneState(zone);
	return z ? z->medal : -1;
}

int pc_campaign_zone_parts_found(int zone)
{
	if (!sOpen || zone < 0 || zone >= int(sPack.zones.size())) return 0;
	int n = 0;
	for (const std::string& id : sPack.zones[size_t(zone)].parts) n += std::count(sSlot.parts.begin(), sSlot.parts.end(), id) ? 1 : 0;
	return n;
}

std::string pc_campaign_lock_text(int zone, bool es)
{
	if (!sOpen || zone < 0 || zone >= int(sPack.doc.zones.size()) || pc_campaign_zone_open(zone)) return "";
	const nectar::UnlockBlock& b = sPack.doc.zones[size_t(zone)].unlock;
	for (const nectar::UnlockRule& r : b.all) {
		nectar::UnlockBlock one;
		one.all.push_back(r);
		if (!nectar::unlockMet(one, sProgress)) return ruleText(sPack, r, es);
	}
	if (!b.any.empty()) return (es ? "Una de: " : "One of: ") + ruleText(sPack, b.any[0], es) + (b.any.size() > 1 ? "..." : "");
	return "";
}

bool pc_campaign_enter(int zone, std::string& error)
{
	if (!sOpen || zone < 0 || zone >= int(sPack.doc.zones.size())) {
		error = "no hay campaña";
		return false;
	}
	if (!pc_campaign_zone_open(zone)) {
		error = pc_campaign_lock_text(zone, false);
		return false;
	}
	const nectar::CampaignZone& z = sPack.doc.zones[size_t(zone)];
	const ZoneState* zs           = zoneState(zone);
	sSlot.zone                    = z.id;
	pc_mission_select_zone(sPack.zones[size_t(zone)].file, sSlot.players, zs ? zs->run : std::string());
	pc_mission_set_mods(sSlot.mods);
	if (!pc_mission_map()) {
		error = pc_mission_error();
		return false;
	}
	if (const char* why = pc_vs_arena_prepare()) {
		error = why;
		return false;
	}
	pc_mission_start(false);
	return true;
}

bool pc_campaign_has_onion(int color) { return sOpen && color >= 0 && color < 3 && sSlot.onions[color]; }
int pc_campaign_onion_pikmin(int color) { return pc_campaign_has_onion(color) ? sSlot.pikmin[color] : 0; }

void pc_campaign_onion_stages(int color, int out[3])
{
	for (int k = 0; k < 3; k++) out[k] = pc_campaign_has_onion(color) ? sSlot.stages[color][k] : 0;
}
int pc_campaign_start_pikmin(int color) { return sOpen && !sSlot.started && color >= 0 && color < 3 ? sPack.doc.start.pikmin[color] : 0; }

int pc_campaign_day_end(const PcCampaignDayEnd& e)
{
	if (!sOpen) return -1;
	const int zone = currentZone();
	ZoneState& zs  = sSlot.zones[sPack.doc.zones[size_t(zone)].id];
	zs.run         = e.run;
	zs.hash        = sPack.zones[size_t(zone)].hash;
	zs.visited     = true;
	if (e.completed) zs.completed = true;
	zs.medal = std::max(zs.medal, e.medal);
	// Las cebollas encontradas en la zona ya son de la partida.
	for (int c = 0; c < 3; c++) {
		if (e.onions[c]) sSlot.onions[c] = true;
		sSlot.pikmin[c] = sSlot.onions[c] ? e.onionPikmin[c] : 0;
		for (int k = 0; k < 3; k++) sSlot.stages[c][k] = sSlot.onions[c] ? e.onionStages[c][k] : 0;
	}
	for (const std::string& id : e.parts) {
		if (std::find(sSlot.parts.begin(), sSlot.parts.end(), id) == sSlot.parts.end()) sSlot.parts.push_back(id);
	}
	sSlot.started = true;
	// El diario de esta noche: la primera entrada de este día (o de cualquiera)
	// y de esta zona (o de cualquiera) que no se haya leído.
	sDiaryText.clear();
	sSlot.diary.resize(sPack.doc.diary.size(), '0');
	for (size_t k = 0; k < sPack.doc.diary.size(); k++) {
		const nectar::CampaignDiary& d = sPack.doc.diary[k];
		if (sSlot.diary[k] == '1' || (d.day && d.day != sSlot.day) || (!d.zone.empty() && d.zone != sSlot.zone)) continue;
		sSlot.diary[k] = '1';
		sDiaryText     = pc_campaign_text(d.text, d.textEn);
		break;
	}
	sSlot.day++;
	sNew      = openZones(sPack, sSlot);
	sProgress = progressOf(sSlot);
	nectar::CampaignDoc withLimit = sPack.doc; // con los días extra
	withLimit.ending.dayLimit     = pc_campaign_day_limit();
	sSlot.ending                  = nectar::campaignEnding(withLimit, sProgress, sPack.partTotal);
	writeSlot(slotPath(sPack.id, sSlotIndex), sSlot);
	fprintf(stderr, "[campaign] fin del día %d en %s: %zu piezas, %d Pikmin, %zu zonas nuevas%s\n", sSlot.day - 1, zs.run.empty() ? "?" : sSlot.zone.c_str(),
	        sSlot.parts.size(), sSlot.pikmin[0] + sSlot.pikmin[1] + sSlot.pikmin[2], sNew.size(),
	        sSlot.ending == 1 ? " - CAMPAÑA COMPLETADA" : sSlot.ending == 0 ? " - SIN DÍAS" : "");
	return sSlot.ending;
}

const std::vector<int>& pc_campaign_new_zones(void) { return sNew; }
const std::string& pc_campaign_diary(void) { return sDiaryText; }
int pc_campaign_parts_count(void) { return sOpen ? int(sSlot.parts.size()) : 0; }

const std::string& pc_campaign_text(const std::string& es, const std::string& en)
{
	return pc_settings_get_language() != 3 && !en.empty() ? en : es; // 3 = OS_LANG_SPANISH
}
void pc_campaign_clear_new_zones(void) { sNew.clear(); }

// ── Prueba desde el editor ──────────────────────────────────────────────────

void pc_campaign_direct_request(const std::string& dir, const std::string& zone, const std::string& state)
{
	sDirect      = 1;
	sDirectState = state;
	sDirectDir  = dir;
	sDirectZone = zone;
}

bool pc_campaign_direct_take_title(void)
{
	if (sDirect != 1) return false;
	sDirect = 2;
	return true;
}

bool pc_campaign_direct_take_setup(std::string& error)
{
	if (sDirect != 2) return false;
	sDirect = 3;
	PcCampaignPack pack;
	if (!loadPack(fs::path(sDirectDir), pack)) {
		error = "no hay campaign.json en " + sDirectDir;
		return true;
	}
	if (!pack.error.empty()) {
		error = pack.error;
		return true;
	}
	// Partida nueva, sin ranura, con la zona pedida abierta.
	Slot s;
	s.players = 1;
	for (int c = 0; c < 3; c++) s.onions[c] = pack.doc.start.onions[c];
	int zone = pack.doc.find(sDirectZone);
	if (zone < 0) zone = pack.doc.startZone();
	s.zone = pack.doc.zones[size_t(zone)].id;
	// Estado de prueba pedido por el editor.
	{
		std::istringstream items(sDirectState);
		std::string item;
		while (std::getline(items, item, ';')) {
			const size_t eq = item.find('=');
			if (eq == std::string::npos) continue;
			const std::string key = item.substr(0, eq), val = item.substr(eq + 1);
			if (key == "day") s.day = std::max(1, atoi(val.c_str()));
			else if (key == "pikmin") {
				sscanf(val.c_str(), "%d %d %d", &s.pikmin[0], &s.pikmin[1], &s.pikmin[2]);
				for (int c = 0; c < 3; c++) s.stages[c][0] = s.pikmin[c];
			} else if (key == "onions") {
				int o[3] = { 0, 0, 0 };
				sscanf(val.c_str(), "%d %d %d", &o[0], &o[1], &o[2]);
				for (int c = 0; c < 3; c++) s.onions[c] = o[c] != 0;
			} else if (key == "parts") {
				int n = atoi(val.c_str());
				for (const PcCampaignZoneInfo& zi : pack.zones) {
					for (const std::string& id : zi.parts) {
						if (n-- > 0) s.parts.push_back(id);
					}
				}
			}
		}
		if (!sDirectState.empty()) s.started = true; // con estado propio, sin los Pikmin del principio
	}
	openZones(pack, s);
	s.zones[s.zone].open = true;
	sPack      = pack;
	sSlotIndex = -1;
	sSlot      = s;
	sProgress  = progressOf(sSlot);
	sOpen      = true;
	sTest      = true;
	sNew.clear();
	sDirectEntered = pc_campaign_enter(zone, error);
	return true;
}

bool pc_campaign_take_direct_zone(void)
{
	const bool r   = sDirectEntered;
	sDirectEntered = false;
	return r;
}
