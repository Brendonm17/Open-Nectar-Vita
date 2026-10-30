/** GameCube disc image reader: ISO/GCM, CISO, WIA, RVZ. */
#ifndef PC_PORT_DVD_IMAGE_H
#define PC_PORT_DVD_IMAGE_H

#include <cstdint>
#include <string>
#include <vector>

struct DVDDiskID;

/// Opens and parses the image; `error` is a user-facing reason on failure.
bool dvd_image_mount(const char* path, std::string& error);
void dvd_image_unmount();
bool dvd_image_mounted();
/// Path the image was mounted from ("" when none).
const char* dvd_image_path();

/// Number of FST entries (index 0 is the root directory), 0 when not mounted.
int dvd_image_entry_count();

/// Case-insensitive disc path lookup. FST entry index or -1.
int dvd_image_find(const char* discPath);

/// Disc offset and size of a file entry.
bool dvd_image_entry(int idx, uint32_t* offset, uint32_t* size);
bool dvd_image_entry_is_dir(int idx);
/// Full disc path of an entry, no leading slash.
const char* dvd_image_entry_path(int idx);

/** Reads file `idx` at `fileOffset`, zero-padded past end of file. */
int32_t dvd_image_read(int idx, void* dst, int32_t len, int32_t fileOffset);

/** dvd_image_read addressed by disc offset and length. */
int32_t dvd_image_read_file(uint32_t fileStart, uint32_t fileLength, void* dst,
                            int32_t len, int32_t fileOffset);

/// Raw disc bytes at a disc offset.
int64_t dvd_image_read_raw(uint64_t offset, void* dst, uint32_t len);
/// Size of the disc the image represents (0 when unmounted).
uint64_t dvd_image_disc_size();
/// Container description for logs.
const char* dvd_image_container();
/// GameCube junk-data generator.
void dvd_image_junk_generate(const uint8_t seed[68], uint64_t offset, uint32_t count, uint8_t* out);

/// Disc header as a DVDDiskID.
void dvd_image_disk_id(DVDDiskID* id);
/// Six-character game id, e.g. "GPIE01".
const char* dvd_image_game_id();
uint8_t dvd_image_revision();
uint8_t dvd_image_disc_number();

/* Host-path readers */

/// Reads "assets/<disc path>" from the image if mounted, else from disk.
bool pc_disc_file_size(const char* path, uint32_t* size);
bool pc_disc_read_whole_file(const char* path, std::vector<uint8_t>& out);
/// Bytes read (short at EOF) or -1.
int64_t pc_disc_read_range(const char* path, void* dst, uint32_t offset, uint32_t len);

#endif // PC_PORT_DVD_IMAGE_H
