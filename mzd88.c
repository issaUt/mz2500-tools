#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HEADER_SIZE 0x2b0
#define TRACK_OFFSET_COUNT 164
#define CYLINDERS 80
#define HEADS 2
#define TRACKS (CYLINDERS * HEADS)
#define SECTORS_PER_TRACK 16
#define SECTOR_SIZE 256
#define SECTOR_HEADER_SIZE 16
#define TRACK_SIZE (SECTORS_PER_TRACK * (SECTOR_HEADER_SIZE + SECTOR_SIZE))
#define DISK_SIZE (HEADER_SIZE + TRACKS * TRACK_SIZE)

#define TOTAL_RECORDS (CYLINDERS * HEADS * SECTORS_PER_TRACK)
#define BLOCK_FACTOR 2
#define TOTAL_BLOCKS (TOTAL_RECORDS / BLOCK_FACTOR)
#define DATA_OFFSET_BLOCK 0x18
#define DATA_BLOCKS (TOTAL_BLOCKS - DATA_OFFSET_BLOCK)

#define BITMAP_RECORD 0x000f
#define DIRECTORY_START_RECORD 0x0010
#define DIRECTORY_RECORDS 8
#define DIRECTORY_ENTRIES 64
#define DIRECTORY_ENTRY_SIZE 32

#define EMPTY_NAME_BYTE 0x0d

#define MODE_UNUSED 0x00
#define MODE_OBJ 0x01
#define MODE_BTX 0x02
#define MODE_BSD 0x03
#define MODE_BRD 0x04
#define MODE_DIR 0x0f
#define MODE_SWAP_EMPTY 0x80
#define MODE_SWAP_USED 0x81

typedef struct {
    uint8_t *bytes;
    size_t size;
    int directory_base;
} Disk;

typedef struct {
    uint8_t mode;
    char name[18];
    uint8_t attr;
    uint16_t length;
    uint16_t load_addr;
    uint16_t exec_addr;
    uint8_t timestamp[4];
    uint16_t start_record;
} DirectoryEntry;

static void copy_cstr(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0) return;
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void die(const char *message) {
    fprintf(stderr, "error: %s\n", message);
    exit(1);
}

static void die2(const char *prefix, const char *value) {
    fprintf(stderr, "error: %s%s\n", prefix, value);
    exit(1);
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void put_le16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value & 0xff);
    p[1] = (uint8_t)((value >> 8) & 0xff);
}

static void put_le32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value & 0xff);
    p[1] = (uint8_t)((value >> 8) & 0xff);
    p[2] = (uint8_t)((value >> 16) & 0xff);
    p[3] = (uint8_t)((value >> 24) & 0xff);
}

static int streq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

static int starts_with_dash(const char *s) {
    return s != NULL && s[0] == '-';
}

static const char *command_name(const char *s) {
    if (!starts_with_dash(s)) return NULL;
    while (*s == '-') s++;
    return s;
}

static const char *base_name(const char *path) {
    const char *a = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    const char *p = a > b ? a : b;
    return p ? p + 1 : path;
}

static void default_disk_name(const char *path, char out[18]) {
    const char *base = base_name(path);
    size_t len = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot != NULL && dot != base) len = (size_t)(dot - base);
    if (len > 16) len = 16;
    memcpy(out, base, len);
    out[len] = '\0';
}

static const char *extension_of(const char *path) {
    const char *base = base_name(path);
    const char *dot = strrchr(base, '.');
    return dot ? dot : "";
}

static int ext_eq(const char *ext, const char *want) {
    while (*ext && *want) {
        if (tolower((unsigned char)*ext) != tolower((unsigned char)*want)) return 0;
        ext++;
        want++;
    }
    return *ext == '\0' && *want == '\0';
}

static uint8_t infer_mode(const char *path) {
    const char *ext = extension_of(path);
    if (ext_eq(ext, ".brd")) return MODE_BRD;
    if (ext_eq(ext, ".obj") || ext_eq(ext, ".bin")) return MODE_OBJ;
    if (ext_eq(ext, ".btx")) return MODE_BTX;
    if (ext_eq(ext, ".bsd") || ext_eq(ext, ".bas") || ext_eq(ext, ".txt")) return MODE_BSD;
    return MODE_BSD;
}

static uint8_t parse_mode(const char *value) {
    char lower[16];
    size_t len = strlen(value);
    if (len >= sizeof(lower)) die2("unknown mode: ", value);
    for (size_t i = 0; i <= len; i++) lower[i] = (char)tolower((unsigned char)value[i]);
    if (streq(lower, "obj")) return MODE_OBJ;
    if (streq(lower, "btx")) return MODE_BTX;
    if (streq(lower, "bsd") || streq(lower, "bas") || streq(lower, "txt")) return MODE_BSD;
    if (streq(lower, "brd")) return MODE_BRD;
    die2("unknown mode: ", value);
}

static uint16_t parse_word(const char *text, const char *option_name) {
    int base = 10;
    char buf[64];
    size_t len = strlen(text);
    if (len == 0 || len >= sizeof(buf)) {
        fprintf(stderr, "error: invalid %s: %s\n", option_name, text);
        exit(1);
    }
    copy_cstr(buf, sizeof(buf), text);
    if (len > 1 && (buf[len - 1] == 'h' || buf[len - 1] == 'H')) {
        buf[len - 1] = '\0';
        base = 16;
    } else if (len > 2 && buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) {
        base = 16;
    }
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(buf, &end, base);
    if (errno || end == buf || *end != '\0' || value > 0xffffUL) {
        fprintf(stderr, "error: invalid %s: %s\n", option_name, text);
        exit(1);
    }
    return (uint16_t)value;
}

static const char *mode_name(uint8_t mode) {
    switch (mode) {
    case MODE_UNUSED: return "UNUSED";
    case MODE_OBJ: return "OBJ";
    case MODE_BTX: return "BTX";
    case MODE_BSD: return "BSD";
    case MODE_BRD: return "BRD";
    case MODE_DIR: return "DIR";
    case MODE_SWAP_EMPTY: return "SWAP_EMPTY";
    case MODE_SWAP_USED: return "SWAP_USED";
    default: return "UNK";
    }
}

static size_t read_file(const char *path, uint8_t **out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) die2("cannot open: ", path);
    if (fseek(fp, 0, SEEK_END) != 0) die2("cannot seek: ", path);
    long len = ftell(fp);
    if (len < 0) die2("cannot tell: ", path);
    if (fseek(fp, 0, SEEK_SET) != 0) die2("cannot seek: ", path);
    uint8_t *buf = malloc((size_t)len ? (size_t)len : 1);
    if (!buf) die("out of memory");
    if (len > 0 && fread(buf, 1, (size_t)len, fp) != (size_t)len) die2("cannot read: ", path);
    fclose(fp);
    *out = buf;
    return (size_t)len;
}

static void write_file(const char *path, const uint8_t *data, size_t size) {
    FILE *fp = fopen(path, "wb");
    if (!fp) die2("cannot write: ", path);
    if (size > 0 && fwrite(data, 1, size, fp) != size) die2("cannot write: ", path);
    fclose(fp);
}

static int file_exists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}

static size_t sector_header_offset(int track_index, int sector) {
    return HEADER_SIZE + (size_t)track_index * TRACK_SIZE +
           (size_t)(sector - 1) * (SECTOR_HEADER_SIZE + SECTOR_SIZE);
}

static void record_to_track_sector(int record, int *track_index, int *sector) {
    if (record < 0 || record >= TOTAL_RECORDS) die("record out of range");
    int cylinder = record / (HEADS * SECTORS_PER_TRACK);
    int in_cylinder = record % (HEADS * SECTORS_PER_TRACK);
    int head = in_cylinder < SECTORS_PER_TRACK ? 1 : 0;
    *sector = (in_cylinder % SECTORS_PER_TRACK) + 1;
    *track_index = cylinder * HEADS + head;
}

static void read_record(Disk *disk, int record, uint8_t out[SECTOR_SIZE]) {
    int track_index, sector;
    record_to_track_sector(record, &track_index, &sector);
    size_t offset = sector_header_offset(track_index, sector) + SECTOR_HEADER_SIZE;
    for (int i = 0; i < SECTOR_SIZE; i++) out[i] = disk->bytes[offset + i] ^ 0xff;
}

static void write_record(Disk *disk, int record, const uint8_t data[SECTOR_SIZE]) {
    int track_index, sector;
    record_to_track_sector(record, &track_index, &sector);
    size_t offset = sector_header_offset(track_index, sector) + SECTOR_HEADER_SIZE;
    for (int i = 0; i < SECTOR_SIZE; i++) disk->bytes[offset + i] = data[i] ^ 0xff;
}

static void build_blank_d88(Disk *disk, const char *title) {
    disk->size = DISK_SIZE;
    disk->directory_base = DIRECTORY_START_RECORD;
    disk->bytes = calloc(1, DISK_SIZE);
    if (!disk->bytes) die("out of memory");

    size_t title_len = strlen(title);
    if (title_len > 16) title_len = 16;
    memcpy(disk->bytes, title, title_len);
    disk->bytes[0x1a] = 0x00;
    disk->bytes[0x1b] = 0x10;
    put_le32(disk->bytes + 0x1c, DISK_SIZE);

    for (int track = 0; track < TRACKS; track++) {
        put_le32(disk->bytes + 0x20 + track * 4, (uint32_t)(HEADER_SIZE + track * TRACK_SIZE));
    }

    for (int track = 0; track < TRACKS; track++) {
        int cylinder = track / HEADS;
        int head = track % HEADS;
        for (int s = 1; s <= SECTORS_PER_TRACK; s++) {
            size_t off = sector_header_offset(track, s);
            disk->bytes[off + 0] = (uint8_t)cylinder;
            disk->bytes[off + 1] = (uint8_t)head;
            disk->bytes[off + 2] = (uint8_t)s;
            disk->bytes[off + 3] = 1;
            put_le16(disk->bytes + off + 4, SECTORS_PER_TRACK);
            put_le16(disk->bytes + off + 14, SECTOR_SIZE);
        }
    }
}

static void load_disk(Disk *disk, const char *path) {
    disk->size = read_file(path, &disk->bytes);
    disk->directory_base = DIRECTORY_START_RECORD;
    if (disk->size < DISK_SIZE) die2("unsupported D88 image: ", path);
}

static void save_disk(Disk *disk, const char *path) {
    write_file(path, disk->bytes, disk->size);
}

static int entry_used(const DirectoryEntry *entry) {
    return entry->mode != MODE_UNUSED && entry->mode != MODE_SWAP_EMPTY;
}

static void parse_entry(const uint8_t raw[DIRECTORY_ENTRY_SIZE], DirectoryEntry *entry) {
    memset(entry, 0, sizeof(*entry));
    entry->mode = raw[0];
    int n = 0;
    while (n < 17 && raw[1 + n] != EMPTY_NAME_BYTE && raw[1 + n] != 0) {
        entry->name[n] = (char)raw[1 + n];
        n++;
    }
    entry->name[n] = '\0';
    entry->attr = raw[18];
    entry->length = le16(raw + 20);
    entry->load_addr = le16(raw + 22);
    entry->exec_addr = le16(raw + 24);
    memcpy(entry->timestamp, raw + 26, 4);
    entry->start_record = le16(raw + 30);
}

static void empty_entry(int index, DirectoryEntry *entry) {
    memset(entry, 0, sizeof(*entry));
    entry->mode = index == 0 ? MODE_SWAP_EMPTY : MODE_UNUSED;
}

static void entry_to_raw(const DirectoryEntry *entry, uint8_t raw[DIRECTORY_ENTRY_SIZE]) {
    memset(raw, 0, DIRECTORY_ENTRY_SIZE);
    raw[0] = entry->mode;
    size_t len = strlen(entry->name);
    if (len > 17) len = 17;
    for (int i = 0; i < 17; i++) raw[1 + i] = i < (int)len ? (uint8_t)entry->name[i] : EMPTY_NAME_BYTE;
    raw[18] = entry->attr;
    put_le16(raw + 20, entry->length);
    put_le16(raw + 22, entry->load_addr);
    put_le16(raw + 24, entry->exec_addr);
    memcpy(raw + 26, entry->timestamp, 4);
    put_le16(raw + 30, entry->start_record);
}

static void read_directory_entry(Disk *disk, int index, DirectoryEntry *entry) {
    uint8_t record_data[SECTOR_SIZE];
    int record = disk->directory_base + (index * DIRECTORY_ENTRY_SIZE / SECTOR_SIZE);
    int offset = (index * DIRECTORY_ENTRY_SIZE) % SECTOR_SIZE;
    read_record(disk, record, record_data);
    parse_entry(record_data + offset, entry);
}

static void write_directory_entry(Disk *disk, int index, const DirectoryEntry *entry) {
    uint8_t record_data[SECTOR_SIZE];
    uint8_t raw[DIRECTORY_ENTRY_SIZE];
    int record = disk->directory_base + (index * DIRECTORY_ENTRY_SIZE / SECTOR_SIZE);
    int offset = (index * DIRECTORY_ENTRY_SIZE) % SECTOR_SIZE;
    read_record(disk, record, record_data);
    entry_to_raw(entry, raw);
    memcpy(record_data + offset, raw, DIRECTORY_ENTRY_SIZE);
    write_record(disk, record, record_data);
}

static uint32_t entry_byte_length(const DirectoryEntry *entry) {
    return entry->mode == MODE_BRD ? (uint32_t)entry->length * 32U : entry->length;
}

static void timestamp_text(const uint8_t bytes[4], char out[15]) {
    int year = (bytes[0] >> 4) * 10 + (bytes[0] & 15);
    int month = (bytes[1] >> 7) * 10 + ((bytes[1] >> 3) & 15);
    int day = ((bytes[1] >> 1) & 3) * 10 + ((bytes[1] & 1) << 3) + (bytes[2] >> 5);
    int hour = ((bytes[2] >> 3) & 3) * 10 + ((bytes[2] & 7) << 1) + (bytes[3] >> 7);
    int minute = ((bytes[3] >> 4) & 7) * 10 + (bytes[3] & 15);
    snprintf(out, 15, "%02d-%02d-%02d %02d:%02d", year, month, day, hour, minute);
}

static int find_entry(Disk *disk, const char *name, DirectoryEntry *entry_out) {
    for (int i = 0; i < DIRECTORY_ENTRIES; i++) {
        DirectoryEntry entry;
        read_directory_entry(disk, i, &entry);
        if (entry_used(&entry) && strcmp(entry.name, name) == 0) {
            if (entry_out) *entry_out = entry;
            return i;
        }
    }
    return -1;
}

static int find_empty_directory_index(Disk *disk) {
    for (int i = 1; i < DIRECTORY_ENTRIES; i++) {
        DirectoryEntry entry;
        read_directory_entry(disk, i, &entry);
        if (!entry_used(&entry)) return i;
    }
    return -1;
}

static void validate_name(const char *name) {
    size_t len = strlen(name);
    if (len == 0 || len > 16 || streq(name, ".") || streq(name, "..") || strchr(name, '\r')) {
        die2("invalid disk name: ", name);
    }
}

static int valid_directory_record(int record) {
    return record >= DATA_OFFSET_BLOCK * BLOCK_FACTOR &&
           record <= TOTAL_RECORDS - DIRECTORY_RECORDS && record % BLOCK_FACTOR == 0;
}

static void resolve_directory(Disk *disk, const char *path) {
    char copy[512];
    copy_cstr(copy, sizeof(copy), path);
    if (strlen(path) >= sizeof(copy)) die("directory path is too long");
    disk->directory_base = DIRECTORY_START_RECORD;
    char *part = copy;
    while (*part) {
        while (*part == '/' || *part == '\\') part++;
        if (!*part) break;
        char *end = part;
        while (*end && *end != '/' && *end != '\\') end++;
        char saved = *end;
        *end = '\0';
        validate_name(part);
        DirectoryEntry entry;
        if (find_entry(disk, part, &entry) < 0 || entry.mode != MODE_DIR) die2("directory not found: ", part);
        if (!valid_directory_record(entry.start_record)) die("invalid directory location");
        disk->directory_base = entry.start_record;
        if (!saved) break;
        part = end + 1;
    }
}

static void resolve_parent(Disk *disk, const char *path, char leaf[18]) {
    char copy[512];
    if (strlen(path) >= sizeof(copy)) die("disk path is too long");
    copy_cstr(copy, sizeof(copy), path);
    size_t len = strlen(copy);
    while (len > 0 && (copy[len - 1] == '/' || copy[len - 1] == '\\')) copy[--len] = '\0';
    char *slash = strrchr(copy, '/');
    char *backslash = strrchr(copy, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
    const char *name = slash ? slash + 1 : copy;
    validate_name(name);
    copy_cstr(leaf, 18, name);
    if (slash) *slash = '\0';
    resolve_directory(disk, slash ? copy : "/");
}

static void read_bitmap(Disk *disk, uint8_t used[DATA_BLOCKS]) {
    uint8_t data[SECTOR_SIZE];
    read_record(disk, BITMAP_RECORD, data);
    for (int i = 0; i < DATA_BLOCKS; i++) {
        uint8_t byte = data[6 + i / 8];
        used[i] = (uint8_t)((byte & (1 << (i % 8))) != 0);
    }
}

static void write_bitmap(Disk *disk, const uint8_t used[DATA_BLOCKS]) {
    uint8_t data[SECTOR_SIZE];
    memset(data, 0, sizeof(data));
    int used_count = 0;
    for (int i = 0; i < DATA_BLOCKS; i++) if (used[i]) used_count++;
    data[0] = 0x01;
    data[1] = DATA_OFFSET_BLOCK;
    put_le16(data + 2, (uint16_t)(DATA_OFFSET_BLOCK + used_count));
    put_le16(data + 4, TOTAL_BLOCKS);
    data[255] = BLOCK_FACTOR - 1;
    for (int i = 0; i < DATA_BLOCKS; i++) {
        if (used[i]) data[6 + i / 8] = (uint8_t)(data[6 + i / 8] | (uint8_t)(1 << (i % 8)));
    }
    write_record(disk, BITMAP_RECORD, data);
}

static void mark_blocks(Disk *disk, const int *blocks, int block_count, int used_flag) {
    uint8_t used[DATA_BLOCKS];
    read_bitmap(disk, used);
    for (int i = 0; i < block_count; i++) {
        int index = blocks[i] - DATA_OFFSET_BLOCK;
        if (index < 0 || index >= DATA_BLOCKS) die("block out of data area");
        used[index] = (uint8_t)used_flag;
    }
    write_bitmap(disk, used);
}

static int free_blocks(Disk *disk) {
    uint8_t used[DATA_BLOCKS];
    int count = 0;
    read_bitmap(disk, used);
    for (int i = 0; i < DATA_BLOCKS; i++) if (!used[i]) count++;
    return count;
}

static int find_free_run(Disk *disk, int block_count) {
    uint8_t used[DATA_BLOCKS];
    int run_start = -1;
    int run_length = 0;
    read_bitmap(disk, used);
    for (int i = 0; i < DATA_BLOCKS; i++) {
        if (used[i]) {
            run_start = -1;
            run_length = 0;
        } else {
            if (run_start < 0) run_start = i;
            run_length++;
            if (run_length >= block_count) return DATA_OFFSET_BLOCK + run_start;
        }
    }
    return -1;
}

static void read_block(Disk *disk, int block, uint8_t out[BLOCK_FACTOR * SECTOR_SIZE]) {
    read_record(disk, block * BLOCK_FACTOR, out);
    read_record(disk, block * BLOCK_FACTOR + 1, out + SECTOR_SIZE);
}

static void write_block(Disk *disk, int block, const uint8_t data[BLOCK_FACTOR * SECTOR_SIZE]) {
    write_record(disk, block * BLOCK_FACTOR, data);
    write_record(disk, block * BLOCK_FACTOR + 1, data + SECTOR_SIZE);
}

static void encode_time_bytes(uint8_t out[4]) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (!tm) {
        memset(out, 0, 4);
        return;
    }
    int year = (tm->tm_year + 1900) % 100;
    int month = tm->tm_mon + 1;
    int day = tm->tm_mday;
    int hour = tm->tm_hour;
    int minute = tm->tm_min;
    out[0] = (uint8_t)(((year / 10) << 4) | (year % 10));
    out[1] = (uint8_t)(((month / 10) << 7) | ((month % 10) << 3) | ((day / 10) << 1) | ((day % 10) >> 3));
    out[2] = (uint8_t)((((day % 10) & 7) << 5) | ((hour / 10) << 3) | ((hour % 10) >> 1));
    out[3] = (uint8_t)((((hour % 10) & 1) << 7) | ((minute / 10) << 4) | (minute % 10));
}

static void format_data_disk(Disk *disk) {
    uint8_t data[SECTOR_SIZE];
    memset(data, 0xbf, SECTOR_SIZE);
    for (int r = 0; r < TOTAL_RECORDS; r++) write_record(disk, r, data);
    memset(data, 0, SECTOR_SIZE);
    for (int r = 0; r < 0x000f; r++) write_record(disk, r, data);
    read_record(disk, 0, data);
    data[0] = 0x04;
    write_record(disk, 0, data);
    for (int i = 0; i < DIRECTORY_ENTRIES; i++) {
        DirectoryEntry entry;
        empty_entry(i, &entry);
        write_directory_entry(disk, i, &entry);
    }
    uint8_t used[DATA_BLOCKS];
    memset(used, 0, sizeof(used));
    write_bitmap(disk, used);
}

static void blank_disk(Disk *disk, const char *title) {
    build_blank_d88(disk, title);
    format_data_disk(disk);
}

static DirectoryEntry make_directory(Disk *disk, const char *path) {
    char name[18];
    resolve_parent(disk, path, name);
    int parent_base = disk->directory_base;
    if (find_entry(disk, name, NULL) >= 0) die2("file already exists: ", name);
    int index = find_empty_directory_index(disk);
    if (index < 0) die("directory is full");
    int start_block = find_free_run(disk, DIRECTORY_RECORDS / BLOCK_FACTOR);
    if (start_block < 0) die("not enough contiguous free space");
    int blocks[DIRECTORY_RECORDS / BLOCK_FACTOR];
    for (int i = 0; i < DIRECTORY_RECORDS / BLOCK_FACTOR; i++) blocks[i] = start_block + i;
    mark_blocks(disk, blocks, DIRECTORY_RECORDS / BLOCK_FACTOR, 1);

    int child_base = start_block * BLOCK_FACTOR;
    uint8_t zero[SECTOR_SIZE] = {0};
    for (int i = 0; i < DIRECTORY_RECORDS; i++) write_record(disk, child_base + i, zero);
    uint8_t timestamp[4];
    encode_time_bytes(timestamp);
    disk->directory_base = child_base;
    DirectoryEntry entry;
    empty_entry(0, &entry);
    copy_cstr(entry.name, sizeof(entry.name), name);
    entry.attr = 0x81;
    entry.length = DIRECTORY_RECORDS * SECTOR_SIZE;
    entry.load_addr = 1;
    int in_component = 0;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') in_component = 0;
        else if (!in_component) { entry.load_addr++; in_component = 1; }
    }
    entry.start_record = (uint16_t)child_base;
    memcpy(entry.timestamp, timestamp, 4);
    write_directory_entry(disk, 0, &entry);
    for (int i = 1; i <= 2; i++) {
        empty_entry(i, &entry);
        entry.mode = MODE_DIR;
        copy_cstr(entry.name, sizeof(entry.name), i == 1 ? "." : "..");
        entry.attr = 0x81;
        entry.length = DIRECTORY_RECORDS * SECTOR_SIZE;
        entry.start_record = (uint16_t)(i == 1 ? child_base : parent_base);
        memcpy(entry.timestamp, timestamp, 4);
        write_directory_entry(disk, i, &entry);
    }
    disk->directory_base = parent_base;
    empty_entry(index, &entry);
    entry.mode = MODE_DIR;
    copy_cstr(entry.name, sizeof(entry.name), name);
    entry.attr = 0x80;
    entry.length = DIRECTORY_RECORDS * SECTOR_SIZE;
    entry.start_record = (uint16_t)child_base;
    memcpy(entry.timestamp, timestamp, 4);
    write_directory_entry(disk, index, &entry);
    return entry;
}

static int write_linear_data(Disk *disk, const uint8_t *data, size_t data_len, int *blocks_out, int *block_count_out) {
    int block_count = (int)((data_len + (BLOCK_FACTOR * SECTOR_SIZE) - 1) / (BLOCK_FACTOR * SECTOR_SIZE));
    if (block_count < 1) block_count = 1;
    int start_block = find_free_run(disk, block_count);
    if (start_block < 0) die("not enough contiguous free space");
    size_t payload_size = (size_t)block_count * BLOCK_FACTOR * SECTOR_SIZE;
    uint8_t *payload = calloc(1, payload_size);
    if (!payload) die("out of memory");
    memcpy(payload, data, data_len);
    for (int i = 0; i < block_count; i++) {
        blocks_out[i] = start_block + i;
        write_block(disk, start_block + i, payload + (size_t)i * BLOCK_FACTOR * SECTOR_SIZE);
    }
    free(payload);
    *block_count_out = block_count;
    return start_block * BLOCK_FACTOR;
}

static int write_bsd_data(Disk *disk, const uint8_t *data, size_t data_len, int *blocks_out, int *block_count_out) {
    int record_count = (int)((data_len + 253) / 254);
    if (record_count < 1) record_count = 1;
    int block_count = (record_count + BLOCK_FACTOR - 1) / BLOCK_FACTOR;
    if (block_count < 1) block_count = 1;
    int start_block = find_free_run(disk, block_count);
    if (start_block < 0) die("not enough contiguous free space");
    uint8_t empty_block[BLOCK_FACTOR * SECTOR_SIZE];
    memset(empty_block, 0xbf, sizeof(empty_block));
    for (int i = 0; i < block_count; i++) {
        blocks_out[i] = start_block + i;
        write_block(disk, start_block + i, empty_block);
    }
    int start_record = start_block * BLOCK_FACTOR;
    for (int i = 0; i < record_count; i++) {
        uint8_t record_data[SECTOR_SIZE];
        memset(record_data, 0, sizeof(record_data));
        size_t offset = (size_t)i * 254;
        size_t chunk = data_len > offset ? data_len - offset : 0;
        if (chunk > 254) chunk = 254;
        if (chunk > 0) memcpy(record_data, data + offset, chunk);
        put_le16(record_data + 254, (uint16_t)(i == record_count - 1 ? 0 : start_record + i + 1));
        write_record(disk, start_record + i, record_data);
    }
    *block_count_out = block_count;
    return start_record;
}

static int write_brd_data(Disk *disk, const uint8_t *data, size_t data_len, int *blocks_out, int *block_count_out) {
    int chunk_count = (int)((data_len + 4095) / 4096);
    if (chunk_count < 1) chunk_count = 1;
    int pointer_block = find_free_run(disk, 1);
    if (pointer_block < 0) die("not enough free space for BRD pointer block");
    blocks_out[0] = pointer_block;
    int total_blocks = 1;
    mark_blocks(disk, &pointer_block, 1, 1);

    int chunk_blocks[128];
    for (int chunk = 0; chunk < chunk_count; chunk++) {
        int start = find_free_run(disk, 8);
        if (start < 0) die("not enough free space for BRD data");
        int temp[8];
        for (int i = 0; i < 8; i++) {
            temp[i] = start + i;
            blocks_out[total_blocks++] = start + i;
        }
        mark_blocks(disk, temp, 8, 1);
        chunk_blocks[chunk] = start;
    }

    uint8_t pointer[SECTOR_SIZE];
    memset(pointer, 0, sizeof(pointer));
    for (int i = 0; i < chunk_count; i++) {
        put_le16(pointer + i * 2, (uint16_t)(chunk_blocks[i] * BLOCK_FACTOR));
    }
    write_record(disk, pointer_block * BLOCK_FACTOR, pointer);
    memset(pointer, 0xbf, sizeof(pointer));
    write_record(disk, pointer_block * BLOCK_FACTOR + 1, pointer);

    uint8_t block_data[BLOCK_FACTOR * SECTOR_SIZE];
    for (int chunk = 0; chunk < chunk_count; chunk++) {
        for (int i = 0; i < 8; i++) {
            size_t offset = ((size_t)chunk * 8 + i) * BLOCK_FACTOR * SECTOR_SIZE;
            size_t remain = data_len > offset ? data_len - offset : 0;
            size_t copy = remain > sizeof(block_data) ? sizeof(block_data) : remain;
            memset(block_data, 0, sizeof(block_data));
            if (copy > 0) memcpy(block_data, data + offset, copy);
            write_block(disk, chunk_blocks[chunk] + i, block_data);
        }
    }

    *block_count_out = total_blocks;
    return pointer_block * BLOCK_FACTOR;
}

static int bsd_records(Disk *disk, const DirectoryEntry *entry, int *records, int max_records) {
    int record = entry->start_record;
    int expected = (int)((entry_byte_length(entry) + 253) / 254);
    if (expected < 1) expected = 1;
    int count = 0;
    for (int i = 0; i < expected && count < max_records; i++) {
        uint8_t data[SECTOR_SIZE];
        records[count++] = record;
        read_record(disk, record, data);
        record = le16(data + 254);
        if (record == 0) break;
    }
    return count;
}

static int entry_blocks(Disk *disk, const DirectoryEntry *entry, int *blocks, int max_blocks) {
    int count = 0;
    if (entry->mode == MODE_BRD) {
        uint8_t pointer[SECTOR_SIZE];
        blocks[count++] = entry->start_record / BLOCK_FACTOR;
        read_record(disk, entry->start_record, pointer);
        for (int i = 0; i < 128 && count + 8 <= max_blocks; i++) {
            uint16_t record = le16(pointer + i * 2);
            if (record == 0) break;
            int start_block = record / BLOCK_FACTOR;
            for (int j = 0; j < 8; j++) blocks[count++] = start_block + j;
        }
    } else if (entry->mode == MODE_BSD) {
        int records[512];
        int record_count = bsd_records(disk, entry, records, 512);
        for (int i = 0; i < record_count; i++) {
            int block = records[i] / BLOCK_FACTOR;
            int exists = 0;
            for (int j = 0; j < count; j++) if (blocks[j] == block) exists = 1;
            if (!exists && count < max_blocks) blocks[count++] = block;
        }
    } else {
        int block_count = (int)((entry_byte_length(entry) + (BLOCK_FACTOR * SECTOR_SIZE) - 1) /
                                (BLOCK_FACTOR * SECTOR_SIZE));
        if (block_count < 1) block_count = 1;
        int start_block = entry->start_record / BLOCK_FACTOR;
        for (int i = 0; i < block_count && count < max_blocks; i++) blocks[count++] = start_block + i;
    }
    return count;
}

static void add_file(Disk *disk, const char *source_path, const char *name_option, int mode_set,
                     uint8_t mode, int force, int has_load, uint16_t load_addr,
                     int has_exec, uint16_t exec_addr) {
    uint8_t *data = NULL;
    size_t data_len = read_file(source_path, &data);
    char name[18];
    if (name_option) resolve_parent(disk, name_option, name);
    else { default_disk_name(source_path, name); resolve_directory(disk, "/"); }
    if (!mode_set) mode = infer_mode(source_path);
    if ((has_load || has_exec) && mode != MODE_OBJ) die("--load-addr/--exec-addr can be used only with OBJ files");

    DirectoryEntry existing;
    int existing_index = find_entry(disk, name, &existing);
    if (existing_index >= 0) {
        if (!force) die2("file already exists: ", name);
        if (existing.mode == MODE_DIR) die2("cannot replace a directory: ", name);
        int blocks[2048];
        int block_count = entry_blocks(disk, &existing, blocks, 2048);
        uint8_t empty[BLOCK_FACTOR * SECTOR_SIZE];
        memset(empty, 0xbf, sizeof(empty));
        for (int i = 0; i < block_count; i++) write_block(disk, blocks[i], empty);
        mark_blocks(disk, blocks, block_count, 0);
        DirectoryEntry empty_dir_entry;
        empty_entry(existing_index, &empty_dir_entry);
        write_directory_entry(disk, existing_index, &empty_dir_entry);
    }

    int entry_index = find_empty_directory_index(disk);
    if (entry_index < 0) die("directory is full");

    int blocks[2048];
    int block_count = 0;
    int start_record;
    uint16_t length;
    if (mode == MODE_BRD) {
        start_record = write_brd_data(disk, data, data_len, blocks, &block_count);
        length = (uint16_t)((data_len + 31) / 32);
    } else if (mode == MODE_BSD) {
        start_record = write_bsd_data(disk, data, data_len, blocks, &block_count);
        length = (uint16_t)data_len;
    } else {
        start_record = write_linear_data(disk, data, data_len, blocks, &block_count);
        length = (uint16_t)data_len;
    }

    DirectoryEntry entry;
    empty_entry(entry_index, &entry);
    entry.mode = mode;
    copy_cstr(entry.name, sizeof(entry.name), name);
    entry.length = length;
    entry.load_addr = has_load ? load_addr : 0;
    entry.exec_addr = has_exec ? exec_addr : 0;
    entry.start_record = (uint16_t)start_record;
    encode_time_bytes(entry.timestamp);
    write_directory_entry(disk, entry_index, &entry);
    mark_blocks(disk, blocks, block_count, 1);
    printf("added %s %s %u bytes\n", entry.name, mode_name(entry.mode), entry_byte_length(&entry));
    free(data);
}

static DirectoryEntry delete_one(Disk *disk, const char *path, int recursive) {
    char name[18];
    resolve_parent(disk, path, name);
    int parent_base = disk->directory_base;
    DirectoryEntry entry;
    int index = find_entry(disk, name, &entry);
    if (index < 0) die2("file not found: ", path);
    if (entry.mode == MODE_DIR) {
        if (!valid_directory_record(entry.start_record)) die("invalid directory location");
        disk->directory_base = entry.start_record;
        for (int i = 1; i < DIRECTORY_ENTRIES; i++) {
            DirectoryEntry child;
            read_directory_entry(disk, i, &child);
            if (!entry_used(&child) || streq(child.name, ".") || streq(child.name, "..")) continue;
            if (!recursive) die2("directory is not empty: ", path);
            char child_path[512];
            int n = snprintf(child_path, sizeof(child_path), "%s/%s", path, child.name);
            if (n < 0 || n >= (int)sizeof(child_path)) die("disk path is too long");
            delete_one(disk, child_path, 1);
            disk->directory_base = entry.start_record;
        }
        disk->directory_base = parent_base;
    }
    int blocks[2048];
    int block_count = entry_blocks(disk, &entry, blocks, 2048);
    uint8_t empty[BLOCK_FACTOR * SECTOR_SIZE];
    memset(empty, 0xbf, sizeof(empty));
    for (int i = 0; i < block_count; i++) write_block(disk, blocks[i], empty);
    mark_blocks(disk, blocks, block_count, 0);
    DirectoryEntry empty_entry_value;
    empty_entry(index, &empty_entry_value);
    write_directory_entry(disk, index, &empty_entry_value);
    return entry;
}

static void extract_file(Disk *disk, const char *name, const char *output_path) {
    char leaf[18];
    resolve_parent(disk, name, leaf);
    DirectoryEntry entry;
    if (find_entry(disk, leaf, &entry) < 0) die2("file not found: ", name);
    if (entry.mode == MODE_DIR) die("cannot extract a directory");
    uint32_t length = entry_byte_length(&entry);
    uint8_t *out = malloc(length ? length : 1);
    if (!out) die("out of memory");
    uint32_t pos = 0;
    if (entry.mode == MODE_BRD) {
        uint8_t pointer[SECTOR_SIZE];
        read_record(disk, entry.start_record, pointer);
        for (int i = 0; i < 128 && pos < length; i++) {
            uint16_t record = le16(pointer + i * 2);
            if (record == 0) break;
            int start_block = record / BLOCK_FACTOR;
            for (int j = 0; j < 8 && pos < length; j++) {
                uint8_t block_data[BLOCK_FACTOR * SECTOR_SIZE];
                read_block(disk, start_block + j, block_data);
                uint32_t copy = length - pos;
                if (copy > sizeof(block_data)) copy = sizeof(block_data);
                memcpy(out + pos, block_data, copy);
                pos += copy;
            }
        }
    } else if (entry.mode == MODE_BSD) {
        int records[512];
        int count = bsd_records(disk, &entry, records, 512);
        for (int i = 0; i < count && pos < length; i++) {
            uint8_t data[SECTOR_SIZE];
            read_record(disk, records[i], data);
            uint32_t copy = length - pos;
            if (copy > 254) copy = 254;
            memcpy(out + pos, data, copy);
            pos += copy;
        }
    } else {
        int block_count = (int)((length + (BLOCK_FACTOR * SECTOR_SIZE) - 1) / (BLOCK_FACTOR * SECTOR_SIZE));
        if (block_count < 1) block_count = 1;
        int start_block = entry.start_record / BLOCK_FACTOR;
        for (int i = 0; i < block_count && pos < length; i++) {
            uint8_t block_data[BLOCK_FACTOR * SECTOR_SIZE];
            read_block(disk, start_block + i, block_data);
            uint32_t copy = length - pos;
            if (copy > sizeof(block_data)) copy = sizeof(block_data);
            memcpy(out + pos, block_data, copy);
            pos += copy;
        }
    }
    write_file(output_path, out, length);
    free(out);
}

static void usage(void) {
    puts("usage:");
    puts("  mzd88 -blank OUTPUT.d88 [--title TITLE]");
    puts("  mzd88 -list IMAGE.d88 [DIR] [--free]");
    puts("  mzd88 -mkdir IMAGE.d88 DIR...");
    puts("  mzd88 -add IMAGE.d88 SOURCE... [--dir DIR] [--name NAME] [--mode bsd|brd|btx|obj] [--force] [--load-addr ADDR] [--exec-addr ADDR]");
    puts("  mzd88 -extract IMAGE.d88 PATH OUTPUT");
    puts("  mzd88 -delete IMAGE.d88 PATH... [--recursive] | --all [--dir DIR]");
    puts("  mzd88 -rename IMAGE.d88 OLD_PATH NEW_PATH");
}

static void cmd_blank(int argc, char **argv) {
    const char *title = "BLANK";
    const char *output_path = NULL;
    for (int i = 0; i < argc; i++) {
        if (streq(argv[i], "--title")) {
            if (i + 1 >= argc) die("missing --title value");
            title = argv[i + 1];
            i++;
        } else if (starts_with_dash(argv[i])) {
            die2("unknown option: ", argv[i]);
        } else if (!output_path) {
            output_path = argv[i];
        } else {
            die2("unexpected argument: ", argv[i]);
        }
    }
    if (!output_path) die("missing output D88 path");
    Disk disk;
    blank_disk(&disk, title);
    save_disk(&disk, output_path);
    free(disk.bytes);
}

static void cmd_list(int argc, char **argv) {
    int show_free = 0;
    const char *image_path = NULL;
    const char *directory = "/";
    int has_directory = 0;
    for (int i = 0; i < argc; i++) {
        if (streq(argv[i], "--free")) {
            show_free = 1;
        } else if (starts_with_dash(argv[i])) {
            die2("unknown option: ", argv[i]);
        } else if (!image_path) {
            image_path = argv[i];
        } else if (!has_directory) {
            directory = argv[i];
            has_directory = 1;
        } else {
            die2("unexpected argument: ", argv[i]);
        }
    }
    if (!image_path) die("missing D88 path");
    Disk disk;
    load_disk(&disk, image_path);
    resolve_directory(&disk, directory);
    puts("idx mode name              bytes  start  load  exec  date");
    for (int index = 0; index < DIRECTORY_ENTRIES; index++) {
        DirectoryEntry entry;
        read_directory_entry(&disk, index, &entry);
        if (entry_used(&entry) && !streq(entry.name, ".") && !streq(entry.name, "..")) {
            char date[15];
            timestamp_text(entry.timestamp, date);
            printf("%3d %-4s %-17s %6u  %04X  %04X  %04X  %s\n",
                   index, mode_name(entry.mode), entry.name, entry_byte_length(&entry),
                   entry.start_record, entry.load_addr, entry.exec_addr, date);
        }
    }
    if (show_free) {
        int blocks = free_blocks(&disk);
        printf("free: %d bytes (%d blocks)\n", blocks * BLOCK_FACTOR * SECTOR_SIZE, blocks);
    }
    free(disk.bytes);
}

static void cmd_mkdir(int argc, char **argv) {
    if (argc < 1) die("missing D88 path");
    if (argc < 2) die("missing directory path");
    Disk disk;
    load_disk(&disk, argv[0]);
    for (int i = 1; i < argc; i++) {
        if (starts_with_dash(argv[i])) die2("unknown option: ", argv[i]);
        make_directory(&disk, argv[i]);
        printf("created %s\n", argv[i]);
    }
    save_disk(&disk, argv[0]);
    free(disk.bytes);
}

static void cmd_add(int argc, char **argv) {
    uint8_t mode = 0;
    int mode_set = 0, force = 0, has_load = 0, has_exec = 0;
    uint16_t load_addr = 0, exec_addr = 0;
    const char *name = NULL;
    const char *directory = "/";
    const char *positionals[256];
    int positional_count = 0;
    for (int i = 0; i < argc; i++) {
        if (streq(argv[i], "--mode")) {
            if (i + 1 >= argc) die("missing --mode value");
            mode = parse_mode(argv[i + 1]);
            mode_set = 1;
            i++;
        } else if (streq(argv[i], "--name")) {
            if (i + 1 >= argc) die("missing --name value");
            name = argv[i + 1];
            i++;
        } else if (streq(argv[i], "--dir")) {
            if (i + 1 >= argc) die("missing --dir value");
            directory = argv[++i];
        } else if (streq(argv[i], "--force")) {
            force = 1;
        } else if (streq(argv[i], "--load-addr")) {
            if (i + 1 >= argc) die("missing --load-addr value");
            load_addr = parse_word(argv[i + 1], "--load-addr");
            has_load = 1;
            i++;
        } else if (streq(argv[i], "--exec-addr")) {
            if (i + 1 >= argc) die("missing --exec-addr value");
            exec_addr = parse_word(argv[i + 1], "--exec-addr");
            has_exec = 1;
            i++;
        } else if (starts_with_dash(argv[i])) {
            die2("unknown option: ", argv[i]);
        } else {
            if (positional_count >= (int)(sizeof(positionals) / sizeof(positionals[0]))) die("too many arguments");
            positionals[positional_count++] = argv[i];
        }
    }
    if (positional_count < 1) die("missing D88 path");
    const char *image_path = positionals[0];
    if (positional_count < 2) die("missing source file path");
    if (name && positional_count - 1 > 1) die("--name can be used only when adding one file");
    if ((has_load || has_exec) && mode_set && mode != MODE_OBJ) die("--load-addr/--exec-addr can be used only with OBJ files");
    Disk disk;
    if (file_exists(image_path)) load_disk(&disk, image_path);
    else blank_disk(&disk, "BLANK");
    resolve_directory(&disk, directory);
    for (int i = 1; i < positional_count; i++) {
        char default_name[18];
        if (!name) default_disk_name(positionals[i], default_name);
        const char *leaf = name ? name : default_name;
        char target[512];
        int n = snprintf(target, sizeof(target), "%s/%s", directory, leaf);
        if (n < 0 || n >= (int)sizeof(target)) die("disk path is too long");
        add_file(&disk, positionals[i], target, mode_set, mode, force, has_load, load_addr, has_exec, exec_addr);
    }
    save_disk(&disk, image_path);
    free(disk.bytes);
}

static void cmd_delete(int argc, char **argv) {
    int delete_all = 0;
    int recursive = 0;
    const char *directory = "/";
    const char *positionals[256];
    int positional_count = 0;
    for (int i = 0; i < argc; i++) {
        if (streq(argv[i], "--all")) {
            delete_all = 1;
        } else if (streq(argv[i], "--recursive")) {
            recursive = 1;
        } else if (streq(argv[i], "--dir")) {
            if (i + 1 >= argc) die("missing --dir value");
            directory = argv[++i];
        } else if (starts_with_dash(argv[i])) {
            die2("unknown option: ", argv[i]);
        } else {
            if (positional_count >= (int)(sizeof(positionals) / sizeof(positionals[0]))) die("too many arguments");
            positionals[positional_count++] = argv[i];
        }
    }
    if (positional_count < 1) die("missing D88 path");
    const char *image_path = positionals[0];
    Disk disk;
    load_disk(&disk, image_path);
    if (delete_all) {
        if (positional_count > 1) die("--all cannot be combined with file names");
        resolve_directory(&disk, directory);
        char names[DIRECTORY_ENTRIES][18];
        int count = 0;
        for (int idx = 0; idx < DIRECTORY_ENTRIES; idx++) {
            DirectoryEntry entry;
            read_directory_entry(&disk, idx, &entry);
            if (entry_used(&entry) && !streq(entry.name, ".") && !streq(entry.name, ".."))
                copy_cstr(names[count++], sizeof(names[0]), entry.name);
        }
        for (int n = 0; n < count; n++) {
            char target[512];
            int len = snprintf(target, sizeof(target), "%s/%s", directory, names[n]);
            if (len < 0 || len >= (int)sizeof(target)) die("disk path is too long");
            DirectoryEntry entry = delete_one(&disk, target, 1);
            printf("deleted %s\n", entry.name);
        }
    } else {
        if (positional_count < 2) die("missing file name");
        for (int i = 1; i < positional_count; i++) {
            DirectoryEntry entry = delete_one(&disk, positionals[i], recursive);
            printf("deleted %s\n", entry.name);
        }
    }
    save_disk(&disk, image_path);
    free(disk.bytes);
}

static void cmd_rename(int argc, char **argv) {
    if (argc < 3) die("missing rename arguments");
    Disk disk;
    load_disk(&disk, argv[0]);
    const char *old_name = argv[1];
    const char *new_name = argv[2];
    char old_leaf[18], new_leaf[18];
    resolve_parent(&disk, new_name, new_leaf);
    int new_base = disk.directory_base;
    if (find_entry(&disk, new_leaf, NULL) >= 0) die2("file already exists: ", new_name);
    resolve_parent(&disk, old_name, old_leaf);
    if (disk.directory_base != new_base) die("moving between directories is not supported");
    DirectoryEntry entry;
    int index = find_entry(&disk, old_leaf, &entry);
    if (index < 0) die2("file not found: ", old_name);
    copy_cstr(entry.name, sizeof(entry.name), new_leaf);
    encode_time_bytes(entry.timestamp);
    write_directory_entry(&disk, index, &entry);
    save_disk(&disk, argv[0]);
    printf("renamed %s -> %s\n", old_name, new_name);
    free(disk.bytes);
}

static void cmd_extract(int argc, char **argv) {
    if (argc < 3) die("missing extract arguments");
    Disk disk;
    load_disk(&disk, argv[0]);
    extract_file(&disk, argv[1], argv[2]);
    free(disk.bytes);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 0;
    }
    const char *cmd = command_name(argv[1]);
    if (!cmd) {
        usage();
        return 1;
    }
    if (streq(cmd, "blank")) cmd_blank(argc - 2, argv + 2);
    else if (streq(cmd, "list") || streq(cmd, "ls")) cmd_list(argc - 2, argv + 2);
    else if (streq(cmd, "mkdir")) cmd_mkdir(argc - 2, argv + 2);
    else if (streq(cmd, "add")) cmd_add(argc - 2, argv + 2);
    else if (streq(cmd, "delete") || streq(cmd, "del") || streq(cmd, "rm")) cmd_delete(argc - 2, argv + 2);
    else if (streq(cmd, "rename") || streq(cmd, "ren") || streq(cmd, "mv")) cmd_rename(argc - 2, argv + 2);
    else if (streq(cmd, "extract") || streq(cmd, "get")) cmd_extract(argc - 2, argv + 2);
    else {
        usage();
        return 1;
    }
    return 0;
}
