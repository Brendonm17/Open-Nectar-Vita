// Personajes propios del creador de modelos: cada carpeta válida de
// Load/Characters (character.json + model.nhm, nectar_assets/charpack) es un
// hueco de la tercera fila del selector de capitán, por nombre.

#include "pc_coop.h"

#include "nectar_assets/charpack.h"

#include <filesystem>
#include <string>
#include <vector>

namespace {

struct Slot {
	std::string name, model, portrait;
};
std::vector<Slot> sSlots;
bool sScanned = false;

void scan()
{
	sScanned = true;
	sSlots.clear();
	// El juego corre en su carpeta (ahí están assets/ y Load/).
	for (const nectar::CharacterInfo& c : nectar::listCharacters(".")) {
		if (int(sSlots.size()) >= PC_CAPTAIN_CUSTOM_MAX) break;
		Slot s;
		s.name  = c.name;
		s.model = (std::filesystem::u8path(c.dir) / "model.nhm").u8string();
		std::error_code ec;
		const std::filesystem::path portrait = std::filesystem::u8path(c.dir) / "portrait.png";
		if (std::filesystem::is_regular_file(portrait, ec)) s.portrait = "@" + portrait.u8string();
		sSlots.push_back(std::move(s));
	}
}

const Slot* slot(int captain)
{
	if (!sScanned) scan();
	const int i = captain - PC_CAPTAIN_CUSTOM_FIRST;
	return (i >= 0 && i < int(sSlots.size())) ? &sSlots[std::size_t(i)] : nullptr;
}

} // namespace

bool pc_captain_is_custom(int captain) { return captain >= PC_CAPTAIN_CUSTOM_FIRST && captain < PC_CAPTAIN_COUNT; }
void pc_captain_custom_refresh(void) { scan(); }
int pc_captain_custom_count(void)
{
	if (!sScanned) scan();
	return int(sSlots.size());
}
const char* pc_captain_custom_name(int captain)
{
	const Slot* s = slot(captain);
	return s ? s->name.c_str() : "Custom";
}
const char* pc_captain_custom_model(int captain)
{
	const Slot* s = slot(captain);
	return s ? s->model.c_str() : nullptr;
}
const char* pc_captain_custom_portrait(int captain)
{
	const Slot* s = slot(captain);
	return s && !s->portrait.empty() ? s->portrait.c_str() : nullptr;
}
