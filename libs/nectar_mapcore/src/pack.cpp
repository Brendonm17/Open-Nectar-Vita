#include "nectar_mapcore/pack.h"

#include "nectar_mapcore/campaign.h"
#include "nectar_mapcore/i18n.h"
#include "nectar_mapcore/map.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace nectar {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxFiles = 512, kMaxFile = std::size_t(32) << 20, kMaxTotal = std::size_t(128) << 20;

std::uint32_t crc32(const std::uint8_t* p, std::size_t n)
{
	static std::uint32_t table[256];
	static bool ready = false;
	if (!ready) {
		for (std::uint32_t i = 0; i < 256; i++) {
			std::uint32_t c = i;
			for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		ready = true;
	}
	std::uint32_t c = 0xFFFFFFFFu;
	for (std::size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

void put16(std::vector<std::uint8_t>& o, std::uint32_t v)
{
	o.push_back(std::uint8_t(v));
	o.push_back(std::uint8_t(v >> 8));
}
void put32(std::vector<std::uint8_t>& o, std::uint32_t v)
{
	put16(o, v & 0xFFFF);
	put16(o, v >> 16);
}
std::uint32_t get16(const std::uint8_t* p) { return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8; }
std::uint32_t get32(const std::uint8_t* p) { return get16(p) | get16(p + 2) << 16; }

bool hasExt(const std::string& p, const char* ext)
{
	const std::size_t n = std::strlen(ext);
	if (p.size() < n) return false;
	for (std::size_t i = 0; i < n; i++) {
		if (std::tolower((unsigned char)p[p.size() - n + i]) != ext[i]) return false;
	}
	return true;
}

// Una ruta de dentro del paquete que no sale de su carpeta.
bool safePath(const std::string& p)
{
	if (p.empty() || p.size() > 200 || p[0] == '/' || p.find('\\') != std::string::npos || p.find(':') != std::string::npos) return false;
	std::size_t start = 0;
	while (start <= p.size()) {
		const std::size_t end = std::min(p.find('/', start), p.size());
		const std::string seg = p.substr(start, end - start);
		if (seg.empty() || seg == "." || seg == "..") return false;
		start = end + 1;
	}
	return true;
}

bool readFile(const fs::path& p, std::vector<std::uint8_t>& out)
{
	std::ifstream f(p, std::ios::binary);
	if (!f) return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}

// ── Deflate (como «puff» de zlib, en pequeño) ───────────────────────────────

struct Inflate {
	const std::uint8_t* in;
	std::size_t inSize, inPos = 0;
	std::uint32_t bitBuf = 0;
	int bitCnt           = 0;
	std::vector<std::uint8_t>* out;
	std::size_t outMax;

	int bits(int need)
	{
		std::uint32_t val = bitBuf;
		while (bitCnt < need) {
			if (inPos >= inSize) throw std::runtime_error("deflate: faltan datos");
			val |= std::uint32_t(in[inPos++]) << bitCnt;
			bitCnt += 8;
		}
		bitBuf = val >> need;
		bitCnt -= need;
		return int(val & ((1u << need) - 1));
	}

	struct Huffman {
		short count[16];
		short symbol[288];
	};

	int decode(const Huffman& h)
	{
		int code = 0, first = 0, index = 0;
		for (int len = 1; len <= 15; len++) {
			code |= bits(1);
			const int count = h.count[len];
			if (code - count < first) return h.symbol[index + (code - first)];
			index += count;
			first += count;
			first <<= 1;
			code <<= 1;
		}
		throw std::runtime_error("deflate: código no válido");
	}

	static int construct(Huffman& h, const short* length, int n)
	{
		for (short& c : h.count) c = 0;
		for (int s = 0; s < n; s++) h.count[length[s]]++;
		if (h.count[0] == n) return 0;
		int left = 1;
		for (int len = 1; len <= 15; len++) {
			left <<= 1;
			left -= h.count[len];
			if (left < 0) return left;
		}
		short offs[16];
		offs[1] = 0;
		for (int len = 1; len < 15; len++) offs[len + 1] = short(offs[len] + h.count[len]);
		for (int s = 0; s < n; s++) {
			if (length[s]) h.symbol[offs[length[s]]++] = short(s);
		}
		return left;
	}

	void emit(std::uint8_t b)
	{
		if (out->size() >= outMax) throw std::runtime_error("deflate: más datos de los dichos");
		out->push_back(b);
	}

	void codes(const Huffman& lencode, const Huffman& distcode)
	{
		static const short kBase[] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
		static const short kExt[]  = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
		static const short kDist[] = { 1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
			                           193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
		static const short kDExt[] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
		for (;;) {
			int symbol = decode(lencode);
			if (symbol < 256) {
				emit(std::uint8_t(symbol));
			} else if (symbol == 256) {
				return;
			} else {
				symbol -= 257;
				if (symbol >= 29) throw std::runtime_error("deflate: longitud no válida");
				const int len = kBase[symbol] + bits(kExt[symbol]);
				symbol        = decode(distcode);
				if (symbol >= 30) throw std::runtime_error("deflate: distancia no válida");
				const std::size_t dist = std::size_t(kDist[symbol] + bits(kDExt[symbol]));
				if (dist > out->size()) throw std::runtime_error("deflate: distancia demasiado lejos");
				for (int k = 0; k < len; k++) emit((*out)[out->size() - dist]);
			}
		}
	}

	void stored()
	{
		bitBuf = 0;
		bitCnt = 0;
		if (inPos + 4 > inSize) throw std::runtime_error("deflate: faltan datos");
		const std::uint32_t len = get16(in + inPos), nlen = get16(in + inPos + 2);
		inPos += 4;
		if (len != (~nlen & 0xFFFF)) throw std::runtime_error("deflate: bloque guardado dañado");
		if (inPos + len > inSize) throw std::runtime_error("deflate: faltan datos");
		for (std::uint32_t k = 0; k < len; k++) emit(in[inPos++]);
	}

	void fixed()
	{
		static Huffman lencode, distcode;
		static bool ready = false;
		if (!ready) {
			short lengths[288];
			int s = 0;
			for (; s < 144; s++) lengths[s] = 8;
			for (; s < 256; s++) lengths[s] = 9;
			for (; s < 280; s++) lengths[s] = 7;
			for (; s < 288; s++) lengths[s] = 8;
			construct(lencode, lengths, 288);
			for (s = 0; s < 30; s++) lengths[s] = 5;
			construct(distcode, lengths, 30);
			ready = true;
		}
		codes(lencode, distcode);
	}

	void dynamic()
	{
		static const short kOrder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
		short lengths[320];
		const int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
		if (nlen > 286 || ndist > 30) throw std::runtime_error("deflate: tablas no válidas");
		int index = 0;
		for (; index < ncode; index++) lengths[kOrder[index]] = short(bits(3));
		for (; index < 19; index++) lengths[kOrder[index]] = 0;
		Huffman lencode, distcode;
		if (construct(lencode, lengths, 19) != 0) throw std::runtime_error("deflate: tablas no válidas");
		index = 0;
		while (index < nlen + ndist) {
			int symbol = decode(lencode);
			if (symbol < 16) {
				lengths[index++] = short(symbol);
			} else {
				short len = 0;
				if (symbol == 16) {
					if (index == 0) throw std::runtime_error("deflate: repetición sin nada");
					len    = lengths[index - 1];
					symbol = 3 + bits(2);
				} else if (symbol == 17) {
					symbol = 3 + bits(3);
				} else {
					symbol = 11 + bits(7);
				}
				if (index + symbol > nlen + ndist) throw std::runtime_error("deflate: demasiadas longitudes");
				while (symbol--) lengths[index++] = len;
			}
		}
		if (lengths[256] == 0) throw std::runtime_error("deflate: sin fin de bloque");
		int err = construct(lencode, lengths, nlen);
		if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) throw std::runtime_error("deflate: tabla incompleta");
		err = construct(distcode, lengths + nlen, ndist);
		if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) throw std::runtime_error("deflate: tabla incompleta");
		codes(lencode, distcode);
	}
};

std::string slug(const std::string& name)
{
	std::string out;
	for (unsigned char c : name) {
		if (std::isalnum(c)) out += char(std::tolower(c));
		else if ((c == ' ' || c == '-' || c == '_' || c == '.') && !out.empty() && out.back() != '_') out += '_';
	}
	while (!out.empty() && out.back() == '_') out.pop_back();
	return out.empty() ? std::string("paquete") : out.substr(0, 40);
}

} // namespace

bool inflateRaw(const std::uint8_t* in, std::size_t inSize, std::vector<std::uint8_t>& out, std::size_t size)
{
	try {
		out.clear();
		out.reserve(size);
		Inflate s;
		s.in     = in;
		s.inSize = inSize;
		s.out    = &out;
		s.outMax = size;
		int last;
		do {
			last           = s.bits(1);
			const int type = s.bits(2);
			if (type == 0) s.stored();
			else if (type == 1) s.fixed();
			else if (type == 2) s.dynamic();
			else throw std::runtime_error("deflate: tipo de bloque no válido");
		} while (!last);
		return out.size() == size;
	} catch (const std::exception&) {
		return false;
	}
}

bool writeZip(const std::string& zipPath, const std::vector<PackFile>& files, std::string& error)
{
	std::vector<std::uint8_t> zip, central;
	for (const PackFile& f : files) {
		const std::uint32_t crc = crc32(f.data.data(), f.data.size()), size = std::uint32_t(f.data.size()), offset = std::uint32_t(zip.size());
		// Cabecera local + datos (sin comprimir).
		put32(zip, 0x04034b50);
		put16(zip, 20);
		put16(zip, 0x0800); // nombres en UTF-8
		put16(zip, 0);      // sin comprimir
		put16(zip, 0);
		put16(zip, 0x21); // 1980-01-01
		put32(zip, crc);
		put32(zip, size);
		put32(zip, size);
		put16(zip, std::uint32_t(f.path.size()));
		put16(zip, 0);
		zip.insert(zip.end(), f.path.begin(), f.path.end());
		zip.insert(zip.end(), f.data.begin(), f.data.end());
		// Su entrada en el directorio central.
		put32(central, 0x02014b50);
		put16(central, 20);
		put16(central, 20);
		put16(central, 0x0800);
		put16(central, 0);
		put16(central, 0);
		put16(central, 0x21);
		put32(central, crc);
		put32(central, size);
		put32(central, size);
		put16(central, std::uint32_t(f.path.size()));
		put16(central, 0);
		put16(central, 0);
		put16(central, 0);
		put16(central, 0);
		put32(central, 0);
		put32(central, offset);
		central.insert(central.end(), f.path.begin(), f.path.end());
	}
	const std::uint32_t centralAt = std::uint32_t(zip.size());
	zip.insert(zip.end(), central.begin(), central.end());
	put32(zip, 0x06054b50);
	put16(zip, 0);
	put16(zip, 0);
	put16(zip, std::uint32_t(files.size()));
	put16(zip, std::uint32_t(files.size()));
	put32(zip, std::uint32_t(central.size()));
	put32(zip, centralAt);
	put16(zip, 0);
	std::ofstream out(zipPath, std::ios::binary | std::ios::trunc);
	if (!out || !out.write(reinterpret_cast<const char*>(zip.data()), std::streamsize(zip.size()))) {
		error = tr("no se puede escribir ") + zipPath;
		return false;
	}
	return true;
}

bool readZip(const std::string& zipPath, std::vector<PackFile>& files, std::string& error)
{
	try {
		std::vector<std::uint8_t> z;
		if (!readFile(zipPath, z)) throw std::runtime_error(tr("no se puede leer ") + zipPath);
		if (z.size() < 22 || z.size() > kMaxTotal) throw std::runtime_error(tr("no es un paquete válido (tamaño)"));
		// El final del directorio central, buscando desde atrás (puede haber comentario).
		std::size_t end = std::string::npos;
		for (std::size_t i = z.size() - 22 + 1; i-- > 0 && z.size() - i <= 22 + 65535;) {
			if (get32(&z[i]) == 0x06054b50) {
				end = i;
				break;
			}
		}
		if (end == std::string::npos) throw std::runtime_error(tr("no es un zip"));
		const std::uint32_t count = get16(&z[end + 10]), centralAt = get32(&z[end + 16]);
		if (count > kMaxFiles) throw std::runtime_error(tr("el paquete tiene demasiados archivos"));
		std::size_t p = centralAt, total = 0;
		files.clear();
		for (std::uint32_t k = 0; k < count; k++) {
			if (p + 46 > z.size() || get32(&z[p]) != 0x02014b50) throw std::runtime_error(tr("zip dañado"));
			const std::uint32_t flags = get16(&z[p + 8]), method = get16(&z[p + 10]), crc = get32(&z[p + 16]);
			const std::uint32_t csize = get32(&z[p + 20]), usize = get32(&z[p + 24]);
			const std::uint32_t nlen = get16(&z[p + 28]), elen = get16(&z[p + 30]), clen = get16(&z[p + 32]), local = get32(&z[p + 42]);
			if (p + 46 + nlen > z.size()) throw std::runtime_error(tr("zip dañado"));
			std::string name(reinterpret_cast<const char*>(&z[p + 46]), nlen);
			p += 46 + nlen + elen + clen;
			if (!name.empty() && name.back() == '/') continue; // carpetas
			if (flags & 1) throw std::runtime_error(tr("zip cifrado"));
			if (usize > kMaxFile || (total += usize) > kMaxTotal) throw std::runtime_error(tr("el paquete es demasiado grande"));
			if (local + 30 > z.size() || get32(&z[local]) != 0x04034b50) throw std::runtime_error(tr("zip dañado"));
			const std::size_t dataAt = local + 30 + get16(&z[local + 26]) + get16(&z[local + 28]);
			if (dataAt + csize > z.size()) throw std::runtime_error(tr("zip dañado"));
			PackFile f;
			f.path = name;
			if (method == 0) {
				if (csize != usize) throw std::runtime_error(tr("zip dañado"));
				f.data.assign(z.begin() + long(dataAt), z.begin() + long(dataAt + csize));
			} else if (method == 8) {
				if (!inflateRaw(&z[dataAt], csize, f.data, usize)) throw std::runtime_error(tr("no se puede descomprimir: ") + name);
			} else {
				throw std::runtime_error(tr("compresión no soportada (solo sin comprimir o Deflate): ") + name);
			}
			if (crc32(f.data.data(), f.data.size()) != crc) throw std::runtime_error(tr("archivo dañado en el paquete: ") + name);
			files.push_back(std::move(f));
		}
		return true;
	} catch (const std::exception& e) {
		error = e.what();
		return false;
	}
}

bool collectPack(const std::string& dir, std::vector<PackFile>& files, std::string& error)
{
	files.clear();
	std::error_code ec;
	const fs::path root(dir);
	if (!fs::exists(root / "campaign.json", ec) && !fs::exists(root / "map.json", ec)) {
		error = tr("la carpeta no tiene campaign.json ni map.json");
		return false;
	}
	for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
		if (!it->is_regular_file(ec)) continue;
		const std::string rel = fs::relative(it->path(), root, ec).generic_string();
		if (!hasExt(rel, ".json") && !hasExt(rel, ".png") && !hasExt(rel, ".mod")) continue;
		PackFile f;
		f.path = rel;
		if (!readFile(it->path(), f.data)) {
			error = tr("no se puede leer ") + rel;
			return false;
		}
		files.push_back(std::move(f));
	}
	std::sort(files.begin(), files.end(), [](const PackFile& a, const PackFile& b) { return a.path < b.path; });
	return true;
}

bool exportPack(const std::string& dir, const std::string& zipPath, std::string& error)
{
	std::vector<PackFile> files;
	return collectPack(dir, files, error) && writeZip(zipPath, files, error);
}

bool installPack(const std::string& zipPath, const std::string& userDir, std::string& installed, std::string& error)
{
	try {
		std::vector<PackFile> files;
		if (!readZip(zipPath, files, error)) return false;
		if (files.empty()) throw std::runtime_error(tr("el paquete está vacío"));
		// Todo dentro de una carpeta: se quita.
		const std::size_t slash = files[0].path.find('/');
		if (slash != std::string::npos) {
			const std::string top = files[0].path.substr(0, slash + 1);
			bool all              = true;
			for (const PackFile& f : files) all = all && f.path.compare(0, top.size(), top) == 0;
			if (all) {
				for (PackFile& f : files) f.path = f.path.substr(top.size());
			}
		}
		const PackFile* campaign = nullptr, *map = nullptr;
		for (const PackFile& f : files) {
			if (!safePath(f.path)) throw std::runtime_error(tr("ruta no válida en el paquete: ") + f.path);
			if (!hasExt(f.path, ".json") && !hasExt(f.path, ".png") && !hasExt(f.path, ".mod")) throw std::runtime_error(tr("archivo no permitido en el paquete: ") + f.path);
			if (f.path == "campaign.json") campaign = &f;
			if (f.path == "map.json") map = &f;
		}
		if (!campaign && !map) throw std::runtime_error(tr("el paquete no tiene campaign.json ni map.json"));
		// Que se pueda leer antes de instalar nada.
		std::string err;
		if (campaign) {
			CampaignDoc d;
			if (!parseCampaignDoc(std::string(campaign->data.begin(), campaign->data.end()), d, err)) throw std::runtime_error(err);
			for (const CampaignZone& z : d.zones) {
				const std::string want = z.folder() + "/map.json";
				const auto it = std::find_if(files.begin(), files.end(), [&](const PackFile& f) { return f.path == want; });
				MapDoc m;
				if (it == files.end()) throw std::runtime_error(tr("falta el nivel de la zona ") + z.id);
				if (!parseMapDoc(std::string(it->data.begin(), it->data.end()), m, err)) throw std::runtime_error(z.id + ": " + err);
			}
		} else {
			MapDoc m;
			if (!parseMapDoc(std::string(map->data.begin(), map->data.end()), m, err)) throw std::runtime_error(err);
		}
		// Carpeta nueva (nunca se sobrescribe nada).
		std::error_code ec;
		const std::string base = slug(fs::path(zipPath).stem().string());
		const fs::path parent  = fs::path(userDir) / (campaign ? "campaigns" : "maps");
		fs::path dest          = parent / base;
		for (int n = 2; fs::exists(dest, ec); n++) dest = parent / (base + "_" + std::to_string(n));
		for (const PackFile& f : files) {
			const fs::path out = dest / f.path;
			fs::create_directories(out.parent_path(), ec);
			std::ofstream o(out, std::ios::binary | std::ios::trunc);
			if (!o || !o.write(reinterpret_cast<const char*>(f.data.data()), std::streamsize(f.data.size())))
				throw std::runtime_error(tr("no se puede escribir ") + out.string());
		}
		installed = dest.string();
		return true;
	} catch (const std::exception& e) {
		error = e.what();
		return false;
	}
}

} // namespace nectar
