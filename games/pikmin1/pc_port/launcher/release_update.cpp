#include "release_update.h"

#include "json_mini.h"
#include "launcher_platform.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#ifdef _WIN32
#include "stb_image.h"

#include <cstdint>
#endif

#ifndef OPEN_NECTAR_VERSION
#define OPEN_NECTAR_VERSION "0.0"
#endif

namespace fs = std::filesystem;

namespace pikmin {
namespace launcher {
namespace {

constexpr const char* kLatestReleaseApi
    = "https://api.github.com/repos/SSunnKing/Open-Nectar---Pikmin-Native-PC-Mobile-Port/releases/latest";

// Paquetes de este sistema en un release, por orden de preferencia. En Linux
// se publican los dos formatos; el .tar.gz se extrae sin más, el AppImage
// queda como alternativa.
#ifdef _WIN32
const char* const kAssetNames[] = { "nectar-windows.zip" };
#elif defined(__APPLE__)
const char* const kAssetNames[] = { "nectar-macos.tar.gz" };
#elif defined(__ANDROID__)
// Android: el .apk del release, se llame como se llame (open_nectar_0.9.apk).
const char* const kAssetNames[] = { ".apk" };
#else
const char* const kAssetNames[] = { "nectar-linux.tar.gz", "Open_Nectar-x86_64.AppImage" };
#endif

#ifndef _WIN32
bool isAppImage(const std::string& name)
{
    return name.size() > 9 && name.compare(name.size() - 9, 9, ".AppImage") == 0;
}
#endif

// "v1.2.3-beta" -> {1, 2, 3}: los números, en orden; lo demás se ignora.
std::vector<long> versionParts(const std::string& text)
{
    std::vector<long> parts;
    std::size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && (text[at] < '0' || text[at] > '9')) {
            // Un sufijo tras los números ("-beta") no cuenta.
            if (!parts.empty() && text[at] != '.') return parts;
            ++at;
        }
        if (at >= text.size()) break;
        char* end = nullptr;
        parts.push_back(std::strtol(text.c_str() + at, &end, 10));
        at = std::size_t(end - text.c_str());
    }
    return parts;
}

bool readText(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// Busca, hasta tres niveles por debajo de `root`, la carpeta con el launcher.
bool findPackage(const fs::path& root, const std::string& launcherName, fs::path& found)
{
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() > 3) {
            it.disable_recursion_pending();
            continue;
        }
        const std::string name = it->path().filename().string();
        if (it->is_regular_file(ec) && (name == launcherName || name == launcherName + ".real")) {
            found = it->path().parent_path();
            return true;
        }
    }
    return false;
}

} // namespace

const char* currentVersion() { return OPEN_NECTAR_VERSION; }

std::vector<TexturePackDownload> texturePackDownloads(int game)
{
#ifdef __ANDROID__
    // Android: PNG, el formato que el móvil carga sin problemas.
    if (game == 0) return { { "HD", "https://drive.usercontent.google.com/download?export=download&confirm=t&id=1870dA11T9FpuLQDnI3axoCJ_Eh5ptgWP", 925049459ull } };
    return { { "HD", "https://drive.usercontent.google.com/download?export=download&confirm=t&id=1blzsLSFBr617h-gP7lvp_7HqsLRyQWzl", 1072944206ull } };
#else
    if (game == 0) return { { "4K", "https://drive.usercontent.google.com/download?export=download&confirm=t&id=1leAGZmK1Ay_W3KwtAyyyBWZzyw5Wy3Db", 1240690969ull },
                             { "1080p", "https://drive.usercontent.google.com/download?export=download&confirm=t&id=1eZQmHBen1EuZcIUzfAEZUJl7TeL93ClX", 560393581ull } };
    return { { "4K", "https://drive.usercontent.google.com/download?export=download&confirm=t&id=1hsm2XnnZ0e8U-Ikjuxt5NP8xc-1boIEF", 3068646745ull },
             { "1080p", "https://drive.usercontent.google.com/download?export=download&confirm=t&id=1gcjtuP09gGoFxs41iNqfiMsaZjE4ddqS", 1116177778ull } };
#endif
}

const char* texturePackFolder(int game) { return game == 0 ? "GPI" : "GPV"; }

bool isNewerVersion(const std::string& candidate, const std::string& current)
{
    std::vector<long> a = versionParts(candidate), b = versionParts(current);
    if (a.empty()) return false;
    const std::size_t n = std::max(a.size(), b.size());
    a.resize(n, 0);
    b.resize(n, 0);
    return a > b;
}

bool fetchLatestRelease(ReleaseInfo& release, std::string& error)
{
    const fs::path cached = platform::cacheDirectory() / "latest-release.json";
    if (!platform::downloadFile(kLatestReleaseApi, cached, error)) {
        error = "could not reach GitHub (" + error + ")";
        return false;
    }
    std::string text;
    Json root;
    if (!readText(cached, text) || !parseJson(text, root) || root.type != Json::Type::Object) {
        error = "GitHub sent an answer that could not be read";
        return false;
    }
    release = ReleaseInfo();
    release.version = root.str("tag_name");
    release.title = root.str("name");
    release.notes = root.str("body");
    if (const Json* assets = root.get("assets"); assets && assets->type == Json::Type::Array) {
        for (const char* wanted : kAssetNames) {
            for (const Json& asset : assets->items) {
#ifdef __ANDROID__
                const std::string name = asset.str("name");
                const bool match = name.size() > 4 && name.compare(name.size() - 4, 4, wanted) == 0;
#else
                const bool match = asset.str("name") == wanted;
#endif
                if (release.assetUrl.empty() && match) {
                    release.assetName = asset.str("name");
                    release.assetUrl = asset.str("browser_download_url");
                }
            }
        }
    }
    if (release.version.empty()) {
        error = "GitHub has no release yet";
        return false;
    }
    if (release.assetUrl.empty()) {
        error = std::string("the latest release has no ") + kAssetNames[0];
        return false;
    }
    return true;
}

namespace {

#ifdef _WIN32
// Extrae el .zip de la release dentro del launcher, con el inflador de
// stb_image: lanzar tar.exe oculto tras descargar es otro patrón que los
// antivirus marcan como troyano.
std::uint32_t zipCrc32(const std::uint8_t* data, std::size_t size)
{
    static std::uint32_t table[256];
    if (!table[1]) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

bool extractZip(const fs::path& archive, const fs::path& destination, std::string& error)
{
    std::ifstream in(archive, std::ios::binary);
    const std::vector<std::uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto get16 = [&](std::size_t at) -> std::uint32_t { return at + 2 <= d.size() ? d[at] | (d[at + 1] << 8) : 0; };
    auto get32 = [&](std::size_t at) -> std::uint32_t {
        return at + 4 <= d.size() ? get16(at) | (get16(at + 2) << 16) : 0;
    };
    error = "damaged zip";
    std::size_t eocd = std::string::npos;
    for (std::size_t at = d.size() >= 22 ? d.size() - 22 : 0; d.size() >= 22; --at) {
        if (get32(at) == 0x06054b50) {
            eocd = at;
            break;
        }
        if (at == 0 || d.size() - at > 22 + 0xFFFF) break;
    }
    if (eocd == std::string::npos) return false;
    const std::uint32_t count = get16(eocd + 10);
    std::size_t at = get32(eocd + 16);
    for (std::uint32_t e = 0; e < count; ++e) {
        if (get32(at) != 0x02014b50 || at + 46 > d.size()) return false;
        const std::uint32_t method = get16(at + 10), crc = get32(at + 16), csize = get32(at + 20), usize = get32(at + 24);
        const std::uint32_t nameLen = get16(at + 28), extraLen = get16(at + 30), commentLen = get16(at + 32);
        const std::uint32_t local = get32(at + 42);
        if (at + 46 + nameLen > d.size()) return false;
        std::string name(reinterpret_cast<const char*>(&d[at + 46]), nameLen);
        at += 46 + nameLen + extraLen + commentLen;
        std::replace(name.begin(), name.end(), '\\', '/');
        // Nada de rutas absolutas ni "..": todo queda dentro de destination.
        const fs::path relative = fs::u8path(name).lexically_normal();
        if (name.empty() || relative.is_absolute() || relative.has_root_name()
            || (!relative.empty() && *relative.begin() == "..")) {
            error = "unsafe path in zip: " + name;
            return false;
        }
        const fs::path target = destination / relative;
        std::error_code ec;
        if (name.back() == '/') {
            fs::create_directories(target, ec);
            continue;
        }
        if (get32(local) != 0x04034b50) return false;
        const std::size_t start = std::size_t(local) + 30 + get16(local + 26) + get16(local + 28);
        if (start + csize > d.size() || usize > 0x7FFFFFFFu || csize > 0x7FFFFFFFu) return false;
        std::vector<std::uint8_t> data;
        if (method == 0) {
            data.assign(d.begin() + std::ptrdiff_t(start), d.begin() + std::ptrdiff_t(start + csize));
        } else if (method == 8) {
            data.resize(usize);
            const int got = stbi_zlib_decode_noheader_buffer(reinterpret_cast<char*>(data.data()), int(usize),
                                                             reinterpret_cast<const char*>(&d[start]), int(csize));
            if (got != int(usize)) return false;
        } else {
            error = "unsupported compression in zip (" + name + ")";
            return false;
        }
        if (zipCrc32(data.data(), data.size()) != crc) {
            error = "damaged zip (" + name + ")";
            return false;
        }
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!out) {
            error = "could not write " + target.string();
            return false;
        }
    }
    error.clear();
    return true;
}
#endif

} // namespace

bool downloadRelease(const ReleaseInfo& release, const std::string& launcherName, fs::path& packageDirectory,
                     std::string& error)
{
    const fs::path base = platform::cacheDirectory() / "update";
    std::error_code ec;
    fs::remove_all(base, ec); // restos de una actualización anterior
    fs::create_directories(base / "extracted", ec);
    if (ec) {
        error = "could not prepare the download folder: " + ec.message();
        return false;
    }
    const fs::path archive = base / release.assetName;
    if (!platform::downloadFile(release.assetUrl, archive, error)) {
        error = "the download failed (" + error + ")";
        return false;
    }

    std::string output;
#ifndef _WIN32
    if (isAppImage(release.assetName)) {
        // El AppImage se desempaqueta a sí mismo (squashfs-root/usr/bin), sin FUSE.
        fs::permissions(archive, fs::perms::owner_exec, fs::perm_options::add, ec);
        if (!platform::runAndCapture(archive, { "--appimage-extract" }, base / "extracted", output, error)) {
            error = "could not unpack " + release.assetName + " (" + error + ")";
            return false;
        }
        if (!findPackage(base / "extracted", launcherName, packageDirectory)) {
            error = "the downloaded package has no launcher in it";
            return false;
        }
        return true;
    }
#endif
#ifdef _WIN32
    if (!extractZip(archive, base / "extracted", error)) {
        error = "could not unpack " + release.assetName + " (" + error + ")";
        return false;
    }
#else
    // tar viene con cualquier Linux.
    const std::vector<std::string> arguments = { "-xzf", archive.string(), "-C", (base / "extracted").string() };
    if (!platform::runAndCapture("tar", arguments, base, output, error)) {
        error = "could not unpack " + release.assetName + " (" + error + ")";
        return false;
    }
#endif
    if (!findPackage(base / "extracted", launcherName, packageDirectory)) {
        error = "the downloaded package has no launcher in it";
        return false;
    }
    return true;
}

bool cachedRelease(const std::string& launcherName, fs::path& packageDirectory)
{
    std::error_code ec;
    const fs::path extracted = platform::cacheDirectory() / "update" / "extracted";
    return fs::is_directory(extracted, ec) && findPackage(extracted, launcherName, packageDirectory);
}

void forgetCachedRelease()
{
    std::error_code ec;
    fs::remove_all(platform::cacheDirectory() / "update", ec);
}

} // namespace launcher
} // namespace pikmin
