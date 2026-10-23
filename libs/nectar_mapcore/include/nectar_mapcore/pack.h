#pragma once

// Paquetes .nectarpack: un zip con una campaña (campaign.json en la raíz, sus
// zonas en zones/<zona>/) o con un mapa suelto (map.json en la raíz). Si todo
// va dentro de una sola carpeta (lo que hacen las herramientas del sistema al
// comprimir una carpeta), se quita. Solo .json, .png y .mod (mallas
// importadas, que se comprueban al compilar), sin rutas que salgan.
// Se escriben sin comprimir; se leen sin comprimir o con Deflate.
// No depende del juego ni de ninguna librería de zip.

#include <cstdint>
#include <string>
#include <vector>

namespace nectar {

struct PackFile {
	std::string path; ///< relativa, con '/'
	std::vector<std::uint8_t> data;
};

bool writeZip(const std::string& zipPath, const std::vector<PackFile>& files, std::string& error);
bool readZip(const std::string& zipPath, std::vector<PackFile>& files, std::string& error);

/// Los archivos de una carpeta de campaña o de mapa (solo .json y .png).
bool collectPack(const std::string& dir, std::vector<PackFile>& files, std::string& error);
/// Exporta una carpeta (campaña o mapa) a un .nectarpack.
bool exportPack(const std::string& dir, const std::string& zipPath, std::string& error);

/// Instala un .nectarpack en `userDir` (la carpeta user/ del juego): una
/// campaña en campaigns/<id>/, un mapa en maps/<id>/ (id sacado del nombre
/// del archivo; nunca sobrescribe: si ya existe, <id>_2...). Comprueba que se
/// pueda leer. En `installed`, la carpeta creada.
bool installPack(const std::string& zipPath, const std::string& userDir, std::string& installed, std::string& error);

/// Comprimido con Deflate (sin cabecera zlib) → `out` (de `size` bytes).
bool inflateRaw(const std::uint8_t* in, std::size_t inSize, std::vector<std::uint8_t>& out, std::size_t size);

} // namespace nectar
