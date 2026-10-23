#include "pc_map_catalog.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

constexpr const char* kUserMaps   = "user/maps";
constexpr const char* kLegacyMaps = "user/maps/vs"; // solo VS, de antes del formato 2

std::vector<PcMapCatalogEntry> sList;
bool sScanned = false;

bool readText(const std::string& path, std::string& out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

void scan(const char* dir, bool legacy)
{
	namespace fs = std::filesystem;
	std::error_code ec;
	for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
		if (!it->is_directory(ec)) continue;
		PcMapCatalogEntry e;
		e.id     = it->path().filename().string();
		e.legacy = legacy;
		if (!legacy && e.id == "vs") continue;
		e.file = (it->path() / "map.json").string();
		if (!fs::exists(e.file, ec)) continue;
		if (std::any_of(sList.begin(), sList.end(), [&](const PcMapCatalogEntry& o) { return o.id == e.id; })) {
			fprintf(stderr, "[maps] %s: ya hay un mapa con ese id en %s; se ignora\n", e.file.c_str(), kLegacyMaps);
			continue;
		}
		nectar::MapData m;
		if (pc_map_catalog_load(e.file, m, e.error)) {
			e.name   = m.name;
			e.author = m.author;
			e.modes  = m.modes;
		} else {
			e.name = e.id;
		}
		const fs::path thumb = it->path() / "thumb.png";
		if (fs::exists(thumb, ec)) e.thumb = thumb.string();
		sList.push_back(e);
	}
}

} // namespace

bool pc_map_catalog_load(const std::string& path, nectar::MapData& out, std::string& error, std::string* source)
{
	std::string text;
	if (!readText(path, text)) {
		error = "no se encuentra " + path;
		return false;
	}
	if (source) *source = text;
	if (!nectar::parseMap(text, out, error)) return false;
	const size_t slash = path.find_last_of("/\\");
	out.dir            = slash == std::string::npos ? "." : path.substr(0, slash);
	return true;
}

std::string pc_map_catalog_file(const std::string& id)
{
	const std::string legacy = std::string(kLegacyMaps) + "/" + id + "/map.json";
	std::error_code ec;
	if (std::filesystem::exists(legacy, ec)) return legacy;
	return std::string(kUserMaps) + "/" + id + "/map.json";
}

const std::vector<PcMapCatalogEntry>& pc_map_catalog(bool rescan)
{
	if (sScanned && !rescan) return sList;
	sList.clear();
	scan(kLegacyMaps, true);
	scan(kUserMaps, false);
	std::sort(sList.begin(), sList.end(), [](const PcMapCatalogEntry& a, const PcMapCatalogEntry& b) { return a.id < b.id; });
	sScanned = true;
	return sList;
}
