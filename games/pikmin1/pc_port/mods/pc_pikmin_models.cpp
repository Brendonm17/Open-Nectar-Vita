// Modelos de Pikmin propios: los packs de Load/Pikmin (nectar_assets/charpack)
// y cuál usa cada color según los ajustes (pc_settings_get_pikmin_model).

#include "pc_pikmin_models.h"

#include "nectar_assets/charpack.h"
#include "settings/pc_settings.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

struct Pack {
	nectar::CharacterInfo info;
	std::string model;
};
std::vector<Pack> sPacks;
bool sScanned = false;

// Lo elegido para cada color, ya resuelto (se mira el id de los ajustes en
// cada consulta, pero solo se busca cuando cambia).
struct Choice {
	std::string id;
	int pack = -1;
	bool resolved = false;
};
Choice sChoice[3];

const char* const kKeys[3] = { "blue", "red", "yellow" };

void scan()
{
	sScanned = true;
	sPacks.clear();
	// El juego corre en su carpeta (ahí están assets/ y Load/).
	for (const nectar::CharacterInfo& c : nectar::listPikminModels(".")) {
		Pack p;
		p.info  = c;
		p.model = (std::filesystem::u8path(c.dir) / "model.nhm").u8string();
		sPacks.push_back(std::move(p));
	}
	for (Choice& c : sChoice) c.resolved = false;
}

void ensure()
{
	if (!sScanned) scan();
}

// Índices de sPacks que valen para ese color.
std::vector<int> packsFor(int color)
{
	ensure();
	std::vector<int> out;
	if (color < 0 || color > 2) return out;
	for (std::size_t i = 0; i < sPacks.size(); ++i)
		if (sPacks[i].info.forPikmin(kKeys[color])) out.push_back(int(i));
	return out;
}

const Pack* chosen(int color)
{
	if (color < 0 || color > 2) return nullptr;
	ensure();
	const char* id = pc_settings_get_pikmin_model(color);
	Choice& c      = sChoice[color];
	if (!c.resolved || c.id != (id ? id : "")) {
		c.id       = id ? id : "";
		c.pack     = -1;
		c.resolved = true;
		if (!c.id.empty())
			for (int i : packsFor(color))
				if (sPacks[std::size_t(i)].info.id == c.id) c.pack = i;
	}
	return c.pack >= 0 ? &sPacks[std::size_t(c.pack)] : nullptr;
}

} // namespace

void pc_pikmin_models_refresh(void) { scan(); }

int pc_pikmin_models_count(int color) { return int(packsFor(color).size()); }

const char* pc_pikmin_models_id(int color, int index)
{
	const std::vector<int> p = packsFor(color);
	return index >= 0 && index < int(p.size()) ? sPacks[std::size_t(p[std::size_t(index)])].info.id.c_str() : "";
}

const char* pc_pikmin_models_name(int color, int index)
{
	const std::vector<int> p = packsFor(color);
	return index >= 0 && index < int(p.size()) ? sPacks[std::size_t(p[std::size_t(index)])].info.name.c_str() : "";
}

const char* pc_pikmin_models_name_of(int color, const char* id)
{
	if (!id || !*id) return "";
	for (int i : packsFor(color))
		if (sPacks[std::size_t(i)].info.id == id) return sPacks[std::size_t(i)].info.name.c_str();
	return "";
}

const char* pc_pikmin_models_colour_key(int color) { return color >= 0 && color < 3 ? kKeys[color] : ""; }

int pc_pikmin_models_colour_from_key(const char* key)
{
	for (int i = 0; i < 3; ++i)
		if (key && std::strcmp(key, kKeys[i]) == 0) return i;
	return -1;
}

const char* pc_pikmin_model_path(int color)
{
	const Pack* p = chosen(color);
	return p ? p->model.c_str() : nullptr;
}

bool pc_pikmin_model_tinted(int color)
{
	const Pack* p = chosen(color);
	return p && p->info.target == nectar::kTargetPikminAll;
}
