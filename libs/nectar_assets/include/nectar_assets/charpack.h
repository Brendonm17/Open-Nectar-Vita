#pragma once

// Personajes propios (.nectarchar): un zip con character.json (nombre, autor),
// model.nhm (la malla con su rig sobre el esqueleto de Olimar, ver
// pc_port/mods/pc_hd_models.cpp) y portrait.png (la tarjeta del selector).
// Se instalan en <juego>/Load/Characters/<id>/ con esos mismos tres archivos;
// el juego da un hueco del selector de capitán a cada carpeta.
//
// Lo usan el creador de modelos (exportar e instalar), el launcher (instalar)
// y el juego (listar). Sin dependencias del juego.

#include <cstdint>
#include <string>
#include <vector>

namespace nectar {

constexpr int kCharacterFormat = 1;
constexpr const char* kCharacterExtension = ".nectarchar";

struct CharacterInfo {
	std::string id;     ///< nombre de su carpeta (letras, números y guiones)
	std::string name;   ///< el que sale en el selector
	std::string author;
	std::string skeleton = "olimar"; ///< sobre qué esqueleto va (de momento solo Olimar)
	int format           = kCharacterFormat;
	std::string dir; ///< carpeta instalada (solo al listar)
};

struct CharacterFiles {
	CharacterInfo info;
	std::vector<std::uint8_t> nhm, portrait; ///< portrait puede ir vacío
};

/// Id para la carpeta a partir del nombre ("Capitán Nox" -> "capitan-nox-1a2b").
std::string characterId(const std::string& name, const std::vector<std::uint8_t>& nhm);

/// Comprueba la cabecera del pack (NHM1, versión 1-3) sin cargarlo entero.
bool validNhm(const std::vector<std::uint8_t>& nhm, std::string& error);

bool writeCharacterPack(const std::string& path, const CharacterFiles& files, std::string& error);
bool readCharacterPack(const std::string& path, CharacterFiles& files, std::string& error);

/// Carpeta Load/Characters de un juego (gameDir = donde están assets/ y Load/).
std::string charactersDir(const std::string& gameDir);
/// Instala (o reemplaza el del mismo id). En `installedDir`, su carpeta.
bool installCharacter(const std::string& gameDir, const CharacterFiles& files, std::string& installedDir, std::string& error);
bool installCharacterPack(const std::string& gameDir, const std::string& packPath, CharacterInfo& info, std::string& error);
/// Los instalados y válidos, por nombre.
std::vector<CharacterInfo> listCharacters(const std::string& gameDir);
bool removeCharacter(const std::string& gameDir, const std::string& id, std::string& error);

} // namespace nectar
