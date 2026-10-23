#include "pc_mission.h"
#include "pc_campaign.h"
#include "pc_map_catalog.h"

#include "settings/pc_settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

bool sPending = false, sActive = false;

std::string sSelected;
std::string sFile; // --mission-map o una zona de campaña: el map.json por ruta
bool sZone   = false; // zona de campaña (sin guardado propio)
std::string sZoneRun; // lo hecho en la zona en visitas anteriores
int sPlayers = 1;
int sDirect  = 0; // --mission-map: 1 pedido, 2 en el título, 3 hecho
int sSerial  = 0;
nectar::MapData sMap;
int sState = 0; // 0 sin leer, 1 listo, -1 error
std::string sError;
unsigned long long sMapHash = 0; // del map.json: un guardado de otra versión no vale

// La misión entera (pasa de un día a otro y es lo que se guarda).
struct Run {
	int day = 1;
	std::vector<char> parts, pellets, objects; // 1 = ya no vuelve
	std::vector<char> triggers;                // 1 = ya no vuelve a saltar
	std::vector<int> tcount;                   // veces que ha saltado cada disparador
	std::vector<int> dead;                     // día en que se derrotó a cada enemigo (0 = no)
	std::vector<std::string> ostate;           // estado de cada construcción (hex)
	std::string gcache;                        // caché de generadores del juego (hex)
	int onion[3]      = { 0, 0, 0 };
	int onionStage[3][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } }; // por color y fase
	bool hasStages    = false;
	bool found[3]     = { false, false, false }; // cebollas enterradas ya encontradas
	bool hasMods      = false;                   // el guardado trae modificadores
	PcMissionMods mods;
};
PcMissionMods sMods;
Run sRun;

// El día en curso.
int sBossesAlive = 0, sPikmin = 0;
bool sHadPikmin = false;
int sResult     = -1;
int sLoseReason = 0; // 0 tiempo, 1 sin Pikmin
int sLostAtNight = 0;
f32 sOverFor    = 0.0f;
bool sExitTaken = false;
constexpr f32 kEndScreenDelay = 1.8f; // el aviso grande, y luego los resultados
int sExitChoice   = 0;
int sStartPikmin  = -1; // los del principio del día (en el primer fotograma)
int sBorn = 0, sKills = 0;
f32 sElapsed      = 0.0f;
int sNight[3]     = { 0, 0, 0 };
int sMedal       = -1;    // al cumplirla
bool sNewRecord  = false;
bool sZoneDone   = false; // zona de campaña: objetivo cumplido hoy (o antes)
int sZoneEnding  = -1;    // zona de campaña: final de la campaña al acabar el día
// Disparadores: lo de hoy.
struct ActionRef {
	int trigger, action;
};
std::vector<ActionRef> sActionQueue;  // lo que hace el juego (aparecer, romper)
std::vector<f32> sFiredAt;            // segundo del día en que saltó cada uno (-1 = hoy no)
std::vector<char> sObjDone;           // construcciones terminadas o rotas (lista desplegada)
bool sEndDayRequest = false;
std::string sTextbox;
f32 sCountdown     = -1.0f;
f32 sCameraHold    = 0.0f;
int sCountdownThen = 0;
std::string sMessage;
f32 sMessageFor = 0.0f;

unsigned long long fnv1a(const std::string& s)
{
	unsigned long long h = 1469598103934665603ull;
	for (unsigned char c : s) {
		h ^= c;
		h *= 1099511628211ull;
	}
	return h;
}

int countDone(const std::vector<char>& v)
{
	int n = 0;
	for (char c : v) n += c ? 1 : 0;
	return n;
}

bool doneAt(const std::vector<char>& v, int i) { return i >= 0 && i < int(v.size()) && v[i]; }

int bossTotal()
{
	int n = 0;
	for (const nectar::MapEnemy& e : sMap.enemies) n += e.boss >= 0 ? 1 : 0;
	return n;
}

int need()
{
	const nectar::MapStory& s = sMap.story;
	switch (s.objective) {
	case nectar::MAPGOAL_Parts: return s.target ? s.target : int(s.parts.size());
	case nectar::MAPGOAL_Pellets: return s.target ? s.target : int(sMap.pellets.size());
	case nectar::MAPGOAL_Pikmin: return s.target;
	case nectar::MAPGOAL_Boss: return bossTotal();
	case nectar::MAPGOAL_Waves: return s.target;
	default: return 0;
	}
}

int sWaveAlive = 0; // enemigos de oleadas aún vivos

int wavesDone()
{
	int n = 0;
	for (size_t i = 0; i < sMap.story.triggers.size() && i < sRun.tcount.size(); i++) {
		if (sMap.story.triggers[i].wave) n += sRun.tcount[i];
	}
	return n;
}

int progress()
{
	switch (sMap.story.objective) {
	case nectar::MAPGOAL_Parts: return countDone(sRun.parts);
	case nectar::MAPGOAL_Pellets: return countDone(sRun.pellets);
	case nectar::MAPGOAL_Pikmin: return sPikmin;
	// Los de días anteriores no salen, así que no cuentan como vivos.
	case nectar::MAPGOAL_Boss: return bossTotal() - sBossesAlive;
	// Una oleada cuenta cuando no le queda nadie (la última, también).
	case nectar::MAPGOAL_Waves: return std::max(0, wavesDone() - (sWaveAlive > 0 ? 1 : 0));
	default: return 0;
	}
}

std::string partName(std::uint32_t id)
{
	std::string s;
	for (int b = 3; b >= 0; b--) s += char((id >> (8 * b)) & 0xFF);
	return s;
}

// ── Lo hecho en el mapa, en texto ───────────────────────────────────────────

std::string bits(const std::vector<char>& v)
{
	std::string s;
	for (char c : v) s += c ? '1' : '0';
	return s;
}

void unbits(const std::string& s, std::vector<char>& v)
{
	for (size_t i = 0; i < v.size() && i < s.size(); i++) v[i] = s[i] == '1';
}

void freshRun()
{
	sRun          = Run();
	sRun.parts    = std::vector<char>(sMap.story.parts.size(), 0);
	sRun.pellets  = std::vector<char>(sMap.pellets.size(), 0);
	sRun.dead     = std::vector<int>(sMap.enemies.size(), 0);
	sRun.objects  = std::vector<char>(sMap.objects.size(), 0);
	sRun.ostate   = std::vector<std::string>(sMap.objects.size());
	sRun.triggers = std::vector<char>(sMap.story.triggers.size(), 0);
	sRun.tcount   = std::vector<int>(sMap.story.triggers.size(), 0);
}

/// "clave=valor" separados por `sep` (';' en una línea, '\n' en un archivo).
std::string runText(const Run& r, char sep)
{
	char hash[24];
	snprintf(hash, sizeof(hash), "%016llx", sMapHash);
	std::ostringstream o;
	o << "map=" << hash << sep << "day=" << r.day << sep << "parts=" << bits(r.parts) << sep << "pellets=" << bits(r.pellets) << sep
	  << "objects=" << bits(r.objects) << sep << "triggers=" << bits(r.triggers) << sep
	  << "onions=" << r.onion[0] << " " << r.onion[1] << " " << r.onion[2] << sep << "onst=";
	for (int c = 0; c < 3; c++) o << r.onionStage[c][0] << " " << r.onionStage[c][1] << " " << r.onionStage[c][2] << (c < 2 ? " " : "");
	o << sep
	  << "found=" << int(r.found[0]) << " " << int(r.found[1]) << " " << int(r.found[2]) << sep << "dead=";
	for (size_t i = 0; i < r.dead.size(); i++) o << (i ? "," : "") << r.dead[i];
	o << sep << "tcount=";
	for (size_t i = 0; i < r.tcount.size(); i++) o << (i ? "," : "") << r.tcount[i];
	o << sep << "mods=" << sMods.tekiHealth << " " << sMods.extraDays << " " << int(sMods.noLosses) << " " << sMods.carry;
	o << sep << "ostate=";
	for (size_t i = 0, n = 0; i < r.ostate.size(); i++) {
		if (!r.ostate[i].empty()) o << (n++ ? "," : "") << i << ":" << r.ostate[i];
	}
	o << sep;
	if (!r.gcache.empty()) o << "gcache=" << r.gcache << sep;
	return o.str();
}

/// Lee `text` en `out` (con las listas ya del tamaño del mapa). false si es de
/// otra versión del mapa.
bool parseRun(const std::string& text, Run& out)
{
	bool sameMap = false;
	const char sep = text.find(';') != std::string::npos ? ';' : '\n';
	std::string item;
	std::istringstream in(text);
	while (std::getline(in, item, sep)) {
		const size_t eq = item.find('=');
		if (eq == std::string::npos) continue;
		const std::string key = item.substr(0, eq), val = item.substr(eq + 1);
		if (key == "map") sameMap = strtoull(val.c_str(), nullptr, 16) == sMapHash;
		else if (key == "day") out.day = atoi(val.c_str());
		else if (key == "parts") unbits(val, out.parts);
		else if (key == "pellets") unbits(val, out.pellets);
		else if (key == "bosses") {
			// Guardados de antes: solo jefes, sin día.
			for (size_t i = 0; i < out.dead.size() && i < val.size(); i++) out.dead[i] = val[i] == '1' ? 1 : 0;
		} else if (key == "dead") {
			std::istringstream list(val);
			std::string n;
			for (size_t i = 0; i < out.dead.size() && std::getline(list, n, ','); i++) out.dead[i] = atoi(n.c_str());
		} else if (key == "gcache") {
			out.gcache = val;
		} else if (key == "ostate") {
			std::istringstream list(val);
			std::string item;
			while (std::getline(list, item, ',')) {
				const size_t colon = item.find(':');
				const int i        = colon == std::string::npos ? -1 : atoi(item.c_str());
				if (i >= 0 && i < int(out.ostate.size())) out.ostate[size_t(i)] = item.substr(colon + 1);
			}
		} else if (key == "tcount") {
			std::istringstream list(val);
			std::string n;
			for (size_t i = 0; i < out.tcount.size() && std::getline(list, n, ','); i++) out.tcount[i] = atoi(n.c_str());
		} else if (key == "mods") {
			int noLosses = 0;
			out.hasMods  = sscanf(val.c_str(), "%d %d %d %d", &out.mods.tekiHealth, &out.mods.extraDays, &noLosses, &out.mods.carry) == 4;
			out.mods.noLosses = noLosses != 0;
		} else if (key == "found") {
			int f[3] = { 0, 0, 0 };
			sscanf(val.c_str(), "%d %d %d", &f[0], &f[1], &f[2]);
			for (int c = 0; c < 3; c++) out.found[c] = f[c] != 0;
		}
		else if (key == "objects") unbits(val, out.objects);
		else if (key == "triggers") unbits(val, out.triggers);
		else if (key == "onions") sscanf(val.c_str(), "%d %d %d", &out.onion[0], &out.onion[1], &out.onion[2]);
		else if (key == "onst") {
			int* v       = &out.onionStage[0][0];
			out.hasStages = sscanf(val.c_str(), "%d %d %d %d %d %d %d %d %d", v, v + 1, v + 2, v + 3, v + 4, v + 5, v + 6, v + 7, v + 8) == 9;
		}
	}
	// Guardados de antes: todos en hoja.
	if (!out.hasStages) {
		for (int c = 0; c < 3; c++) out.onionStage[c][0] = out.onion[c];
	}
	return sameMap;
}

// ── Guardado de las misiones sueltas ────────────────────────────────────────

bool canSave() { return sFile.empty() && !sZone; }

std::string saveFile()
{
	return "user/saves/maps/" + sSelected + "/" + (sPlayers == 2 ? "coop" : "solo") + ".txt";
}

bool readSave(Run& out)
{
	if (!canSave()) return false;
	std::ifstream f(saveFile());
	if (!f) return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	return parseRun(ss.str(), out) && out.day > 1 && out.day <= sMap.story.days + (out.hasMods ? out.mods.extraDays : 0);
}

void writeFile(const std::string& path, const std::string& text)
{
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
	std::ofstream f(path, std::ios::trunc);
	if (f) f << text;
	else fprintf(stderr, "[mission] no se puede guardar en %s\n", path.c_str());
}

void writeSave()
{
	if (canSave()) writeFile(saveFile(), runText(sRun, '\n'));
}

void deleteSave()
{
	if (!canSave()) return;
	std::error_code ec;
	std::filesystem::remove(saveFile(), ec);
}

// Con modificadores, los récords van aparte.
std::string recordFile(const std::string& id, int players)
{
	return "user/saves/maps/" + id + "/" + (players == 2 ? "coop" : "solo") + (sMods.any() ? "-mods" : "") + "-records.txt";
}

int readKey(const std::string& path, const char* key, int fallback)
{
	std::ifstream f(path);
	std::string line;
	const std::string prefix = std::string(key) + "=";
	while (std::getline(f, line)) {
		if (line.rfind(prefix, 0) == 0) return atoi(line.c_str() + prefix.size());
	}
	return fallback;
}

// Medalla por los Pikmin al cumplirla (-1 si el mapa no da medallas).
int medalFor(int pikmin)
{
	const int* m = sMap.story.medals;
	if (!m[0] && !m[1] && !m[2]) return -1;
	int medal = -1;
	for (int k = 0; k < 3; k++) {
		if (m[k] && pikmin >= m[k]) medal = k;
	}
	return medal;
}

void saveRecord()
{
	if (!canSave()) return;
	PcMissionRecord r = pc_mission_record(sSelected, sPlayers);
	sNewRecord        = r.wins > 0 && (sMedal > r.medal || sPikmin > r.pikmin || sRun.day < r.days);
	r.wins++;
	if (sMedal > r.medal) r.medal = sMedal;
	if (sPikmin > r.pikmin) r.pikmin = sPikmin;
	if (!r.days || sRun.day < r.days) r.days = sRun.day;
	char text[128];
	snprintf(text, sizeof(text), "wins=%d\nmedal=%d\npikmin=%d\ndays=%d\n", r.wins, r.medal, r.pikmin, r.days);
	writeFile(recordFile(sSelected, sPlayers), text);
}

void finish(int result, int loseReason = 0)
{
	if (sResult >= 0) return;
	sResult     = result;
	sLoseReason = loseReason;
	sOverFor    = 0.0f;
	sMessageFor = 0.0f;
	if (result == 1 && !sZone) {
		sMedal = medalFor(sPikmin);
		saveRecord();
	}
	fprintf(stderr, "[mission] día %d: %s\n", sRun.day,
	        result == 2 ? "fin del día" : result ? "cumplida" : (loseReason ? "fallida: sin Pikmin" : "fallida: se acabó el tiempo"));
	// Acabada (bien o mal), lo guardado ya no sirve.
	if (result != 2) deleteSave();
}

void message(const std::string& text)
{
	sMessage    = text;
	sMessageFor = 5.0f;
}

void zoneCompleted();

// Lo que pasa al acabar una cuenta atrás (o con las acciones ganar y perder).
void applyThen(int then)
{
	switch (then) {
	case nectar::MAPTHEN_Win:
		if (sZone) zoneCompleted();
		else finish(1);
		break;
	case nectar::MAPTHEN_Lose:
		// En una campaña no se pierde la partida: se acaba el día.
		if (sZone) sEndDayRequest = true;
		else finish(0, 3);
		break;
	case nectar::MAPTHEN_EndDay: sEndDayRequest = true; break;
	default: break;
	}
}

bool condMet(const nectar::MapCondition& c, const PcMissionWorld& w, int bossesAlive)
{
	switch (c.type) {
	case nectar::MAPCOND_Parts: return countDone(sRun.parts) >= c.count;
	case nectar::MAPCOND_Pellets: return countDone(sRun.pellets) >= c.count;
	case nectar::MAPCOND_Bosses: return bossTotal() - bossesAlive >= c.count;
	case nectar::MAPCOND_Enemies: return sKills >= c.count;
	case nectar::MAPCOND_Day: return pc_mission_day() >= c.count;
	case nectar::MAPCOND_Hour: return w.hour >= f32(c.count);
	case nectar::MAPCOND_Time: return sElapsed >= f32(c.count);
	case nectar::MAPCOND_After:
		return c.index >= 0 && c.index < int(sFiredAt.size()) && sFiredAt[size_t(c.index)] >= 0.0f
		       && sElapsed - sFiredAt[size_t(c.index)] >= f32(c.count);
	case nectar::MAPCOND_Area:
		for (int k = 0; k < w.captains && k < 2; k++) {
			const f32 dx = w.cx[k] - c.x, dz = w.cz[k] - c.z;
			if (dx * dx + dz * dz <= c.radius * c.radius) return true;
		}
		return false;
	case nectar::MAPCOND_Object: return c.index >= 0 && c.index < int(sObjDone.size()) && sObjDone[size_t(c.index)];
	case nectar::MAPCOND_Pikmin: return (c.color >= 0 && c.color < 3 ? w.pikmin[c.color] : w.pikmin[0] + w.pikmin[1] + w.pikmin[2]) >= c.count;
	default: return false;
	}
}

void runTriggers(const PcMissionWorld& w, int bossesAlive)
{
	const std::vector<nectar::MapTrigger>& triggers = sMap.story.triggers;
	for (size_t i = 0; i < triggers.size() && i < sRun.triggers.size() && sResult < 0; i++) {
		const nectar::MapTrigger& t = triggers[i];
		if (sRun.triggers[i]) continue;
		if (t.repeat && sFiredAt[i] >= 0.0f && sElapsed - sFiredAt[i] < t.cooldown) continue;
		bool met = !t.any;
		for (const nectar::MapCondition& c : t.conditions) {
			const bool ok = condMet(c, w, bossesAlive);
			met           = t.any ? (met || ok) : (met && ok);
		}
		if (!met) continue;
		sRun.tcount[i]++;
		sFiredAt[i] = sElapsed;
		if (!t.repeat || (t.times > 0 && sRun.tcount[i] >= t.times)) sRun.triggers[i] = 1;
		for (size_t a = 0; a < t.actions.size(); a++) {
			const nectar::MapAction& act = t.actions[a];
			switch (act.type) {
			case nectar::MAPACT_Message: message(act.text); break;
			case nectar::MAPACT_Textbox: sTextbox = act.text; break;
			case nectar::MAPACT_Win: applyThen(nectar::MAPTHEN_Win); break;
			case nectar::MAPACT_Lose: applyThen(nectar::MAPTHEN_Lose); break;
			case nectar::MAPACT_EndDay: sEndDayRequest = true; break;
			case nectar::MAPACT_Countdown:
				sCountdown     = f32(act.seconds);
				sCountdownThen = act.then;
				if (!act.text.empty()) message(act.text);
				break;
			default: sActionQueue.push_back({ int(i), int(a) }); break; // aparecer, romper: el juego
			}
		}
	}
}

} // namespace

namespace {
void zoneCompleted()
{
	if (sZoneDone) return;
	sZoneDone = true;
	sMedal    = medalFor(sPikmin);
	message(pc_settings_get_language() == 3 ? "¡ZONA COMPLETADA!" : "ZONE COMPLETE!"); // OS_LANG_SPANISH
}
} // namespace

void pc_mission_set_pending(bool on) { sPending = on; }
bool pc_mission_pending(void) { return sPending; }
void pc_mission_begin_run(void) { sActive = sPending; }
bool pc_mission_active(void) { return sActive; }
void pc_mission_end_run(void) { sActive = false; }

void pc_mission_select(const std::string& id, int players)
{
	players = players == 2 ? 2 : 1;
	if (id == sSelected && sFile.empty() && !sZone && players == sPlayers && sState > 0) return;
	sFile.clear();
	sZone     = false;
	sSelected = id;
	sPlayers  = players;
	sState    = 0;
	sSerial++;
}

void pc_mission_select_file(const std::string& path, int players)
{
	sSelected.clear();
	sZone    = false;
	sFile    = path;
	sPlayers = players == 2 ? 2 : 1;
	sState   = 0;
	sSerial++;
}

void pc_mission_select_zone(const std::string& path, int players, const std::string& runText)
{
	pc_mission_select_file(path, players);
	sZone    = true;
	sZoneRun = runText;
}

bool pc_mission_zone_mode(void) { return sZone; }

void pc_mission_direct_request(void) { sDirect = 1; }

bool pc_mission_direct_take_title(void)
{
	if (sDirect != 1) return false;
	sDirect = 2;
	return true;
}

bool pc_mission_direct_take_setup(void)
{
	if (sDirect != 2) return false;
	sDirect = 3;
	return true;
}

const std::string& pc_mission_selected(void) { return sSelected; }
int pc_mission_players(void) { return sPlayers; }
int pc_mission_serial(void) { return sSerial; }
const std::string& pc_mission_error(void) { return sError; }
unsigned long long pc_mission_map_hash(void) { return sMapHash; }
std::string pc_mission_run_text(void) { return runText(sRun, ';'); }

const nectar::MapData* pc_mission_map(void)
{
	if (sState) return sState > 0 ? &sMap : nullptr;
	sError.clear();
	std::string text;
	const std::string path = !sFile.empty() ? sFile : sSelected.empty() ? std::string() : pc_map_catalog_file(sSelected);
	bool ok = !path.empty() && pc_map_catalog_load(path, sMap, sError, &text)
	          && nectar::validateMapMode(sMap, sPlayers == 2 ? nectar::MAPMODE_COOP : nectar::MAPMODE_SOLO, sError);
	if (path.empty()) sError = "no hay mapa elegido";
	if (!ok) fprintf(stderr, "[mission] %s: %s\n", path.c_str(), sError.c_str());
	sMapHash = fnv1a(text);
	sState   = ok ? 1 : -1;
	if (ok) freshRun();
	return ok ? &sMap : nullptr;
}

int pc_mission_saved_day(void)
{
	if (!pc_mission_map()) return 0;
	Run r = sRun;
	r.day = 0;
	return readSave(r) ? r.day : 0;
}

void pc_mission_start(bool continueSaved)
{
	if (!pc_mission_map()) return;
	freshRun();
	if (sZone) {
		// Lo hecho en la zona en otras visitas (si el mapa no ha cambiado).
		Run r = sRun;
		if (!sZoneRun.empty() && parseRun(sZoneRun, r)) sRun = r;
		sRun.day = 1;
	} else if (continueSaved) {
		Run r = sRun;
		if (readSave(r)) {
			sRun = r;
			if (r.hasMods) sMods = r.mods; // los de cuando se empezó
		}
	}
	fprintf(stderr, "[mission] %s: día %d de %d%s\n", sFile.empty() ? sSelected.c_str() : sFile.c_str(), sRun.day, sMap.story.days,
	        sZone ? " (zona de campaña)" : "");
}

int pc_mission_day(void) { return sZone ? pc_campaign_day() : sRun.day; }
int pc_mission_days(void) { return sZone ? 0 : sMap.story.days + sMods.extraDays; }

void pc_mission_set_mods(const PcMissionMods& mods) { sMods = mods; }
const PcMissionMods& pc_mission_mods(void) { return sMods; }

int pc_mission_music(void)
{
	if (sZone && pc_campaign_active()) {
		const PcCampaignPack* pack = pc_campaign_pack();
		const int z                = pc_campaign_current_zone();
		if (pack && z >= 0) {
			const std::string& m = pack->doc.zones[size_t(z)].music;
			for (size_t k = 0; k < nectar::musicNames().size(); k++) {
				if (m == nectar::musicNames()[k]) return int(k);
			}
		}
	}
	return sState > 0 ? sMap.music : -1;
}

f32 pc_mission_start_hour(f32 fallback) { return sState > 0 && sMap.startHour > 0.0f ? sMap.startHour : fallback; }
f32 pc_mission_end_hour(f32 fallback) { return sState > 0 && sMap.endHour > 0.0f ? sMap.endHour : fallback; }

void pc_mission_begin_day(void)
{
	sBossesAlive = sPikmin = 0;
	sHadPikmin   = false;
	sResult      = -1;
	sLoseReason  = 0;
	sLostAtNight = 0;
	sOverFor     = 0.0f;
	sExitTaken   = false;
	sExitChoice  = 0;
	sStartPikmin = -1;
	sBorn = sKills = 0;
	sElapsed     = 0.0f;
	for (int& n : sNight) n = 0;
	sMedal       = -1;
	sNewRecord   = false;
	sZoneEnding  = -1;
	sActionQueue.clear();
	sFiredAt.assign(sMap.story.triggers.size(), -1.0f);
	sObjDone.assign(sMap.objects.size(), 0);
	for (size_t i = 0; i < sObjDone.size() && i < sRun.objects.size(); i++) sObjDone[i] = sRun.objects[i]; // rotas otro día
	sEndDayRequest = false;
	sCameraHold    = 0.0f;
	sTextbox.clear();
	// Límite de tiempo del día: una cuenta atrás desde el principio.
	sCountdown     = sMap.story.timeLimit > 0 ? f32(sMap.story.timeLimit) : -1.0f;
	sCountdownThen = sMap.story.timeLimitEnd;
	sMessage.clear();
	sMessageFor = 0.0f;
	// Zona: completada en otra visita (no se vuelve a anunciar).
	sZoneDone = false;
	if (sZone && pc_campaign_active()) {
		const PcCampaignPack* pack = pc_campaign_pack();
		for (size_t z = 0; pack && z < pack->zones.size(); z++) {
			if (pack->zones[z].file == sFile) sZoneDone = pc_campaign_zone_completed(int(z));
		}
	}
}

bool pc_mission_part_done(int index) { return doneAt(sRun.parts, index); }
bool pc_mission_pellet_done(int index) { return doneAt(sRun.pellets, index); }
bool pc_mission_object_done(int objectIndex) { return doneAt(sRun.objects, objectIndex); }

bool pc_mission_enemy_spawns(int i)
{
	if (i < 0 || i >= int(sMap.enemies.size()) || i >= int(sRun.dead.size())) return true;
	const nectar::MapEnemy& e = sMap.enemies[size_t(i)];
	const int day             = pc_mission_day();
	if ((e.fromDay && day < e.fromDay) || (e.untilDay && day > e.untilDay)) return false;
	const int dead = sRun.dead[size_t(i)];
	if (!dead) return true;
	const int rule = e.respawn == nectar::MAPRESPAWN_Auto ? (e.boss >= 0 ? nectar::MAPRESPAWN_Never : nectar::MAPRESPAWN_Always) : e.respawn;
	const bool back = rule == nectar::MAPRESPAWN_Always || (rule > 0 && day - dead >= rule);
	if (back) sRun.dead[size_t(i)] = 0; // vuelve: se le puede derrotar otra vez
	return back;
}

const std::string& pc_mission_object_state(int i)
{
	static const std::string sNone;
	return i >= 0 && i < int(sRun.ostate.size()) ? sRun.ostate[size_t(i)] : sNone;
}

std::vector<PcMissionOnion> pc_mission_onions(void)
{
	std::vector<PcMissionOnion> out;
	const nectar::MapStory& st = sMap.story;
	if (!sZone) {
		for (const nectar::MapStoryOnion& o : st.onions) {
			PcMissionOnion m { o.color, o.x, o.z, { 0, 0, 0 }, o.buried && !sRun.found[o.color] };
			for (int k = 0; k < 3 && sRun.day > 1; k++) m.stages[k] = sRun.onionStage[o.color][k];
			out.push_back(m);
		}
		return out;
	}
	// Zona: las de la campaña donde diga el mapa (o junto a la nave), y las
	// del mapa que aún no se tienen, que se encuentran aquí.
	for (int c = 0; c < 3; c++) {
		const nectar::MapStoryOnion* spot = nullptr;
		for (const nectar::MapStoryOnion& o : st.onions) {
			if (o.color == c) spot = &o;
		}
		if (pc_campaign_has_onion(c)) {
			PcMissionOnion m { c, 0.0f, 0.0f, { 0, 0, 0 }, false };
			pc_campaign_onion_stages(c, m.stages);
			if (spot) {
				m.x = spot->x;
				m.z = spot->z;
			} else {
				// Detrás de la nave, una al lado de otra.
				const f32 back = st.ship.face + 3.14159265f, side = back + 1.5707963f;
				m.x = st.ship.x + sinf(back) * 180.0f + sinf(side) * (c - 1) * 130.0f;
				m.z = st.ship.z + cosf(back) * 180.0f + cosf(side) * (c - 1) * 130.0f;
			}
			out.push_back(m);
		} else if (spot) {
			// La que aún no se tiene: enterrada hasta que un capitán la encuentre.
			out.push_back({ c, spot->x, spot->z, { 0, 0, 0 }, !sRun.found[c] });
		}
	}
	return out;
}

int pc_mission_ship_level(void)
{
	// Como en la historia, de 0 a 5 según la parte recogida del total.
	int have = 0, total = 0;
	if (sZone && pc_campaign_active()) {
		have  = pc_campaign_parts_count();
		total = pc_campaign_pack()->partTotal;
	} else {
		have  = countDone(sRun.parts);
		total = int(sMap.story.parts.size());
	}
	return total > 0 ? std::clamp(have * 5 / total, 0, 5) : 0;
}

int pc_mission_start_pikmin(int color)
{
	if (color < 0 || color > 2) return 0;
	if (sZone) return pc_campaign_start_pikmin(color);
	return sRun.day == 1 ? sMap.story.startPikmin[color] : 0;
}

void pc_mission_note_part(u32 pelletId)
{
	if (!sActive || sResult >= 0) return;
	for (size_t i = 0; i < sMap.story.parts.size() && i < sRun.parts.size(); i++) {
		if (sMap.story.parts[i].id == pelletId && !sRun.parts[i]) {
			sRun.parts[i] = 1;
			return;
		}
	}
}

void pc_mission_note_pellet(u32 pelletId)
{
	if (!sActive || sResult >= 0) return;
	for (size_t i = 0; i < sMap.pellets.size() && i < sRun.pellets.size(); i++) {
		if (sMap.pellets[i].id == pelletId && !sRun.pellets[i]) {
			sRun.pellets[i] = 1;
			return;
		}
	}
}

void pc_mission_note_counts(int born, int kills)
{
	if (sResult >= 0) return;
	sBorn  = born;
	sKills = kills;
}

void pc_mission_update(f32 dt, int pikmin, int bossesAlive, const PcMissionWorld& world)
{
	if (!sActive) return;
	if (sMessageFor > 0.0f) sMessageFor -= dt;
	if (sCameraHold > 0.0f) sCameraHold -= dt;
	if (sResult >= 0) {
		sOverFor += dt;
		return;
	}
	sElapsed += dt;
	if (sStartPikmin < 0) sStartPikmin = pikmin;
	sPikmin = pikmin;
	if (pikmin > 0) sHadPikmin = true;
	// Sin bajas: el primero que cae acaba la misión.
	if (!sZone && sMods.noLosses && sStartPikmin + sBorn - pikmin > 0) {
		finish(0, 2);
		return;
	}
	sBossesAlive = bossesAlive;
	sWaveAlive   = world.waveAlive;
	// Cuenta atrás (de una acción o del límite de tiempo).
	if (sCountdown >= 0.0f && sTextbox.empty()) {
		sCountdown -= dt;
		if (sCountdown < 0.0f) {
			sCountdown = -1.0f;
			applyThen(sCountdownThen);
			if (sResult >= 0) return;
		}
	}
	runTriggers(world, bossesAlive);
	if (sResult >= 0) return;
	const int goal  = sMap.story.objective;
	const bool done = goal != nectar::MAPGOAL_Survive && need() > 0 && progress() >= need();
	if (sZone) {
		// Zona: cumplir el objetivo la completa, pero el día sigue.
		if (done && !sZoneDone) zoneCompleted();
		return;
	}
	if (done) {
		finish(1);
		return;
	}
	if (sHadPikmin && pikmin == 0) finish(0, 1);
}

void pc_mission_set_enemy_dead(int i)
{
	if (i >= 0 && i < int(sRun.dead.size()) && !sRun.dead[size_t(i)]) sRun.dead[size_t(i)] = std::max(1, pc_mission_day());
}

const std::string& pc_mission_gen_cache() { return sRun.gcache; }

void pc_mission_set_gen_cache(const std::string& hex) { sRun.gcache = hex; }

void pc_mission_set_object_state(int i, const std::string& hex)
{
	if (i >= 0 && i < int(sRun.ostate.size())) sRun.ostate[size_t(i)] = hex;
}

void pc_mission_note_onion_found(int color)
{
	if (color < 0 || color > 2 || sRun.found[color]) return;
	sRun.found[color] = true;
	static const char* const kEs[] = { "¡Has encontrado la cebolla azul!", "¡Has encontrado la cebolla roja!", "¡Has encontrado la cebolla amarilla!" };
	static const char* const kEn[] = { "You found the blue Onion!", "You found the red Onion!", "You found the yellow Onion!" };
	message(pc_settings_get_language() == 3 ? kEs[color] : kEn[color]);
}

void pc_mission_set_object_done(int objectIndex)
{
	if (objectIndex >= 0 && objectIndex < int(sRun.objects.size())) sRun.objects[objectIndex] = 1;
}

void pc_mission_end_day(const int night[3][3], int lost)
{
	if (!sActive || sResult >= 0) return;
	int onionPikmin[3];
	for (int c = 0; c < 3; c++) onionPikmin[c] = night[c][0] + night[c][1] + night[c][2];
	sLostAtNight = lost;
	for (int c = 0; c < 3; c++) sNight[c] = onionPikmin[c];
	if (sZone) {
		// La campaña guarda la zona y sigue (o se acaba).
		PcCampaignDayEnd e;
		e.run = runText(sRun, ';');
		for (int c = 0; c < 3; c++) {
			e.onionPikmin[c] = onionPikmin[c];
			for (int k = 0; k < 3; k++) e.onionStages[c][k] = night[c][k];
		}
		// Las que se encontraron aquí pasan a ser de la partida.
		for (int c = 0; c < 3; c++) e.onions[c] = sRun.found[c];
		for (size_t i = 0; i < sMap.story.parts.size() && i < sRun.parts.size(); i++) {
			if (sRun.parts[i]) e.parts.push_back(partName(sMap.story.parts[i].id));
		}
		e.completed = sZoneDone;
		e.medal     = sMedal;
		sZoneEnding = pc_campaign_day_end(e);
		finish(sZoneEnding < 0 ? 2 : sZoneEnding);
		return;
	}
	const int left  = onionPikmin[0] + onionPikmin[1] + onionPikmin[2];
	const bool last = sRun.day >= pc_mission_days();
	if (left == 0) {
		finish(0, 1);
	} else if (last) {
		finish(sMap.story.objective == nectar::MAPGOAL_Survive ? 1 : 0);
	} else {
		for (int c = 0; c < 3; c++) {
			sRun.onion[c] = onionPikmin[c];
			for (int k = 0; k < 3; k++) sRun.onionStage[c][k] = night[c][k];
		}
		sRun.day++;
		writeSave();
		finish(2);
	}
}

bool pc_mission_take_action(int& trigger, int& action, bool& wave)
{
	if (sActionQueue.empty()) return false;
	trigger = sActionQueue.front().trigger;
	action  = sActionQueue.front().action;
	wave    = trigger >= 0 && trigger < int(sMap.story.triggers.size()) && sMap.story.triggers[size_t(trigger)].wave;
	sActionQueue.erase(sActionQueue.begin());
	return true;
}

bool pc_mission_take_end_day(void)
{
	const bool r   = sEndDayRequest && sResult < 0;
	sEndDayRequest = false;
	return r;
}

void pc_mission_note_object_done(int i)
{
	if (i >= 0 && i < int(sObjDone.size())) sObjDone[size_t(i)] = 1;
}

const char* pc_mission_textbox(void) { return sActive && sResult < 0 && !sTextbox.empty() ? sTextbox.c_str() : nullptr; }
void pc_mission_close_textbox(void) { sTextbox.clear(); }
f32 pc_mission_countdown(void) { return sResult < 0 ? sCountdown : -1.0f; }
void pc_mission_set_camera_hold(f32 seconds) { sCameraHold = seconds; }
f32 pc_mission_camera_hold(void) { return sActive ? sCameraHold : 0.0f; }

const char* pc_mission_message(void) { return sMessageFor > 0.0f && sResult < 0 ? sMessage.c_str() : ""; }

PcMissionRecord pc_mission_record(const std::string& id, int players)
{
	PcMissionRecord r;
	const std::string path = recordFile(id, players);
	r.wins   = readKey(path, "wins", 0);
	r.medal  = readKey(path, "medal", -1);
	r.pikmin = readKey(path, "pikmin", 0);
	r.days   = readKey(path, "days", 0);
	return r;
}

const char* pc_mission_medal_name(int medal, bool es)
{
	switch (medal) {
	case 0: return es ? "Bronce" : "Bronze";
	case 1: return es ? "Plata" : "Silver";
	case 2: return es ? "Oro" : "Gold";
	default: return "";
	}
}

int pc_mission_result(void) { return sResult; }
bool pc_mission_over(void) { return sResult >= 0; }

bool pc_mission_end_screen(void) { return sActive && sResult >= 0 && !sExitTaken && sOverFor >= kEndScreenDelay; }

int pc_mission_end_options(int* kinds, int max)
{
	int n     = 0;
	auto push = [&](int k) {
		if (n < max) kinds[n++] = k;
	};
	if (sZone) {
		// Zona de campaña: al mapa de zonas; acabada la campaña, al título.
		if (pc_campaign_ending() < 0) push(PC_MISSION_EXIT_MAP);
	} else if (sResult == 2) {
		push(PC_MISSION_EXIT_NEXT_DAY);
	} else {
		push(PC_MISSION_EXIT_RETRY);
	}
	push(PC_MISSION_EXIT_TITLE);
	return n;
}

void pc_mission_choose_exit(int kind)
{
	if (pc_mission_end_screen()) sExitChoice = kind;
}

int pc_mission_take_exit(void)
{
	if (!sActive || sResult < 0 || sExitTaken || !sExitChoice) return 0;
	sExitTaken = true;
	return sExitChoice;
}

void pc_mission_retry(void)
{
	const PcMissionMods mods = sMods;
	pc_mission_start(false);
	sMods = mods;
}

PcMissionStats pc_mission_stats(void)
{
	PcMissionStats s;
	s.seconds     = sElapsed;
	s.day         = sZone ? pc_campaign_day() - (sResult >= 0 ? 1 : 0) : sRun.day - (sResult == 2 ? 1 : 0);
	s.days        = pc_mission_days();
	s.born        = sBorn;
	s.kills       = sKills;
	s.pikmin      = sPikmin;
	s.lost        = std::max(0, (sStartPikmin < 0 ? 0 : sStartPikmin) + sBorn - sPikmin);
	s.lostAtNight = sLostAtNight;
	s.medal       = sMedal;
	s.newRecord   = sNewRecord;
	for (int c = 0; c < 3; c++) s.night[c] = sNight[c];
	return s;
}

void pc_mission_status(char* out, int n, bool es)
{
	char day[32] = "";
	if (sZone) snprintf(day, sizeof(day), "%s %d   ", es ? "Día" : "Day", pc_campaign_day());
	else if (pc_mission_days() > 1) snprintf(day, sizeof(day), "%s %d/%d   ", es ? "Día" : "Day", sRun.day, pc_mission_days());
	if (sZone && sZoneDone) {
		snprintf(out, n, "%s%s", day, es ? "Zona completada" : "Zone complete");
		return;
	}
	const char* what;
	switch (sMap.story.objective) {
	case nectar::MAPGOAL_Parts: what = es ? "Piezas" : "Parts"; break;
	case nectar::MAPGOAL_Pellets: what = es ? "Pastillas" : "Pellets"; break;
	case nectar::MAPGOAL_Pikmin: what = "Pikmin"; break;
	case nectar::MAPGOAL_Boss: what = es ? "Jefes" : "Bosses"; break;
	case nectar::MAPGOAL_Waves: what = es ? "Oleadas" : "Waves"; break;
	default:
		if (sZone) snprintf(out, n, "%s%s", day, es ? "Explora la zona" : "Explore the zone");
		else snprintf(out, n, "%s%s", day, es ? "Aguanta hasta el último anochecer" : "Survive until the last sunset");
		return;
	}
	snprintf(out, n, "%s%s %d/%d", day, what, progress(), need());
}

const char* pc_mission_announcement(bool es)
{
	static char sBuf[200];
	if (sResult < 0) return "";
	if (sZone) {
		// Fin del día en una zona de campaña (o fin de la campaña).
		const nectar::CampaignEnding* end = pc_campaign_active() ? &pc_campaign_pack()->doc.ending : nullptr;
		const std::string good = end ? pc_campaign_text(end->goodText, end->goodTextEn) : std::string();
		const std::string bad  = end ? pc_campaign_text(end->badText, end->badTextEn) : std::string();
		if (sZoneEnding == 1)
			snprintf(sBuf, sizeof(sBuf), "%s%s%s", es ? "¡CAMPAÑA COMPLETADA!" : "CAMPAIGN COMPLETE!", good.empty() ? "" : " ", good.c_str());
		else if (sZoneEnding == 0)
			snprintf(sBuf, sizeof(sBuf), "%s%s%s", es ? "SE ACABARON LOS DÍAS" : "OUT OF DAYS", bad.empty() ? "" : " ", bad.c_str());
		else if (sLostAtNight > 0)
			snprintf(sBuf, sizeof(sBuf), es ? "FIN DEL DÍA %d - PIKMIN PERDIDOS: %d" : "END OF DAY %d - PIKMIN LOST: %d", pc_campaign_day() - 1, sLostAtNight);
		else
			snprintf(sBuf, sizeof(sBuf), es ? "FIN DEL DÍA %d" : "END OF DAY %d", pc_campaign_day() - 1);
		return sBuf;
	}
	if (sResult == 1) {
		const char* medal = pc_mission_medal_name(sMedal, es);
		snprintf(sBuf, sizeof(sBuf), "%s%s%s%s", es ? "¡MISIÓN CUMPLIDA!" : "MISSION COMPLETE!", *medal ? " - " : "", medal,
		         sNewRecord ? (es ? " - ¡RÉCORD!" : " - NEW RECORD!") : "");
		return sBuf;
	}
	if (sResult == 2) {
		// sRun.day ya es el siguiente.
		if (sLostAtNight > 0)
			snprintf(sBuf, sizeof(sBuf), es ? "FIN DEL DÍA %d - PIKMIN PERDIDOS: %d" : "END OF DAY %d - PIKMIN LOST: %d", sRun.day - 1, sLostAtNight);
		else
			snprintf(sBuf, sizeof(sBuf), es ? "FIN DEL DÍA %d" : "END OF DAY %d", sRun.day - 1);
		return sBuf;
	}
	if (sLoseReason == 2) return es ? "MISIÓN FALLIDA: HA CAÍDO UN PIKMIN" : "MISSION FAILED: A PIKMIN WAS LOST";
	if (sLoseReason == 3) return es ? "MISIÓN FALLIDA" : "MISSION FAILED";
	if (sLoseReason) return es ? "MISIÓN FALLIDA: SIN PIKMIN" : "MISSION FAILED: NO PIKMIN LEFT";
	return es ? "MISIÓN FALLIDA: SE ACABÓ EL TIEMPO" : "MISSION FAILED: OUT OF TIME";
}
