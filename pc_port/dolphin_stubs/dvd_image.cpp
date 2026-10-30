/**
 * @file dvd_image.cpp
 * @brief GameCube disc image reader: raw .iso/.gcm, Dolphin CISO, WIA and RVZ.
 */
#include "dvd_image.h"
#include "sysNew.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/types.h>
#else
#include <mutex>
#endif

#if defined(NECTAR_IMAGE_ZSTD)
#include <zstd.h>
#endif
#if defined(NECTAR_IMAGE_LZMA)
#include <lzma.h>
#endif
#if defined(NECTAR_IMAGE_BZIP2)
#include <bzlib.h>
#endif

namespace {

constexpr uint32_t kHeaderSize     = 0x440;
constexpr uint32_t kFstOffsetField = 0x424;
constexpr uint32_t kFstSizeField   = 0x428;
constexpr uint32_t kMagicField     = 0x01C;
constexpr uint32_t kGameCubeMagic  = 0xC2339F3Du;
constexpr uint32_t kWiiMagicField  = 0x018;
constexpr uint32_t kWiiMagic       = 0x5D1C9EA3u;
// FST size limit.
constexpr uint32_t kMaxFstSize     = 64u * 1024u * 1024u;

struct Entry {
    bool     directory;
    uint32_t offsetOrParent;
    uint32_t sizeOrNext;
    std::string path; // full disc path, original case, no leading slash
};

/* backing file */

#ifdef __vita__
SceUID sFd = -1;

bool backendOpen(const char* path, uint64_t* size, std::string& error)
{
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    const int sr = sceIoGetstat(path, &st);
    if (sr < 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "cannot stat the image (0x%08X)", (unsigned)sr);
        error = buf;
        return false;
    }
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "cannot open the image (0x%08X)", (unsigned)fd);
        error = buf;
        return false;
    }
    sFd = fd;
    *size = (uint64_t)st.st_size;
    return true;
}

void backendClose()
{
    if (sFd >= 0) sceIoClose(sFd);
    sFd = -1;
}

// Positional read; safe from any thread. Returns bytes read or -1.
int64_t backendRead(uint64_t offset, void* dst, size_t len)
{
    uint8_t* out = static_cast<uint8_t*>(dst);
    size_t done = 0;
    while (done < len) {
        const int r = sceIoPread(sFd, out + done, (SceSize)(len - done), (SceOff)(offset + done));
        if (r < 0) return -1;
        if (r == 0) break;
        done += (size_t)r;
    }
    return (int64_t)done;
}

// Container state lock.
SceKernelLwMutexWork sLockWork __attribute__((aligned(8)));
bool sLockReady = false;
void lockInit()   { if (!sLockReady) { sceKernelCreateLwMutex(&sLockWork, "nectar_image", 0, 0, nullptr); sLockReady = true; } }
void lockTake()   { sceKernelLockLwMutex(&sLockWork, 1, nullptr); }
void lockGive()   { sceKernelUnlockLwMutex(&sLockWork, 1); }
#else
FILE* sFile = nullptr;
std::mutex sFileMutex;
std::mutex sContainerMutex;

#if defined(_WIN32)
int seek64(FILE* f, uint64_t off) { return _fseeki64(f, (long long)off, SEEK_SET); }
int64_t tell64(FILE* f) { return (int64_t)_ftelli64(f); }
int seekEnd(FILE* f) { return _fseeki64(f, 0, SEEK_END); }
#else
int seek64(FILE* f, uint64_t off) { return fseeko(f, (off_t)off, SEEK_SET); }
int64_t tell64(FILE* f) { return (int64_t)ftello(f); }
int seekEnd(FILE* f) { return fseeko(f, 0, SEEK_END); }
#endif

bool backendOpen(const char* path, uint64_t* size, std::string& error)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        error = "cannot open the image";
        return false;
    }
    if (seekEnd(f) != 0) {
        fclose(f);
        error = "cannot determine the image size";
        return false;
    }
    const int64_t end = tell64(f);
    if (end < 0) {
        fclose(f);
        error = "cannot determine the image size";
        return false;
    }
    sFile = f;
    *size = (uint64_t)end;
    return true;
}

void backendClose()
{
    if (sFile) fclose(sFile);
    sFile = nullptr;
}

int64_t backendRead(uint64_t offset, void* dst, size_t len)
{
    std::lock_guard<std::mutex> lock(sFileMutex);
    if (seek64(sFile, offset) != 0) return -1;
    const size_t got = fread(dst, 1, len, sFile);
    if (got < len && ferror(sFile)) {
        clearerr(sFile);
        return -1;
    }
    return (int64_t)got;
}

void lockInit() {}
void lockTake() { sContainerMutex.lock(); }
void lockGive() { sContainerMutex.unlock(); }
#endif

struct ScopedLock {
    ScopedLock() { lockTake(); }
    ~ScopedLock() { lockGive(); }
};

/* mounted state (immutable after mount) */

bool     sMounted = false;
uint64_t sImageSize = 0;                      // size of the DISC the image represents
uint64_t sFileSize = 0;                       // size of the file on disk
std::string sImagePath;
std::vector<Entry> sEntries;
std::unordered_map<std::string, int> sLookup; // entry index by folded path
uint8_t sHeader32[32];                        // == DVDDiskID
char    sGameId[7];

uint32_t be32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint64_t be64(const uint8_t* p)
{
    return (uint64_t(be32(p)) << 32) | uint64_t(be32(p + 4));
}
uint32_t le32(const uint8_t* p)
{
    return (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[0]);
}

std::string describeBytes(uint64_t n)
{
    char buf[48];
    if (n >= 1024ull * 1024ull) snprintf(buf, sizeof(buf), "%u MB", (unsigned)(n / (1024ull * 1024ull)));
    else snprintf(buf, sizeof(buf), "%u bytes", (unsigned)n);
    return buf;
}

/* containers */

enum class Container { Raw, Ciso, Wia };
Container sContainer = Container::Raw;
char sContainerName[48] = "raw";

/* CISO */

struct CisoState {
    uint32_t blockSize = 0;
    uint64_t logicalSize = 0;
    std::vector<uint16_t> index;   // stored block number per block, 0xFFFF = absent
};
CisoState sCiso;
constexpr uint32_t kCisoHeader = 0x8000;
constexpr uint32_t kCisoMap    = kCisoHeader - 8;
constexpr uint16_t kCisoAbsent = 0xFFFF;

bool cisoOpen(std::string& error)
{
    std::vector<uint8_t> header(kCisoHeader);
    if (sFileSize < kCisoHeader || backendRead(0, header.data(), kCisoHeader) != (int64_t)kCisoHeader) {
        error = "CISO: the file is too small to hold a CISO header (truncated download?)";
        return false;
    }
    const uint32_t blockSize = le32(header.data() + 4);
    // PSP CISO: same magic, 0x18-byte header size field.
    if (blockSize == 0x18) {
        error = "CISO: this is a PSP-style CISO (compressed blocks), which is not supported; use Dolphin's CISO, RVZ or a plain .iso";
        return false;
    }
    if (blockSize < 0x8000 || blockSize > (64u << 20) || (blockSize & (blockSize - 1)) != 0) {
        error = "CISO: invalid block size " + describeBytes(blockSize);
        return false;
    }
    sCiso.blockSize = blockSize;
    sCiso.index.assign(kCisoMap, kCisoAbsent);
    uint32_t stored = 0, last = 0;
    bool any = false;
    for (uint32_t b = 0; b < kCisoMap; ++b) {
        const uint8_t m = header[8 + b];
        if (m == 1) { sCiso.index[b] = (uint16_t)stored++; last = b; any = true; }
        else if (m != 0) { error = "CISO: the block map is corrupt"; return false; }
    }
    if (!any) { error = "CISO: the block map is empty"; return false; }
    sCiso.logicalSize = (uint64_t)(last + 1) * blockSize;
    const uint64_t needed = (uint64_t)kCisoHeader + (uint64_t)stored * blockSize;
    if (sFileSize < needed) {
        error = "CISO: truncated image: the block map promises " + describeBytes(needed) + ", the file has "
              + describeBytes(sFileSize);
        return false;
    }
    snprintf(sContainerName, sizeof(sContainerName), "CISO, %u KB blocks", (unsigned)(blockSize / 1024));
    return true;
}

int64_t cisoRead(uint64_t offset, uint8_t* out, size_t len)
{
    if (offset >= sCiso.logicalSize) return 0;
    if (len > sCiso.logicalSize - offset) len = (size_t)(sCiso.logicalSize - offset);
    size_t done = 0;
    while (done < len) {
        const uint64_t pos = offset + done;
        const uint64_t block = pos / sCiso.blockSize;
        const uint32_t inBlock = (uint32_t)(pos % sCiso.blockSize);
        const size_t n = std::min<size_t>(sCiso.blockSize - inBlock, len - done);
        const uint16_t stored = block < kCisoMap ? sCiso.index[(size_t)block] : kCisoAbsent;
        if (stored == kCisoAbsent) {
            memset(out + done, 0, n);
        } else {
            const uint64_t fileOff = (uint64_t)kCisoHeader + (uint64_t)stored * sCiso.blockSize + inBlock;
            if (backendRead(fileOff, out + done, n) != (int64_t)n) return -1;
        }
        done += n;
    }
    return (int64_t)done;
}

/* GameCube padding generator (RVZ packing): lagged Fibonacci, j = 32, k = 521. */
struct JunkGenerator {
    uint32_t buf[521];
    uint32_t pos = 0;   // byte position inside the 521-word window

    void seed(const uint8_t* s)
    {
        for (int i = 0; i < 17; ++i) buf[i] = be32(s + 4 * i);
        for (int i = 17; i < 521; ++i) buf[i] = (buf[i - 17] << 23) ^ (buf[i - 16] >> 9) ^ buf[i - 1];
        for (int i = 0; i < 4; ++i) advance();
        pos = 0;
    }
    void advance()
    {
        for (int i = 0; i < 32; ++i) buf[i] ^= buf[i + 521 - 32];
        for (int i = 32; i < 521; ++i) buf[i] ^= buf[i - 32];
    }
    void skip(uint64_t bytes)
    {
        uint64_t p = (uint64_t)pos + bytes;
        while (p >= 521u * 4u) { advance(); p -= 521u * 4u; }
        pos = (uint32_t)p;
    }
    static uint8_t byteOf(uint32_t w, uint32_t k)
    {
        switch (k) {
        case 0:  return (uint8_t)(w >> 24);
        case 1:  return (uint8_t)(w >> 18);
        case 2:  return (uint8_t)(w >> 8);
        default: return (uint8_t)w;
        }
    }
    void generate(uint32_t n, uint8_t* out)
    {
        while (n) {
            if ((pos & 3) == 0 && n >= 4) {
                const uint32_t w = buf[pos >> 2];
                out[0] = (uint8_t)(w >> 24); out[1] = (uint8_t)(w >> 18);
                out[2] = (uint8_t)(w >> 8);  out[3] = (uint8_t)w;
                out += 4; n -= 4; pos += 4;
            } else {
                *out++ = byteOf(buf[pos >> 2], pos & 3);
                ++pos; --n;
            }
            if (pos == 521u * 4u) { advance(); pos = 0; }
        }
    }
};

/* WIA / RVZ */

enum : uint32_t { kCompNone = 0, kCompPurge = 1, kCompBzip2 = 2, kCompLzma = 3, kCompLzma2 = 4, kCompZstd = 5 };
constexpr uint32_t kWiaSector = 0x8000;
constexpr uint64_t kChunkCacheBytes = 4ull << 20;   // decompressed chunks kept around
#ifdef __vita__
constexpr uint32_t kMaxChunk = 8u << 20;
constexpr uint32_t kMaxLzmaDict = 8u << 20;
#else
constexpr uint32_t kMaxChunk = 64u << 20;
constexpr uint32_t kMaxLzmaDict = 256u << 20;
#endif

struct WiaRaw { uint64_t start, size; uint32_t group, nGroups; };
struct WiaGroup { uint32_t off4, size, packed; };
struct WiaSlot { uint32_t group = 0xFFFFFFFFu; uint32_t age = 0; std::vector<uint8_t> data; };

struct WiaState {
    bool     rvz = false;
    uint32_t compression = kCompNone;
    uint32_t chunkSize = 0;
    uint64_t isoSize = 0;
    uint8_t  discHeader[0x80];
    uint8_t  comprData[7];
    uint8_t  comprDataLen = 0;
    std::vector<WiaRaw> raw;
    std::vector<WiaGroup> groups;
    std::vector<WiaSlot> slots;
    std::vector<uint8_t> inBuf, packedBuf;
    uint32_t tick = 0;
    bool     gapReported = false;
    bool     failReported = false;
#if defined(NECTAR_IMAGE_ZSTD)
    ZSTD_DCtx* zstd = nullptr;
#endif
};
WiaState sWia;

void wiaClose()
{
#if defined(NECTAR_IMAGE_ZSTD)
    if (sWia.zstd) ZSTD_freeDCtx(sWia.zstd);
    sWia.zstd = nullptr;
#endif
    sWia.raw.clear(); sWia.groups.clear(); sWia.slots.clear();
    sWia.inBuf.clear(); sWia.inBuf.shrink_to_fit();
    sWia.packedBuf.clear(); sWia.packedBuf.shrink_to_fit();
    sWia.isoSize = 0; sWia.chunkSize = 0;
}

const char* compressionName(uint32_t c)
{
    switch (c) {
    case kCompNone:  return "uncompressed";
    case kCompPurge: return "purge";
    case kCompBzip2: return "bzip2";
    case kCompLzma:  return "LZMA";
    case kCompLzma2: return "LZMA2";
    case kCompZstd:  return "zstd";
    default:         return "unknown";
    }
}

bool compressionSupported(uint32_t c)
{
    switch (c) {
    case kCompBzip2:
#if defined(NECTAR_IMAGE_BZIP2)
        return true;
#else
        return false;
#endif
    case kCompLzma:
    case kCompLzma2:
#if defined(NECTAR_IMAGE_LZMA)
        return true;
#else
        return false;
#endif
    case kCompZstd:
#if defined(NECTAR_IMAGE_ZSTD)
        return true;
#else
        return false;
#endif
    default:
        return c == kCompNone || c == kCompPurge;
    }
}

// One compressed blob, decompressed to exactly outLen bytes.
bool wiaDecompress(uint32_t method, const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen, std::string& error)
{
    switch (method) {
    case kCompNone:
        if (inLen < outLen) { error = "stored data is shorter than its declared size"; return false; }
        memcpy(out, in, outLen);
        return true;
    case kCompPurge: {
        // Segments of non-zero data, then a SHA-1 of the segments (not checked).
        if (inLen < 20) { error = "purge data too small"; return false; }
        memset(out, 0, outLen);
        const size_t end = inLen - 20;
        size_t ip = 0;
        while (ip + 8 <= end) {
            const uint32_t off = be32(in + ip), sz = be32(in + ip + 4);
            ip += 8;
            if (off > outLen || sz > outLen - off || sz > end - ip) { error = "purge segment out of range"; return false; }
            memcpy(out + off, in + ip, sz);
            ip += sz;
        }
        return true;
    }
    case kCompBzip2: {
#if defined(NECTAR_IMAGE_BZIP2)
        unsigned int dest = (unsigned int)outLen;
        const int r = BZ2_bzBuffToBuffDecompress(reinterpret_cast<char*>(out), &dest,
                                                 const_cast<char*>(reinterpret_cast<const char*>(in)),
                                                 (unsigned int)inLen, 0, 0);
        if (r != BZ_OK) { error = "bzip2 decompression failed"; return false; }
        if (dest != outLen) { error = "bzip2 data has the wrong size"; return false; }
        return true;
#else
        error = "bzip2 compression is not supported by this build; re-convert to RVZ with zstd";
        return false;
#endif
    }
    case kCompLzma:
    case kCompLzma2: {
#if defined(NECTAR_IMAGE_LZMA)
        lzma_options_lzma opts;
        memset(&opts, 0, sizeof(opts));
        if (method == kCompLzma) {
            if (sWia.comprDataLen != 5 || sWia.comprData[0] >= 9 * 5 * 5) { error = "LZMA parameters are invalid"; return false; }
            const uint8_t d = sWia.comprData[0];
            opts.lc = d % 9;
            const uint8_t e = d / 9;
            opts.pb = e / 5;
            opts.lp = e % 5;
            opts.dict_size = le32(sWia.comprData + 1);
        } else {
            if (sWia.comprDataLen != 1 || sWia.comprData[0] > 40) { error = "LZMA2 parameters are invalid"; return false; }
            const uint8_t p = sWia.comprData[0];
            opts.dict_size = p == 40 ? 0xFFFFFFFFu : ((uint32_t)(2 | (p & 1)) << (p / 2 + 11));
        }
        if (opts.dict_size > kMaxLzmaDict) {
            error = "the LZMA dictionary (" + describeBytes(opts.dict_size) + ") is too large for this device; re-convert to RVZ with zstd";
            return false;
        }
        lzma_filter filters[2];
        filters[0].id = method == kCompLzma ? LZMA_FILTER_LZMA1 : LZMA_FILTER_LZMA2;
        filters[0].options = &opts;
        filters[1].id = LZMA_VLI_UNKNOWN;
        filters[1].options = nullptr;
        lzma_stream strm = LZMA_STREAM_INIT;
        if (lzma_raw_decoder(&strm, filters) != LZMA_OK) { error = "LZMA decoder setup failed"; return false; }
        strm.next_in = in; strm.avail_in = inLen;
        strm.next_out = out; strm.avail_out = outLen;
        const lzma_ret r = lzma_code(&strm, LZMA_FINISH);
        const bool ok = (r == LZMA_STREAM_END || r == LZMA_OK) && strm.avail_out == 0;
        lzma_end(&strm);
        if (!ok) { error = "LZMA decompression failed"; return false; }
        return true;
#else
        error = "LZMA compression is not supported by this build; re-convert to RVZ with zstd";
        return false;
#endif
    }
    case kCompZstd: {
#if defined(NECTAR_IMAGE_ZSTD)
        if (!sWia.zstd) sWia.zstd = ZSTD_createDCtx();
        if (!sWia.zstd) { error = "out of memory (zstd)"; return false; }
        const size_t r = ZSTD_decompressDCtx(sWia.zstd, out, outLen, in, inLen);
        if (ZSTD_isError(r)) { error = std::string("zstd: ") + ZSTD_getErrorName(r); return false; }
        if (r != outLen) { error = "zstd data has the wrong size"; return false; }
        return true;
#else
        error = "zstd compression is not supported by this build";
        return false;
#endif
    }
    default:
        error = "unknown compression method";
        return false;
    }
}

// RVZ packing: literal runs and regenerated padding runs.
bool rvzUnpack(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen, uint64_t discOffset, std::string& error)
{
    JunkGenerator gen;
    size_t ip = 0, op = 0;
    while (op < outLen) {
        if (ip + 4 > inLen) { error = "packed data ends early"; return false; }
        uint32_t size = be32(in + ip);
        ip += 4;
        const bool junk = (size & 0x80000000u) != 0;
        size &= 0x7FFFFFFFu;
        if (size > outLen - op) { error = "packed run overruns the chunk"; return false; }
        if (junk) {
            if (ip + 68 > inLen) { error = "packed data ends inside a seed"; return false; }
            gen.seed(in + ip);
            ip += 68;
            gen.skip(discOffset % kWiaSector);
            gen.generate(size, out + op);
        } else {
            if (ip + size > inLen) { error = "packed data ends inside a run"; return false; }
            memcpy(out + op, in + ip, size);
            ip += size;
        }
        op += size;
        discOffset += size;
    }
    return true;
}

bool wiaReadTable(uint64_t fileOff, uint32_t compressedSize, std::vector<uint8_t>& out, size_t outLen, std::string& error)
{
    if (compressedSize == 0 || fileOff > sFileSize || compressedSize > sFileSize - fileOff) {
        error = "a table lies beyond the end of the file (truncated download?)";
        return false;
    }
    std::vector<uint8_t> in(compressedSize);
    if (backendRead(fileOff, in.data(), compressedSize) != (int64_t)compressedSize) { error = "could not read a table"; return false; }
    out.resize(outLen);
    if (outLen == 0) return true;
    return wiaDecompress(sWia.compression, in.data(), compressedSize, out.data(), outLen, error);
}

bool wiaOpen(bool rvz, std::string& error)
{
    const char* fmt = rvz ? "RVZ" : "WIA";
    auto fail = [&](const std::string& what) { error = std::string(fmt) + ": " + what; wiaClose(); return false; };

    uint8_t h1[0x48];
    if (sFileSize < sizeof(h1) || backendRead(0, h1, sizeof(h1)) != (int64_t)sizeof(h1)) return fail("the file is too small");
    const uint32_t version = be32(h1 + 0x04), versionCompatible = be32(h1 + 0x08), header2Size = be32(h1 + 0x0C);
    const uint64_t isoSize = be64(h1 + 0x24), fileSize = be64(h1 + 0x2C);
    const uint32_t ourVersion = 0x01000000u, ourReadCompatible = rvz ? 0x00030000u : 0x00080000u;
    if (ourVersion < versionCompatible || ourReadCompatible > version) {
        char buf[96];
        snprintf(buf, sizeof(buf), "unsupported format version %u.%02u", (unsigned)(version >> 24), (unsigned)((version >> 16) & 0xFF));
        return fail(buf);
    }
    if (fileSize != sFileSize) {
        return fail("the file size does not match its header (" + describeBytes(sFileSize) + " on disk, " + describeBytes(fileSize)
                    + " expected): truncated or corrupted download");
    }
    if (header2Size < 0xD5 || header2Size > 0x1000) return fail("the second header has an invalid size");
    std::vector<uint8_t> h2(header2Size);
    if (backendRead(0x48, h2.data(), header2Size) != (int64_t)header2Size) return fail("could not read the second header");

    const uint32_t discType = be32(h2.data() + 0x00);
    const uint32_t compression = be32(h2.data() + 0x04);
    const uint32_t chunkSize = be32(h2.data() + 0x0C);
    if (discType == 2) return fail("this is a Wii disc image, not a GameCube one");
    if (discType != 1) return fail("unknown disc type");
    if (compression > kCompZstd || (rvz && compression == kCompPurge) || (!rvz && compression == kCompZstd))
        return fail("unsupported compression method");
    if (!compressionSupported(compression))
        return fail(std::string(compressionName(compression)) + " compression is not supported by this build; re-convert to RVZ with zstd");
    const bool powerOfTwo = chunkSize != 0 && (chunkSize & (chunkSize - 1)) == 0;
    const bool multipleOf2M = chunkSize != 0 && chunkSize % 0x200000u == 0;
    if (rvz ? !((chunkSize >= kWiaSector && powerOfTwo) || multipleOf2M) : !multipleOf2M)
        return fail("invalid chunk size");
    if (chunkSize > kMaxChunk)
        return fail("the chunk size (" + describeBytes(chunkSize) + ") is too large for this device; re-convert with a chunk size of 2 MB or less");

    sWia.rvz = rvz;
    sWia.compression = compression;
    sWia.chunkSize = chunkSize;
    sWia.isoSize = isoSize;
    memcpy(sWia.discHeader, h2.data() + 0x10, 0x80);
    sWia.comprDataLen = h2[0xD4];
    if (sWia.comprDataLen > 7 || header2Size < 0xD5u + sWia.comprDataLen) return fail("invalid compressor data");
    memset(sWia.comprData, 0, sizeof(sWia.comprData));
    memcpy(sWia.comprData, h2.data() + 0xD5, sWia.comprDataLen);

    const uint32_t nPart = be32(h2.data() + 0x90);
    if (nPart != 0) return fail("the image has Wii partitions");
    const uint32_t nRaw = be32(h2.data() + 0xB4);
    const uint64_t rawOff = be64(h2.data() + 0xB8);
    const uint32_t rawSize = be32(h2.data() + 0xC0);
    const uint32_t nGroups = be32(h2.data() + 0xC4);
    const uint64_t groupOff = be64(h2.data() + 0xC8);
    const uint32_t groupSize = be32(h2.data() + 0xD0);
    if (nRaw == 0 || nRaw > 65536 || nGroups == 0 || nGroups > (1u << 22)) return fail("invalid table sizes");

    std::vector<uint8_t> table;
    if (!wiaReadTable(rawOff, rawSize, table, (size_t)nRaw * 0x18, error)) return fail(error);
    sWia.raw.resize(nRaw);
    for (uint32_t i = 0; i < nRaw; ++i) {
        const uint8_t* e = table.data() + (size_t)i * 0x18;
        WiaRaw& r = sWia.raw[i];
        uint64_t off = be64(e), size = be64(e + 8);
        const uint64_t skipped = off % kWiaSector;   // groups start at the previous 32 KiB boundary
        off -= skipped;
        size += skipped;
        r.start = off; r.size = size;
        r.group = be32(e + 16); r.nGroups = be32(e + 20);
        if (r.group > nGroups || r.nGroups > nGroups - r.group) return fail("a raw data entry points outside the group table");
        if (size != 0 && (size + chunkSize - 1) / chunkSize != r.nGroups) return fail("a raw data entry's group count does not match its size");
    }
    std::sort(sWia.raw.begin(), sWia.raw.end(), [](const WiaRaw& a, const WiaRaw& b) { return a.start < b.start; });

    const size_t groupEntry = rvz ? 12 : 8;
    if (!wiaReadTable(groupOff, groupSize, table, (size_t)nGroups * groupEntry, error)) return fail(error);
    sWia.groups.resize(nGroups);
    for (uint32_t i = 0; i < nGroups; ++i) {
        const uint8_t* e = table.data() + (size_t)i * groupEntry;
        sWia.groups[i].off4 = be32(e);
        sWia.groups[i].size = be32(e + 4);
        sWia.groups[i].packed = rvz ? be32(e + 8) : 0;
    }
    table.clear(); table.shrink_to_fit();

    size_t slots = (size_t)(kChunkCacheBytes / chunkSize);
    slots = std::max<size_t>(2, std::min<size_t>(slots, 32));
    sWia.slots.assign(slots, WiaSlot());
    sWia.tick = 0;
    sWia.gapReported = sWia.failReported = false;
    snprintf(sContainerName, sizeof(sContainerName), "%s %s, %u KB chunks", fmt, compressionName(compression), (unsigned)(chunkSize / 1024));
    return true;
}

// Decompresses group g (disc offset groupStart, groupLen bytes) into out.
bool wiaLoadGroup(uint32_t g, uint64_t groupStart, uint32_t groupLen, uint8_t* out, std::string& error)
{
    const WiaGroup& ge = sWia.groups[g];
    uint32_t dataSize = ge.size;
    bool discMethod = true;
    if (sWia.rvz) {
        discMethod = (dataSize & 0x80000000u) != 0;
        dataSize &= 0x7FFFFFFFu;
    }
    if (dataSize == 0) {
        memset(out, 0, groupLen);
        return true;
    }
    const uint64_t fileOff = (uint64_t)ge.off4 << 2;
    if (fileOff > sFileSize || dataSize > sFileSize - fileOff) {
        error = "a chunk lies beyond the end of the file (truncated download?)";
        return false;
    }
    if (sWia.inBuf.size() < dataSize) sWia.inBuf.resize(dataSize);
    if (backendRead(fileOff, sWia.inBuf.data(), dataSize) != (int64_t)dataSize) { error = "could not read a chunk"; return false; }
    const uint32_t method = discMethod ? sWia.compression : kCompNone;
    const uint32_t packed = sWia.rvz ? ge.packed : 0;
    if (packed) {
        if (sWia.packedBuf.size() < packed) sWia.packedBuf.resize(packed);
        if (!wiaDecompress(method, sWia.inBuf.data(), dataSize, sWia.packedBuf.data(), packed, error)) return false;
        return rvzUnpack(sWia.packedBuf.data(), packed, out, groupLen, groupStart, error);
    }
    return wiaDecompress(method, sWia.inBuf.data(), dataSize, out, groupLen, error);
}

const uint8_t* wiaGroup(uint32_t g, uint64_t groupStart, uint32_t groupLen)
{
    for (WiaSlot& s : sWia.slots) {
        if (s.group == g) { s.age = ++sWia.tick; return s.data.data(); }
    }
    WiaSlot* victim = &sWia.slots[0];
    for (WiaSlot& s : sWia.slots) {
        if (s.group == 0xFFFFFFFFu) { victim = &s; break; }
        if (s.age < victim->age) victim = &s;
    }
    victim->group = 0xFFFFFFFFu;
    if (victim->data.size() < sWia.chunkSize) victim->data.resize(sWia.chunkSize);
    std::string error;
    if (!wiaLoadGroup(g, groupStart, groupLen, victim->data.data(), error)) {
        if (!sWia.failReported) {
            sWia.failReported = true;
            printf("[PC Port] Disc image: chunk %u at 0x%llX could not be decoded: %s\n", (unsigned)g,
                   (unsigned long long)groupStart, error.c_str());
        }
        return nullptr;
    }
    victim->group = g;
    victim->age = ++sWia.tick;
    return victim->data.data();
}

int64_t wiaRead(uint64_t offset, uint8_t* out, size_t len)
{
    PikiPcPersistentAlloc pcKeep;   // port state outlives the level heaps (sysNew.cpp)
    if (offset >= sWia.isoSize) return 0;
    if (len > sWia.isoSize - offset) len = (size_t)(sWia.isoSize - offset);
    ScopedLock lock;
    size_t done = 0;
    while (done < len) {
        const uint64_t pos = offset + done;
        if (pos < 0x80) {
            const size_t n = std::min<size_t>((size_t)(0x80 - pos), len - done);
            memcpy(out + done, sWia.discHeader + pos, n);
            done += n;
            continue;
        }
        const WiaRaw* entry = nullptr;
        uint64_t next = sWia.isoSize;
        for (const WiaRaw& r : sWia.raw) {
            if (pos >= r.start && pos < r.start + r.size) { entry = &r; break; }
            if (r.start > pos && r.start < next) next = r.start;
        }
        if (!entry) {
            // Not covered by any data entry: zeros.
            if (!sWia.gapReported) {
                sWia.gapReported = true;
                printf("[PC Port] Disc image: offset 0x%llX is not stored in the image; reading zeros\n", (unsigned long long)pos);
            }
            const size_t n = (size_t)std::min<uint64_t>(next - pos, (uint64_t)(len - done));
            memset(out + done, 0, n);
            done += n;
            continue;
        }
        const uint64_t rel = pos - entry->start;
        const uint32_t gi = (uint32_t)(rel / sWia.chunkSize);
        if (gi >= entry->nGroups) return -1;
        const uint64_t groupStart = entry->start + (uint64_t)gi * sWia.chunkSize;
        const uint32_t groupLen = (uint32_t)std::min<uint64_t>(sWia.chunkSize, entry->size - (uint64_t)gi * sWia.chunkSize);
        const uint8_t* data = wiaGroup(entry->group + gi, groupStart, groupLen);
        if (!data) return -1;
        const uint32_t inGroup = (uint32_t)(pos - groupStart);
        if (inGroup >= groupLen) return -1;
        const size_t n = std::min<size_t>(groupLen - inGroup, len - done);
        memcpy(out + done, data + inGroup, n);
        done += n;
    }
    return (int64_t)done;
}

/* dispatcher: disc read, short at the end of the disc, -1 on error */

int64_t discRead(uint64_t offset, void* dst, size_t len)
{
    switch (sContainer) {
    case Container::Ciso: return cisoRead(offset, static_cast<uint8_t*>(dst), len);
    case Container::Wia:  return wiaRead(offset, static_cast<uint8_t*>(dst), len);
    default:              return backendRead(offset, dst, len);
    }
}

void containerClose()
{
    if (sContainer == Container::Wia) wiaClose();
    sCiso = CisoState();
    sContainer = Container::Raw;
    snprintf(sContainerName, sizeof(sContainerName), "raw");
}

/* paths and state */

// Lookup key: folded path.
void foldPath(const char* in, std::string& out)
{
    out.clear();
    if (!in) return;
    std::string component;
    auto flush = [&]() {
        if (component == "..") {
            // ".." pops the previous component.
            const size_t slash = out.rfind('/');
            out.erase(slash == std::string::npos ? 0 : slash);
        } else if (!component.empty() && component != ".") {
            if (!out.empty()) out += '/';
            out += component;
        }
        component.clear();
    };
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(in); *p; ++p) {
        unsigned char c = *p;
        if (c == '/' || c == '\\') { flush(); continue; }
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        component += (char)c;
    }
    flush();
}

void resetState()
{
    sMounted = false;
    sImageSize = 0;
    sFileSize = 0;
    sImagePath.clear();
    sEntries.clear();
    sLookup.clear();
    memset(sHeader32, 0, sizeof(sHeader32));
    memset(sGameId, 0, sizeof(sGameId));
}

// Recognised but unsupported formats.
bool detectUnsupportedContainer(const uint8_t* h, std::string& error)
{
    static const uint8_t kGcz[4] = { 0x01, 0xC0, 0x0B, 0xB1 };   // 0xB10BC001 LE
    static const uint8_t k7z[6]  = { '7', 'z', 0xBC, 0xAF, 0x27, 0x1C };
    static const uint8_t kRar[4] = { 'R', 'a', 'r', '!' };
    const char* format = nullptr;
    if (memcmp(h, kGcz, 4) == 0) format = "GCZ";
    else if (memcmp(h + 0x200, "NKIT", 4) == 0) format = "NKit";
    if (format) {
        error = std::string(format) + " images are not supported: convert to RVZ, CISO or a plain .iso/.gcm";
        return true;
    }
    const char* archive = nullptr;
    if (memcmp(h, k7z, 6) == 0) archive = "7-Zip";
    else if (memcmp(h, "PK\x03\x04", 4) == 0) archive = "ZIP";
    else if (memcmp(h, kRar, 4) == 0) archive = "RAR";
    else if (h[0] == 0x1F && h[1] == 0x8B) archive = "gzip";
    if (archive) {
        error = std::string("this is a ") + archive + " archive, not a disc image: extract the image from it first";
        return true;
    }
    return false;
}

bool parseFst(const std::vector<uint8_t>& fst, std::string& error)
{
    if (fst.size() < 12) {
        error = "the file table (FST) is too small";
        return false;
    }
    const uint32_t rootWord = be32(fst.data());
    const uint32_t entryCount = be32(fst.data() + 8);
    if ((rootWord >> 24) != 1 || entryCount == 0 || (uint64_t)entryCount * 12ull > fst.size()) {
        error = "the file table (FST) root entry is invalid";
        return false;
    }
    const size_t stringTable = (size_t)entryCount * 12;

    sEntries.resize(entryCount);
    sLookup.reserve(entryCount);

    // Directory nesting stack.
    struct Frame { uint32_t next; std::string prefix; };
    std::vector<Frame> stack;
    stack.push_back({ entryCount, std::string() });
    sEntries[0].directory = true;
    sEntries[0].offsetOrParent = 0;
    sEntries[0].sizeOrNext = entryCount;

    for (uint32_t i = 1; i < entryCount; ++i) {
        const uint8_t* raw = fst.data() + (size_t)i * 12;
        const uint32_t typeAndName = be32(raw);
        Entry& e = sEntries[i];
        e.directory = (typeAndName >> 24) != 0;
        e.offsetOrParent = be32(raw + 4);
        e.sizeOrNext = be32(raw + 8);

        while (stack.size() > 1 && i >= stack.back().next) stack.pop_back();

        const size_t nameBegin = stringTable + (typeAndName & 0x00FFFFFFu);
        if (nameBegin >= fst.size()) {
            error = "the file table (FST) string table is corrupt";
            return false;
        }
        const void* nul = memchr(fst.data() + nameBegin, 0, fst.size() - nameBegin);
        if (!nul) {
            error = "the file table (FST) string table is corrupt";
            return false;
        }
        const std::string name(reinterpret_cast<const char*>(fst.data() + nameBegin),
                               reinterpret_cast<const char*>(nul));
        e.path = stack.back().prefix + name;

        if (e.directory) {
            if (e.sizeOrNext <= i || e.sizeOrNext > entryCount) {
                error = "the file table (FST) contains an invalid directory: " + e.path;
                return false;
            }
            stack.push_back({ e.sizeOrNext, e.path + "/" });
        } else if (e.offsetOrParent > sImageSize || e.sizeOrNext > sImageSize - e.offsetOrParent) {
            // Truncated or scrubbed image.
            error = "truncated image: \"" + e.path + "\" lies beyond the end of the disc (needs "
                  + describeBytes((uint64_t)e.offsetOrParent + e.sizeOrNext) + ", the image holds "
                  + describeBytes(sImageSize) + ")";
            return false;
        }

        std::string key;
        foldPath(e.path.c_str(), key);
        sLookup.emplace(std::move(key), (int)i); // first occurrence wins
    }
    return true;
}

// Strips an optional "./" and the "assets/" prefix the desktop readers use.
const char* discPathOf(const char* path)
{
    const char* p = path;
    while (p[0] == '.' && (p[1] == '/' || p[1] == '\\')) p += 2;
    if ((p[0] == 'a' || p[0] == 'A') && (p[1] == 's' || p[1] == 'S') && (p[2] == 's' || p[2] == 'S')
        && (p[3] == 'e' || p[3] == 'E') && (p[4] == 't' || p[4] == 'T') && (p[5] == 's' || p[5] == 'S')
        && (p[6] == '/' || p[6] == '\\')) {
        p += 7;
    }
    return p;
}

// Bytes of file `idx` from `offset`, clamped to the file: no padding.
int64_t readClamped(int idx, void* dst, uint32_t offset, uint32_t len)
{
    uint32_t start = 0, size = 0;
    if (!dvd_image_entry(idx, &start, &size)) return -1;
    if (offset >= size) return 0;
    const uint32_t avail = size - offset;
    const uint32_t want = len < avail ? len : avail;
    if (want == 0) return 0;
    return discRead((uint64_t)start + offset, dst, want);
}

void failMount()
{
    containerClose();
    backendClose();
    resetState();
}

} // namespace

/* public API */

bool dvd_image_mount(const char* path, std::string& error)
{
    error.clear();
    dvd_image_unmount();
    if (!path || !*path) {
        error = "no image path";
        return false;
    }
    lockInit();

    uint64_t size = 0;
    if (!backendOpen(path, &size, error)) return false;
    sFileSize = size;
    sImageSize = size;

    // Container detection on the first bytes of the file.
    uint8_t probe[kHeaderSize];
    memset(probe, 0, sizeof(probe));
    const size_t probeLen = (size_t)std::min<uint64_t>(size, kHeaderSize);
    if (probeLen < 8 || backendRead(0, probe, probeLen) != (int64_t)probeLen) {
        failMount();
        error = "the file is too small to be a disc image";
        return false;
    }
    if (memcmp(probe, "CISO", 4) == 0) {
        sContainer = Container::Ciso;
        if (!cisoOpen(error)) { failMount(); return false; }
        sImageSize = sCiso.logicalSize;
    } else if (memcmp(probe, "WIA\x01", 4) == 0 || memcmp(probe, "RVZ\x01", 4) == 0) {
        sContainer = Container::Wia;
        if (!wiaOpen(probe[0] == 'R', error)) { failMount(); return false; }
        sImageSize = sWia.isoSize;
    } else if (detectUnsupportedContainer(probe, error)) {
        failMount();
        return false;
    }

    uint8_t header[kHeaderSize];
    if (sImageSize < kHeaderSize || discRead(0, header, kHeaderSize) != (int64_t)kHeaderSize) {
        failMount();
        error = "the image is too small to be a GameCube disc image";
        return false;
    }
    if (be32(header + kMagicField) != kGameCubeMagic) {
        failMount();
        error = (be32(header + kWiiMagicField) == kWiiMagic)
              ? "this is a Wii disc image, not a GameCube one"
              : "not a GameCube disc image (header magic mismatch)";
        return false;
    }

    const uint32_t fstOffset = be32(header + kFstOffsetField);
    const uint32_t fstSize   = be32(header + kFstSizeField);
    if (fstSize < 12 || fstSize > kMaxFstSize || fstOffset > sImageSize || fstSize > sImageSize - fstOffset) {
        const bool truncated = fstOffset > sImageSize || (fstSize <= kMaxFstSize && fstSize > sImageSize - fstOffset);
        failMount();
        error = truncated ? "truncated image: the file table (FST) lies beyond the end of the disc"
                          : "the file table (FST) header is invalid";
        return false;
    }
    std::vector<uint8_t> fst(fstSize);
    if (discRead(fstOffset, fst.data(), fstSize) != (int64_t)fstSize) {
        failMount();
        error = "could not read the file table (FST)";
        return false;
    }
    if (!parseFst(fst, error)) {
        failMount();
        return false;
    }

    memcpy(sHeader32, header, sizeof(sHeader32));
    memcpy(sGameId, header, 6);
    sGameId[6] = 0;
    sImagePath = path;
    sMounted = true;
    return true;
}

void dvd_image_unmount()
{
    if (!sMounted && sFileSize == 0) return;
    containerClose();
    backendClose();
    resetState();
}

bool dvd_image_mounted() { return sMounted; }
const char* dvd_image_path() { return sImagePath.c_str(); }
int dvd_image_entry_count() { return sMounted ? (int)sEntries.size() : 0; }
uint64_t dvd_image_disc_size() { return sMounted ? sImageSize : 0; }
const char* dvd_image_container() { return sMounted ? sContainerName : ""; }

int dvd_image_find(const char* discPath)
{
    if (!sMounted || !discPath) return -1;
    std::string key;
    foldPath(discPath, key);
    if (key.empty()) return 0; // the root directory
    const auto it = sLookup.find(key);
    return it == sLookup.end() ? -1 : it->second;
}

bool dvd_image_entry(int idx, uint32_t* offset, uint32_t* size)
{
    if (!sMounted || idx < 0 || idx >= (int)sEntries.size() || sEntries[idx].directory) return false;
    if (offset) *offset = sEntries[idx].offsetOrParent;
    if (size) *size = sEntries[idx].sizeOrNext;
    return true;
}

bool dvd_image_entry_is_dir(int idx)
{
    return sMounted && idx >= 0 && idx < (int)sEntries.size() && sEntries[idx].directory;
}

const char* dvd_image_entry_path(int idx)
{
    if (!sMounted || idx < 0 || idx >= (int)sEntries.size()) return nullptr;
    return sEntries[idx].path.c_str();
}

int32_t dvd_image_read_file(uint32_t fileStart, uint32_t fileLength, void* dst,
                            int32_t len, int32_t fileOffset)
{
    if (!sMounted || !dst || len < 0 || fileOffset < 0) return -1;
    if (fileStart > sImageSize || fileLength > sImageSize - fileStart) return -1;
    uint8_t* out = static_cast<uint8_t*>(dst);
    size_t got = 0;
    if ((uint32_t)fileOffset < fileLength) {
        const uint32_t avail = fileLength - (uint32_t)fileOffset;
        const uint32_t want = (uint32_t)len < avail ? (uint32_t)len : avail;
        if (want) {
            const int64_t r = discRead((uint64_t)fileStart + (uint32_t)fileOffset, out, want);
            if (r < 0) return -1;
            got = (size_t)r;
        }
    }
    // Bytes past the end of the file read as padding.
    if (got < (size_t)len) memset(out + got, 0, (size_t)len - got);
    return len;
}

int32_t dvd_image_read(int idx, void* dst, int32_t len, int32_t fileOffset)
{
    uint32_t start = 0, size = 0;
    if (!dvd_image_entry(idx, &start, &size)) return -1;
    return dvd_image_read_file(start, size, dst, len, fileOffset);
}

int64_t dvd_image_read_raw(uint64_t offset, void* dst, uint32_t len)
{
    if (!sMounted || (!dst && len)) return -1;
    if (offset >= sImageSize) return 0;
    if (len > sImageSize - offset) len = (uint32_t)(sImageSize - offset);
    if (len == 0) return 0;
    return discRead(offset, dst, len);
}

void dvd_image_junk_generate(const uint8_t seed[68], uint64_t offset, uint32_t count, uint8_t* out)
{
    JunkGenerator gen;
    gen.seed(seed);
    gen.skip(offset % kWiaSector);
    gen.generate(count, out);
}

void dvd_image_disk_id(DVDDiskID* id)
{
    if (!id) return;
    // DVDDiskID: the first 32 bytes of the disc header.
    memcpy(id, sHeader32, sizeof(sHeader32));
}

const char* dvd_image_game_id() { return sGameId; }
uint8_t dvd_image_revision() { return sHeader32[7]; }
uint8_t dvd_image_disc_number() { return sHeader32[6]; }

/* host-path routing helpers */

bool pc_disc_file_size(const char* path, uint32_t* size)
{
    if (!path || !size) return false;
    if (sMounted) {
        return dvd_image_entry(dvd_image_find(discPathOf(path)), nullptr, size);
    }
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    bool ok = false;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long n = ftell(f);
        if (n >= 0) { *size = (uint32_t)n; ok = true; }
    }
    fclose(f);
    return ok;
}

bool pc_disc_read_whole_file(const char* path, std::vector<uint8_t>& out)
{
    out.clear();
    if (!path) return false;
    if (sMounted) {
        const int idx = dvd_image_find(discPathOf(path));
        uint32_t size = 0;
        if (!dvd_image_entry(idx, nullptr, &size)) return false;
        out.resize(size);
        if (size == 0) return true;
        return readClamped(idx, out.data(), 0, size) == (int64_t)size;
    }
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    bool ok = false;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long n = ftell(f);
        if (n >= 0 && fseek(f, 0, SEEK_SET) == 0) {
            out.resize((size_t)n);
            ok = (n == 0) || fread(out.data(), 1, (size_t)n, f) == (size_t)n;
        }
    }
    fclose(f);
    if (!ok) out.clear();
    return ok;
}

int64_t pc_disc_read_range(const char* path, void* dst, uint32_t offset, uint32_t len)
{
    if (!path || (!dst && len)) return -1;
    if (sMounted) {
        return readClamped(dvd_image_find(discPathOf(path)), dst, offset, len);
    }
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    int64_t got = 0;
    if (fseek(f, (long)offset, SEEK_SET) == 0 && len) {
        got = (int64_t)fread(dst, 1, len, f);
    }
    fclose(f);
    return got;
}
