// GameCube memory-card API over Dolphin-format .gci files.
#include "Dolphin/card.h"
#include "Dolphin/dvd.h"

#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#define CARD_MKDIR(path) _mkdir(path)
#define CARD_RMDIR(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define CARD_MKDIR(path) mkdir((path), 0777)
#define CARD_RMDIR(path) rmdir(path)
#endif
#include <dirent.h>

namespace {
constexpr u32 kSectorSize   = 0x2000;
constexpr u32 kTotalBlocks  = 2048; // 16 MB card
constexpr u32 kUsableBlocks = kTotalBlocks - CARD_NUM_SYSTEM_BLOCK;
constexpr u32 kDEntrySize   = 0x40;
constexpr u32 kGciEpoch     = 946684800u; // 2000-01-01 as Unix time
constexpr int kHostNameMax  = 256;
constexpr int kPathMax      = 768;

// El hilo trabajador de la tarjeta (CardUtilMain) escribe estos resultados
// mientras el hilo principal los consulta en bucles de espera activa como
// MemoryCard::waitWhileBusy(), cuyo cuerpo esta vacio. Con tipos normales eso
// es una carrera de datos y, bajo LTO, un bucle infinito: el compilador saca la
// carga fuera del bucle. Atomicos relajados cuestan lo mismo en x86 y obligan a
// releer memoria en cada vuelta.
std::atomic<s32> sLastResult[2] = { CARD_RESULT_READY, CARD_RESULT_READY };
std::atomic<s32> sTransferred[2] = {};

struct Slot {
	bool used;
	char hostName[kHostNameMax]; // file name in the store directory
	CARDStat stat;
	u8 permission;
	u8 copyTimes;
	u16 firstBlock;
	u8* data;
	u32 dataLen;                 // whole sectors
};

struct Store {
	bool loaded;
	Slot slots[CARD_MAX_FILE];
};

Store sStore[2];

bool validChannel(s32 channel) { return channel >= 0 && channel < 2; }
const char* storeDir(s32 channel) { return channel == 0 ? "." : "cardB"; }
const char* legacyStoreDir(s32 channel) { return channel == 0 ? "save" : "save/slotB"; }
void adoptHostName(s32 channel, int fileNo);

u16 readBE16(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }
u32 readBE32(const u8* p) { return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | p[3]; }
void writeBE16(u8* p, u16 v) { p[0] = static_cast<u8>(v >> 8); p[1] = static_cast<u8>(v); }
void writeBE32(u8* p, u32 v) { p[0] = static_cast<u8>(v >> 24); p[1] = static_cast<u8>(v >> 16); p[2] = static_cast<u8>(v >> 8); p[3] = static_cast<u8>(v); }

u32 cardTimeNow()
{
	const time_t now = time(nullptr);
	return now > static_cast<time_t>(kGciEpoch) ? static_cast<u32>(now - kGciEpoch) : 0;
}

// Disc game code and company.
void diskGameCode(u8 gameName[4], u8 company[2])
{
	if (const DVDDiskID* disk = DVDGetCurrentDiskID()) {
		memcpy(gameName, disk->gameName, 4);
		memcpy(company, disk->company, 2);
	} else {
		memcpy(gameName, "GPIE", 4);
		memcpy(company, "01", 2);
	}
}

bool sameGame(const Slot& slot)
{
	u8 gameName[4], company[2];
	diskGameCode(gameName, company);
	return memcmp(slot.stat.gameName, gameName, 4) == 0 && memcmp(slot.stat.company, company, 2) == 0;
}

// Banner/icon offsets from the format fields (UpdateIconOffsets).
void deriveOffsets(CARDStat& stat)
{
	u32 offset     = stat.iconAddr;
	u8 bannerFmt   = stat.bannerFormat;
	u16 iconFormat = stat.iconFormat;
	u16 iconSpeed  = stat.iconSpeed;
	if (offset == 0xffffffffu) {
		bannerFmt = 0; iconFormat = 0; iconSpeed = 0; offset = 0;
	}
	switch (bannerFmt & CARD_STAT_BANNER_MASK) {
	case CARD_STAT_BANNER_C8:
		stat.offsetBanner = offset; offset += CARD_BANNER_WIDTH * CARD_BANNER_HEIGHT;
		stat.offsetBannerTlut = offset; offset += 2 * 256;
		break;
	case CARD_STAT_BANNER_RGB5A3:
		stat.offsetBanner = offset; offset += 2 * CARD_BANNER_WIDTH * CARD_BANNER_HEIGHT;
		stat.offsetBannerTlut = 0xffffffffu;
		break;
	default:
		stat.offsetBanner = stat.offsetBannerTlut = 0xffffffffu;
		break;
	}
	bool iconTlut = false;
	for (int i = 0; i < CARD_ICON_MAX; i++) {
		u32 format = (iconFormat >> (2 * i)) & CARD_STAT_ICON_MASK;
		if (((iconSpeed >> (2 * i)) & CARD_STAT_SPEED_MASK) == CARD_STAT_SPEED_END) format = CARD_STAT_ICON_NONE;
		switch (format) {
		case CARD_STAT_ICON_C8:
			stat.offsetIcon[i] = offset; offset += CARD_ICON_WIDTH * CARD_ICON_HEIGHT; iconTlut = true;
			break;
		case CARD_STAT_ICON_RGB5A3:
			stat.offsetIcon[i] = offset; offset += 2 * CARD_ICON_WIDTH * CARD_ICON_HEIGHT;
			break;
		default:
			stat.offsetIcon[i] = 0xffffffffu;
			break;
		}
	}
	if (iconTlut) { stat.offsetIconTlut = offset; offset += 2 * 256; }
	else stat.offsetIconTlut = 0xffffffffu;
	stat.offsetData = offset;
}

void slotPath(s32 channel, const Slot& slot, char* out, size_t size) { snprintf(out, size, "%s/%s", storeDir(channel), slot.hostName); }

bool fileExists(const char* path)
{
	FILE* file = fopen(path, "rb");
	if (!file) return false;
	fclose(file);
	return true;
}

bool isGciName(const char* name)
{
	const size_t length = strlen(name);
	if (length <= 4) return false;
	const char* ext = name + length - 4;
	return ext[0] == '.' && (ext[1] | 0x20) == 'g' && (ext[2] | 0x20) == 'c' && (ext[3] | 0x20) == 'i';
}

int compareNames(const void* a, const void* b) { return strcmp(static_cast<const char*>(a), static_cast<const char*>(b)); }

void buildDEntry(const Slot& slot, u8* d)
{
	const CARDStat& stat = slot.stat;
	memset(d, 0, kDEntrySize);
	memcpy(d + 0x00, stat.gameName, 4);
	memcpy(d + 0x04, stat.company, 2);
	d[0x06] = 0xFF;
	d[0x07] = stat.bannerFormat;
	memcpy(d + 0x08, stat.fileName, CARD_FILENAME_MAX);
	writeBE32(d + 0x28, stat.time);
	writeBE32(d + 0x2C, stat.iconAddr);
	writeBE16(d + 0x30, stat.iconFormat);
	writeBE16(d + 0x32, stat.iconSpeed);
	d[0x34] = slot.permission;
	d[0x35] = slot.copyTimes;
	writeBE16(d + 0x36, slot.firstBlock);
	writeBE16(d + 0x38, static_cast<u16>(slot.dataLen / kSectorSize));
	d[0x3A] = d[0x3B] = 0xFF;
	writeBE32(d + 0x3C, stat.commentAddr);
}

// Loads one .gci into a slot.
bool loadGci(s32 channel, Slot& slot, const char* hostName)
{
	char path[kPathMax];
	snprintf(path, sizeof path, "%s/%s", storeDir(channel), hostName);
	FILE* file = fopen(path, "rb");
	if (!file) return false;
	u8 d[kDEntrySize];
	bool ok          = fread(d, 1, kDEntrySize, file) == kDEntrySize && d[0] != 0xFF;
	const u32 blocks = ok ? readBE16(d + 0x38) : 0;
	u8* data         = nullptr;
	if (ok && blocks >= 1 && blocks <= kUsableBlocks) {
		const u32 length = blocks * kSectorSize;
		data             = static_cast<u8*>(malloc(length));
		ok               = data && fread(data, 1, length, file) == length;
	} else {
		ok = false;
	}
	fclose(file);
	if (!ok) {
		free(data);
		printf("[PC Port] CARD%d: skipping %s (not a complete .gci)\n", static_cast<int>(channel), path);
		return false;
	}
	memset(&slot, 0, sizeof slot);
	slot.used = true;
	snprintf(slot.hostName, sizeof slot.hostName, "%s", hostName);
	slot.data    = data;
	slot.dataLen = blocks * kSectorSize;
	CARDStat& stat = slot.stat;
	memcpy(stat.gameName, d + 0x00, 4);
	memcpy(stat.company, d + 0x04, 2);
	stat.bannerFormat = d[0x07];
	memcpy(stat.fileName, d + 0x08, CARD_FILENAME_MAX);
	stat.time        = readBE32(d + 0x28);
	stat.iconAddr    = readBE32(d + 0x2C);
	stat.iconFormat  = readBE16(d + 0x30);
	stat.iconSpeed   = readBE16(d + 0x32);
	slot.permission  = d[0x34];
	slot.copyTimes   = d[0x35];
	slot.firstBlock  = readBE16(d + 0x36);
	stat.length      = slot.dataLen;
	stat.commentAddr = readBE32(d + 0x3C);
	deriveOffsets(stat);
	return true;
}

// Loads every .gci in a channel's store.
void loadStore(s32 channel)
{
	Store& store = sStore[channel];
	if (store.loaded) return;
	store.loaded = true;
	memset(store.slots, 0, sizeof store.slots);

	// Legacy store migration.
	if (DIR* dir = opendir(legacyStoreDir(channel))) {
		while (struct dirent* entry = readdir(dir)) {
			if (!isGciName(entry->d_name)) continue;
			char from[kPathMax], to[kPathMax];
			snprintf(from, sizeof from, "%s/%s", legacyStoreDir(channel), entry->d_name);
			snprintf(to, sizeof to, "%s/%s", storeDir(channel), entry->d_name);
			if (!fileExists(to)) rename(from, to);
		}
		closedir(dir);
		CARD_RMDIR(legacyStoreDir(channel));
		if (channel != 0) CARD_RMDIR(legacyStoreDir(0));
	}

	char (*names)[kHostNameMax] = nullptr;
	int count = 0, capacity = 0;
	if (DIR* dir = opendir(storeDir(channel))) {
		while (struct dirent* entry = readdir(dir)) {
			if (!isGciName(entry->d_name)) continue;
			if (count == capacity) {
				capacity = capacity ? capacity * 2 : 16;
				names    = static_cast<char (*)[kHostNameMax]>(realloc(names, static_cast<size_t>(capacity) * kHostNameMax));
			}
			snprintf(names[count++], kHostNameMax, "%s", entry->d_name);
		}
		closedir(dir);
	}
	if (count > 1) qsort(names, static_cast<size_t>(count), kHostNameMax, compareNames);

	int fileNo = 0;
	for (int i = 0; i < count && fileNo < CARD_MAX_FILE; i++) {
		if (loadGci(channel, store.slots[fileNo], names[i])) fileNo++;
	}
	free(names);
	// Legacy card_NNN.gci rename.
	for (int i = 0; i < fileNo; i++) {
		int num = 0;
		if (sscanf(store.slots[i].hostName, "card_%d.gci", &num) == 1) adoptHostName(channel, i);
	}
}

void ensureStoreDir(s32 channel)
{
	if (channel != 0) CARD_MKDIR(storeDir(channel));
}

// Writes or deletes the slot's .gci.
bool persist(s32 channel, int fileNo)
{
	const Slot& slot = sStore[channel].slots[fileNo];
	char path[kPathMax], temp[kPathMax + 8];
	slotPath(channel, slot, path, sizeof path);
	if (!slot.used) {
		remove(path);
		return !fileExists(path);
	}
	ensureStoreDir(channel);
	snprintf(temp, sizeof temp, "%s.tmp", path);
	u8 d[kDEntrySize];
	buildDEntry(slot, d);
	FILE* file = fopen(temp, "wb");
	if (!file) return false;
	bool ok = fwrite(d, 1, kDEntrySize, file) == kDEntrySize && fwrite(slot.data, 1, slot.dataLen, file) == slot.dataLen;
	ok      = (fclose(file) == 0) && ok;
	if (!ok) {
		remove(temp);
		return false;
	}
	remove(path);
	const bool renamed = rename(temp, path) == 0;
	if (!renamed) {
		printf("[PC Port] CARD%d: FAILED to rename into %s (%u bytes)\n", static_cast<int>(channel), path,
		       static_cast<unsigned>(kDEntrySize + slot.dataLen));
	}
	return renamed;
}

void freeSlot(Slot& slot)
{
	free(slot.data);
	memset(&slot, 0, sizeof slot);
}

int findByName(s32 channel, const char* fileName)
{
	const Store& store = sStore[channel];
	for (int i = 0; i < CARD_MAX_FILE; i++) {
		const Slot& slot = store.slots[i];
		if (slot.used && sameGame(slot) && strncmp(slot.stat.fileName, fileName, CARD_FILENAME_MAX) == 0) return i;
	}
	return -1;
}

int firstFreeSlot(s32 channel)
{
	for (int i = 0; i < CARD_MAX_FILE; i++) {
		if (!sStore[channel].slots[i].used) return i;
	}
	return -1;
}

u32 usedBlocks(s32 channel)
{
	u32 blocks = 0;
	for (const Slot& slot : sStore[channel].slots) {
		if (slot.used) blocks += slot.dataLen / kSectorSize;
	}
	return blocks;
}

int usedFiles(s32 channel)
{
	int files = 0;
	for (const Slot& slot : sStore[channel].slots) files += slot.used ? 1 : 0;
	return files;
}

// Dolphin export name for a card file.
void dolphinHostName(s32 channel, const u8 gameName[4], const u8 company[2], const char* fileName, char* out, size_t size)
{
	char base[kHostNameMax];
	size_t k = 0;
	for (int i = 0; i < 4 && k + 1 < sizeof base; i++) base[k++] = gameName[i] ? static_cast<char>(gameName[i]) : '_';
	for (int i = 0; i < 2 && k + 1 < sizeof base; i++) base[k++] = company[i] ? static_cast<char>(company[i]) : '_';
	base[k++] = '_';
	for (size_t i = 0; i < CARD_FILENAME_MAX && fileName[i] && k + 1 < sizeof base; i++) {
		const char c = fileName[i];
		base[k++]    = (static_cast<u8>(c) < 0x20 || strchr("\\/:*?\"<>|", c)) ? '_' : c;
	}
	base[k] = 0;
	for (int n = 0; n < 100000; n++) {
		if (n == 0) snprintf(out, size, "%s.gci", base);
		else snprintf(out, size, "%s (%d).gci", base, n);
		bool taken = false;
		for (const Slot& slot : sStore[channel].slots) {
			if (slot.used && strcmp(slot.hostName, out) == 0) { taken = true; break; }
		}
		if (taken) continue;
		char path[kPathMax];
		snprintf(path, sizeof path, "%s/%s", storeDir(channel), out);
		if (!fileExists(path)) return;
	}
}

// Host name assigned by this store.
bool ownHostName(const Slot& slot)
{
	int n = 0;
	if (sscanf(slot.hostName, "card_%d.gci", &n) == 1) return true;
	char want[kHostNameMax];
	snprintf(want, sizeof want, "%.4s%.2s_", reinterpret_cast<const char*>(slot.stat.gameName), reinterpret_cast<const char*>(slot.stat.company));
	return strncmp(slot.hostName, want, strlen(want)) == 0;
}

// Renames the slot's file to its Dolphin export name.
bool persist(s32 channel, int fileNo);
void adoptHostName(s32 channel, int fileNo)
{
	Slot& slot = sStore[channel].slots[fileNo];
	char oldPath[kPathMax], oldName[kHostNameMax];
	slotPath(channel, slot, oldPath, sizeof oldPath);
	memcpy(oldName, slot.hostName, sizeof oldName);
	char fileName[CARD_FILENAME_MAX + 1] = {};
	memcpy(fileName, slot.stat.fileName, CARD_FILENAME_MAX);
	slot.used = false;
	dolphinHostName(channel, slot.stat.gameName, slot.stat.company, fileName, slot.hostName, sizeof slot.hostName);
	slot.used = true;
	if (strcmp(slot.hostName, oldName) == 0) return;
	if (persist(channel, fileNo)) remove(oldPath);
	else memcpy(slot.hostName, oldName, sizeof oldName);
}

s32 finish(s32 channel, s32 result, CARDCallback callback = nullptr)
{
	if (validChannel(channel)) sLastResult[channel] = result;
	if (callback) callback(channel, result);
	return result;
}

void fillInfo(CARDFileInfo* info, s32 channel, int fileNo)
{
	const Slot& slot = sStore[channel].slots[fileNo];
	info->chan   = channel;
	info->fileNo = fileNo;
	info->offset = 0;
	info->length = static_cast<s32>(slot.dataLen);
	info->iBlock = slot.firstBlock;
}

Slot* resolve(const CARDFileInfo* info)
{
	if (!info || !validChannel(info->chan) || info->fileNo < 0 || info->fileNo >= CARD_MAX_FILE) return nullptr;
	loadStore(info->chan);
	Slot& slot = sStore[info->chan].slots[info->fileNo];
	return slot.used ? &slot : nullptr;
}
} // namespace

extern "C" {

void CARDInit(void)
{
	loadStore(0);
}

BOOL CARDProbe(s32 channel) { return validChannel(channel); }
s32 CARDProbeEx(s32 channel, s32* memSize, s32* sectorSize)
{
	if (memSize) *memSize = 128;
	if (sectorSize) *sectorSize = static_cast<s32>(kSectorSize);
	return finish(channel, CARDProbe(channel) ? CARD_RESULT_READY : CARD_RESULT_NOCARD);
}
s32 CARDMountAsync(s32 channel, CARDMemoryCard*, CARDCallback, CARDCallback callback)
{
	if (!validChannel(channel)) return finish(channel, CARD_RESULT_NOCARD, callback);
	loadStore(channel);
	return finish(channel, CARD_RESULT_READY, callback);
}
s32 CARDMount(s32 channel, CARDMemoryCard*, CARDCallback)
{
	if (!validChannel(channel)) return finish(channel, CARD_RESULT_NOCARD);
	loadStore(channel);
	return finish(channel, CARD_RESULT_READY);
}
s32 CARDUnmount(s32 channel) { return finish(channel, CARD_RESULT_READY); }

s32 CARDOpen(s32 channel, const char* fileName, CARDFileInfo* info)
{
	if (!validChannel(channel)) return finish(channel, CARD_RESULT_NOCARD);
	loadStore(channel);
	const int fileNo = fileName ? findByName(channel, fileName) : -1;
	if (!info || fileNo < 0) return finish(channel, CARD_RESULT_NOFILE);
	fillInfo(info, channel, fileNo);
	return finish(channel, CARD_RESULT_READY);
}
s32 CARDFastOpen(s32 channel, s32 fileNo, CARDFileInfo* info)
{
	if (!validChannel(channel)) return finish(channel, CARD_RESULT_NOCARD);
	loadStore(channel);
	if (!info || fileNo < 0 || fileNo >= CARD_MAX_FILE || !sStore[channel].slots[fileNo].used) return finish(channel, CARD_RESULT_NOFILE);
	if (!sameGame(sStore[channel].slots[fileNo])) return finish(channel, CARD_RESULT_NOPERM);
	fillInfo(info, channel, fileNo);
	return finish(channel, CARD_RESULT_READY);
}
s32 CARDClose(CARDFileInfo* info) { return finish(info ? info->chan : 0, CARD_RESULT_READY); }

s32 CARDCreate(s32 channel, const char* fileName, u32 size, CARDFileInfo* info)
{
	if (!validChannel(channel)) return finish(channel, CARD_RESULT_NOCARD);
	loadStore(channel);
	const size_t nameLength = fileName ? strnlen(fileName, CARD_FILENAME_MAX + 1) : 0;
	if (nameLength == 0 || nameLength > CARD_FILENAME_MAX) return finish(channel, CARD_RESULT_NAMETOOLONG);
	if (!info) return finish(channel, CARD_RESULT_FATAL_ERROR);
	if (findByName(channel, fileName) >= 0) return finish(channel, CARD_RESULT_EXIST);
	const int fileNo = firstFreeSlot(channel);
	if (fileNo < 0) return finish(channel, CARD_RESULT_NOENT);
	u32 blocks = (size + kSectorSize - 1) / kSectorSize;
	if (blocks == 0) blocks = 1;
	const u32 used = usedBlocks(channel);
	if (blocks > (used < kUsableBlocks ? kUsableBlocks - used : 0)) return finish(channel, CARD_RESULT_INSSPACE);

	char hostName[kHostNameMax], zeroName[CARD_FILENAME_MAX + 1] = {};
	u8 gameName[4], company[2];
	diskGameCode(gameName, company);
	memcpy(zeroName, fileName, nameLength);
	dolphinHostName(channel, gameName, company, zeroName, hostName, sizeof hostName);
	Slot& slot = sStore[channel].slots[fileNo];
	memset(&slot, 0, sizeof slot);
	slot.dataLen = blocks * kSectorSize;
	slot.data    = static_cast<u8*>(calloc(slot.dataLen, 1));
	if (!slot.data) return finish(channel, CARD_RESULT_IOERROR);
	slot.used       = true;
	slot.permission = CARD_ATTR_PUBLIC;
	slot.copyTimes  = 0;
	slot.firstBlock = CARD_NUM_SYSTEM_BLOCK;
	memcpy(slot.hostName, hostName, sizeof hostName);
	CARDStat& stat = slot.stat;
	memcpy(stat.fileName, fileName, nameLength);
	stat.length = slot.dataLen;
	stat.time   = cardTimeNow();
	diskGameCode(stat.gameName, stat.company);
	stat.iconAddr = stat.commentAddr = 0xffffffffu;
	deriveOffsets(stat);
	if (!persist(channel, fileNo)) {
		freeSlot(slot);
		return finish(channel, CARD_RESULT_IOERROR);
	}
	fillInfo(info, channel, fileNo);
	return finish(channel, CARD_RESULT_READY);
}
s32 CARDCreateAsync(s32 channel, const char* name, u32 size, CARDFileInfo* info, CARDCallback callback)
{
	const s32 result = CARDCreate(channel, name, size, info); if (callback) callback(channel, result); return result;
}

s32 CARDRead(CARDFileInfo* info, void* address, s32 length, s32 offset)
{
	Slot* slot = resolve(info);
	if (!slot) return finish(info ? info->chan : 0, CARD_RESULT_NOFILE);
	if (!address || length < 0 || offset < 0 || static_cast<u32>(offset) + static_cast<u32>(length) > slot->dataLen) return finish(info->chan, CARD_RESULT_LIMIT);
	memcpy(address, slot->data + offset, static_cast<size_t>(length));
	info->offset = offset + length; sTransferred[info->chan] = length;
	return finish(info->chan, CARD_RESULT_READY);
}
s32 CARDReadAsync(CARDFileInfo* info, void* address, s32 length, s32 offset, CARDCallback callback)
{
	const s32 result = CARDRead(info, address, length, offset); if (callback) callback(info ? info->chan : 0, result); return result;
}
s32 CARDWrite(CARDFileInfo* info, void* address, s32 length, s32 offset)
{
	Slot* slot = resolve(info);
	if (!slot) return finish(info ? info->chan : 0, CARD_RESULT_NOFILE);
	if (!address || length < 0 || offset < 0 || static_cast<u32>(offset) + static_cast<u32>(length) > slot->dataLen) return finish(info->chan, CARD_RESULT_LIMIT);
	memcpy(slot->data + offset, address, static_cast<size_t>(length));
	slot->stat.time = cardTimeNow();
	info->offset = offset + length; sTransferred[info->chan] = length;
	return finish(info->chan, persist(info->chan, info->fileNo) ? CARD_RESULT_READY : CARD_RESULT_IOERROR);
}
s32 CARDWriteAsync(CARDFileInfo* info, void* address, s32 length, s32 offset, CARDCallback callback)
{
	const s32 result = CARDWrite(info, address, length, offset); if (callback) callback(info ? info->chan : 0, result); return result;
}
s32 CARDGetXferredBytes(s32 channel) { return validChannel(channel) ? sTransferred[channel].load() : 0; }

s32 CARDFastDelete(s32 channel, s32 fileNo)
{
	if (!validChannel(channel)) return finish(channel, CARD_RESULT_NOCARD);
	loadStore(channel);
	if (fileNo < 0 || fileNo >= CARD_MAX_FILE || !sStore[channel].slots[fileNo].used) return finish(channel, CARD_RESULT_NOFILE);
	Slot& slot = sStore[channel].slots[fileNo];
	if (!sameGame(slot)) return finish(channel, CARD_RESULT_NOPERM);
	free(slot.data);
	slot.data = nullptr; slot.dataLen = 0; slot.used = false;
	const bool removed = persist(channel, fileNo);
	memset(&slot, 0, sizeof slot);
	return finish(channel, removed ? CARD_RESULT_READY : CARD_RESULT_IOERROR);
}
s32 CARDFastDeleteAsync(s32 channel, s32 fileNo, CARDCallback callback)
{
	const s32 result = CARDFastDelete(channel, fileNo); if (callback) callback(channel, result); return result;
}

s32 CARDRename(s32 channel, const char* oldName, const char* newName)
{
	if (!validChannel(channel)) return finish(channel, CARD_RESULT_NOCARD);
	loadStore(channel);
	const int fileNo = oldName ? findByName(channel, oldName) : -1;
	if (fileNo < 0) return finish(channel, CARD_RESULT_NOFILE);
	const size_t nameLength = newName ? strnlen(newName, CARD_FILENAME_MAX + 1) : 0;
	if (nameLength == 0 || nameLength > CARD_FILENAME_MAX) return finish(channel, CARD_RESULT_NAMETOOLONG);
	if (findByName(channel, newName) >= 0) return finish(channel, CARD_RESULT_EXIST);
	Slot& slot         = sStore[channel].slots[fileNo];
	const bool ourName = ownHostName(slot);
	memset(slot.stat.fileName, 0, CARD_FILENAME_MAX);
	memcpy(slot.stat.fileName, newName, nameLength);
	slot.stat.time = cardTimeNow();
	if (ourName) {
		adoptHostName(channel, fileNo);
	}
	return finish(channel, persist(channel, fileNo) ? CARD_RESULT_READY : CARD_RESULT_IOERROR);
}
s32 CARDRenameAsync(s32 channel, const char* oldName, const char* newName, CARDCallback callback)
{
	const s32 result = CARDRename(channel, oldName, newName); if (callback) callback(channel, result); return result;
}

s32 CARDGetStatus(s32 channel, s32 fileNo, CARDStat* stat)
{
	if (!validChannel(channel)) return finish(channel, CARD_RESULT_NOCARD);
	loadStore(channel);
	if (!stat || fileNo < 0 || fileNo >= CARD_MAX_FILE || !sStore[channel].slots[fileNo].used) return finish(channel, CARD_RESULT_NOFILE);
	const Slot& slot = sStore[channel].slots[fileNo];
	if (!sameGame(slot) && !(slot.permission & CARD_ATTR_PUBLIC)) return finish(channel, CARD_RESULT_NOPERM);
	*stat = slot.stat;
	return finish(channel, CARD_RESULT_READY);
}
s32 CARDSetStatus(s32 channel, s32 fileNo, CARDStat* stat)
{
	if (!validChannel(channel)) return finish(channel, CARD_RESULT_NOCARD);
	if (!stat) return finish(channel, CARD_RESULT_FATAL_ERROR);
	if ((stat->iconAddr != 0xffffffffu && stat->iconAddr >= CARD_READ_SIZE)
	    || (stat->commentAddr != 0xffffffffu && (stat->commentAddr % kSectorSize) > kSectorSize - CARD_COMMENT_SIZE)) {
		return finish(channel, CARD_RESULT_FATAL_ERROR);
	}
	loadStore(channel);
	if (fileNo < 0 || fileNo >= CARD_MAX_FILE || !sStore[channel].slots[fileNo].used) return finish(channel, CARD_RESULT_NOFILE);
	Slot& slot = sStore[channel].slots[fileNo];
	if (!sameGame(slot)) return finish(channel, CARD_RESULT_NOPERM);
	CARDStat& own    = slot.stat;
	own.bannerFormat = stat->bannerFormat;
	own.iconAddr     = stat->iconAddr;
	own.iconFormat   = stat->iconFormat;
	own.iconSpeed    = stat->iconSpeed;
	own.commentAddr  = stat->commentAddr;
	own.time         = cardTimeNow();
	deriveOffsets(own);
	stat->offsetBanner     = own.offsetBanner;
	stat->offsetBannerTlut = own.offsetBannerTlut;
	memcpy(stat->offsetIcon, own.offsetIcon, sizeof stat->offsetIcon);
	stat->offsetIconTlut = own.offsetIconTlut;
	stat->offsetData     = own.offsetData;
	return finish(channel, persist(channel, fileNo) ? CARD_RESULT_READY : CARD_RESULT_IOERROR);
}
s32 CARDSetStatusAsync(s32 channel, s32 fileNo, CARDStat* stat, CARDCallback callback)
{
	const s32 result = CARDSetStatus(channel, fileNo, stat); if (callback) callback(channel, result); return result;
}

s32 CARDGetSerialNo(s32 channel, u64* serial) { if (serial) *serial = 0x50494b4d494e0001ULL; return finish(channel, CARD_RESULT_READY); }
s32 CARDGetSectorSize(s32 channel, u32* size) { if (size) *size = kSectorSize; return finish(channel, CARD_RESULT_READY); }
s32 CARDFormat(s32 channel)
{
	if (!validChannel(channel)) return finish(channel, CARD_RESULT_NOCARD);
	loadStore(channel);
	bool ok = true;
	for (int i = 0; i < CARD_MAX_FILE; i++) {
		Slot& slot = sStore[channel].slots[i];
		if (!slot.used) continue;
		free(slot.data);
		slot.data = nullptr; slot.dataLen = 0; slot.used = false;
		ok = persist(channel, i) && ok;
		memset(&slot, 0, sizeof slot);
	}
	return finish(channel, ok ? CARD_RESULT_READY : CARD_RESULT_IOERROR);
}
s32 CARDFormatAsync(s32 channel, CARDCallback callback) { const s32 result = CARDFormat(channel); if (callback) callback(channel, result); return result; }
s32 CARDFreeBlocks(s32 channel, s32* bytesUnused, s32* filesUnused)
{
	if (!validChannel(channel)) return finish(channel, CARD_RESULT_NOCARD);
	loadStore(channel);
	const u32 used = usedBlocks(channel);
	if (bytesUnused) *bytesUnused = static_cast<s32>((used < kUsableBlocks ? kUsableBlocks - used : 0) * kSectorSize);
	if (filesUnused) *filesUnused = CARD_MAX_FILE - usedFiles(channel);
	return finish(channel, CARD_RESULT_READY);
}
s32 CARDGetResultCode(s32 channel) { return validChannel(channel) ? sLastResult[channel].load() : CARD_RESULT_NOCARD; }
s32 CARDCheck(s32 channel) { return finish(channel, validChannel(channel) ? CARD_RESULT_READY : CARD_RESULT_NOCARD); }
s32 CARDCheckAsync(s32 channel, CARDCallback callback) { const s32 result = CARDCheck(channel); if (callback) callback(channel, result); return result; }
s32 CARDCheckExAsync(s32 channel, s32* bytes, CARDCallback callback) { if (bytes) *bytes = 0; return CARDCheckAsync(channel, callback); }

void __CARDSetDiskID(DVDDiskID*) { }
void __CARDDefaultApiCallback(s32, s32) { }
void __CARDSyncCallback(s32, s32) { }
u16 __CARDGetFontEncode() { return CARD_ENCODE_ANSI; }
s32 __CARDSync(s32 channel) { return CARDGetResultCode(channel); }

// Test hook: drops all loaded state.
void card_stubs_reset_for_test(void)
{
	for (Store& store : sStore) {
		for (Slot& slot : store.slots) free(slot.data);
		memset(&store, 0, sizeof store);
	}
	for (int i = 0; i < 2; i++) { sLastResult[i] = CARD_RESULT_READY; sTransferred[i] = 0; }
}

} // extern "C"
