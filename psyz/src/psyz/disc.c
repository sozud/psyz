// Read-only CD images for libcd and for callers that read other discs.
//
// An image is a list of segments laid end to end in one sector space (a
// .cue FILE, a .chd track, or the whole of a .bin/.iso) plus a track list.

#include <psyz/disc.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef PSYZ_CHD
#include <libchdr/chd.h>
#endif

#define MAX_TRACKS 99
#define MAX_SEGMENTS 99
#define MAX_PATH_LEN 1024
#define CHD_FRAME_SIZE 2448

typedef struct {
    int start;      // first sector in the image's sector space
    int count;      // sectors
    int data_size;  // bytes stored per sector: 2352, 2336 or 2048
    int mode;       // header to synthesize for data_size < 2352: 1 or 2
    char path[MAX_PATH_LEN];
    long offset;    // byte offset of the segment in its file
    int chd_frame;  // first CHD frame of the segment
    int swap_audio; // CHD stores audio big-endian
} Segment;

struct PsyzDisc {
    Segment segments[MAX_SEGMENTS];
    int segment_count;
    PsyzDiscTrack tracks[MAX_TRACKS];
    int track_count;
    FILE* file;
    const Segment* file_segment;
#ifdef PSYZ_CHD
    chd_file* chd;
    unsigned char* hunk;
    int hunk_frames;
    int hunk_cached;
#endif
};

static char last_error[512];

static void set_error(const char* format, const char* detail) {
    snprintf(last_error, sizeof(last_error), format, detail);
}

const char* Psyz_DiscLastError(void) { return last_error; }

static int has_extension(const char* path, const char* extension) {
    size_t len = strlen(path), ext = strlen(extension);
    if (len < ext) {
        return 0;
    }
    for (size_t i = 0; i < ext; i++) {
        if (tolower((unsigned char)path[len - ext + i]) != extension[i]) {
            return 0;
        }
    }
    return 1;
}

static long file_size(const char* path) {
    FILE* file = fopen(path, "rb");
    long size;
    if (!file) {
        return -1;
    }
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fclose(file);
    return size;
}

static Segment* add_segment(PsyzDisc* disc, const char* path, int data_size, long bytes) {
    Segment* segment;
    if (disc->segment_count == MAX_SEGMENTS) {
        return NULL;
    }
    segment = &disc->segments[disc->segment_count++];
    memset(segment, 0, sizeof(*segment));
    segment->start = disc->segment_count > 1 ? disc->segments[disc->segment_count - 2].start +
                                                   disc->segments[disc->segment_count - 2].count
                                             : 0;
    segment->count = (int)(bytes / data_size);
    segment->data_size = data_size;
    snprintf(segment->path, sizeof(segment->path), "%s", path ? path : "");
    return segment;
}

static PsyzDiscTrack* add_track(PsyzDisc* disc, int number, PsyzDiscTrackType type, int start) {
    PsyzDiscTrack* track;
    if (disc->track_count == MAX_TRACKS) {
        return NULL;
    }
    track = &disc->tracks[disc->track_count++];
    track->number = number;
    track->type = type;
    track->start = start;
    return track;
}

// ISO9660 primary volume descriptor at sector 16 names the system.
static int is_playstation_iso(const char* path, long offset, int data_size, int header) {
    char pvd[40];
    FILE* file = fopen(path, "rb");
    int result = 0;
    if (!file) {
        return 0;
    }
    if (fseek(file, offset + 16L * data_size + header, SEEK_SET) == 0 &&
        fread(pvd, 1, sizeof(pvd), file) == sizeof(pvd)) {
        result = memcmp(pvd + 1, "CD001", 5) == 0 && memcmp(pvd + 8, "PLAYSTATION", 11) == 0;
    }
    fclose(file);
    return result;
}

static char* trim(char* text) {
    char* end;
    while (isspace((unsigned char)*text)) {
        text++;
    }
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return text;
}

static int open_cue(PsyzDisc* disc, const char* cue_path) {
    char line[1024];
    char directory[MAX_PATH_LEN] = "";
    char file_path[MAX_PATH_LEN] = "";
    const char* slash;
    Segment* segment = NULL;
    PsyzDiscTrack* track = NULL;
    FILE* cue = fopen(cue_path, "r");
    if (!cue) {
        set_error("cannot open %s", cue_path);
        return -1;
    }
    slash = strrchr(cue_path, '/');
    if (!slash) {
        slash = strrchr(cue_path, '\\');
    }
    if (slash) {
        snprintf(directory, sizeof(directory), "%.*s", (int)(slash - cue_path + 1), cue_path);
    }
    while (fgets(line, sizeof(line), cue)) {
        char* text = trim(line);
        if (strncmp(text, "FILE", 4) == 0) {
            char name[MAX_PATH_LEN];
            char* start = strchr(text, '"');
            char* end = start ? strchr(start + 1, '"') : NULL;
            if (start && end) {
                snprintf(name, sizeof(name), "%.*s", (int)(end - start - 1), start + 1);
            } else if (sscanf(text + 4, " %1023s", name) != 1) {
                continue;
            }
            if (name[0] == '/' || name[0] == '\\' || (name[0] && name[1] == ':')) {
                snprintf(file_path, sizeof(file_path), "%s", name);
            } else {
                snprintf(file_path, sizeof(file_path), "%s%s", directory, name);
            }
            segment = NULL; // sized by the first TRACK in the file
        } else if (strncmp(text, "TRACK", 5) == 0) {
            int number, data_size = 2352, mode = 0;
            char type[64];
            PsyzDiscTrackType track_type;
            if (sscanf(text, "TRACK %d %63s", &number, type) != 2 || file_path[0] == '\0') {
                continue;
            }
            if (strcmp(type, "AUDIO") == 0) {
                track_type = PSYZ_DISC_TRACK_AUDIO;
            } else if (strncmp(type, "MODE1", 5) == 0) {
                track_type = PSYZ_DISC_TRACK_MODE1;
                mode = 1;
            } else {
                track_type = PSYZ_DISC_TRACK_MODE2;
                mode = 2;
            }
            if (strcmp(type, "MODE1/2048") == 0 || strcmp(type, "MODE2/2048") == 0) {
                data_size = 2048;
            } else if (strcmp(type, "MODE2/2336") == 0) {
                data_size = 2336;
            }
            if (!segment) {
                long bytes = file_size(file_path);
                if (bytes < 0) {
                    set_error("cannot open %s", file_path);
                    fclose(cue);
                    return -1;
                }
                segment = add_segment(disc, file_path, data_size, bytes);
                if (!segment) {
                    break;
                }
                segment->mode = mode;
            }
            track = add_track(disc, number, track_type, segment->start);
        } else if (strncmp(text, "INDEX", 5) == 0 && track && segment) {
            int index, m, s, f;
            if (sscanf(text, "INDEX %d %d:%d:%d", &index, &m, &s, &f) == 4 && index == 1) {
                track->start = segment->start + (m * 60 + s) * 75 + f;
            }
        }
    }
    fclose(cue);
    if (disc->track_count == 0) {
        set_error("no tracks in %s", cue_path);
        return -1;
    }
    return 0;
}

// A .bin/.img without a cue, or a .iso: one data track.
static int open_single(PsyzDisc* disc, const char* path, int data_size) {
    long bytes = file_size(path);
    Segment* segment;
    int mode;
    if (bytes <= 0) {
        set_error("cannot open %s", path);
        return -1;
    }
    segment = add_segment(disc, path, data_size, bytes);
    if (data_size == 2048) {
        mode = is_playstation_iso(path, 0, 2048, 0) ? 2 : 1;
    } else {
        unsigned char header[16];
        FILE* file = fopen(path, "rb");
        mode = 2;
        if (file && fread(header, 1, sizeof(header), file) == sizeof(header)) {
            mode = header[15] == 1 ? 1 : 2;
        }
        if (file) {
            fclose(file);
        }
    }
    segment->mode = mode;
    add_track(disc, 1, mode == 1 ? PSYZ_DISC_TRACK_MODE1 : PSYZ_DISC_TRACK_MODE2, 0);
    return 0;
}

#ifdef PSYZ_CHD
static int metadata_field(const char* text, const char* key, char* out, size_t size) {
    const char* at = strstr(text, key);
    size_t len = 0;
    if (!at) {
        return 0;
    }
    at += strlen(key);
    while (at[len] && !isspace((unsigned char)at[len]) && len + 1 < size) {
        out[len] = at[len];
        len++;
    }
    out[len] = '\0';
    return 1;
}

static int open_chd(PsyzDisc* disc, const char* path) {
    const chd_header* header;
    int frame = 0;
    if (chd_open(path, CHD_OPEN_READ, NULL, &disc->chd) != CHDERR_NONE) {
        set_error("cannot open CHD %s", path);
        return -1;
    }
    header = chd_get_header(disc->chd);
    if (header->hunkbytes % CHD_FRAME_SIZE != 0) {
        set_error("%s is not a CD image", path);
        return -1;
    }
    disc->hunk_frames = header->hunkbytes / CHD_FRAME_SIZE;
    disc->hunk = malloc(header->hunkbytes);
    disc->hunk_cached = -1;
    for (int i = 0;; i++) {
        char text[512], value[64];
        uint32_t len = 0;
        int number, frames, pregap = 0, data_size, mode = 0;
        PsyzDiscTrackType type;
        Segment* segment;
        if (chd_get_metadata(disc->chd, CDROM_TRACK_METADATA2_TAG, i, text, sizeof(text) - 1, &len,
                             NULL, NULL) != CHDERR_NONE &&
            chd_get_metadata(disc->chd, CDROM_TRACK_METADATA_TAG, i, text, sizeof(text) - 1, &len,
                             NULL, NULL) != CHDERR_NONE) {
            break;
        }
        text[len < sizeof(text) ? len : sizeof(text) - 1] = '\0';
        if (!metadata_field(text, "TRACK:", value, sizeof(value))) {
            break;
        }
        number = atoi(value);
        metadata_field(text, "FRAMES:", value, sizeof(value));
        frames = atoi(value);
        metadata_field(text, "TYPE:", value, sizeof(value));
        if (strcmp(value, "AUDIO") == 0) {
            type = PSYZ_DISC_TRACK_AUDIO;
            data_size = 2352;
        } else if (strcmp(value, "MODE1_RAW") == 0 || strcmp(value, "MODE2_RAW") == 0) {
            type = value[4] == '1' ? PSYZ_DISC_TRACK_MODE1 : PSYZ_DISC_TRACK_MODE2;
            data_size = 2352;
        } else if (strcmp(value, "MODE1") == 0 || strcmp(value, "MODE2_FORM1") == 0) {
            type = value[4] == '1' ? PSYZ_DISC_TRACK_MODE1 : PSYZ_DISC_TRACK_MODE2;
            data_size = 2048;
            mode = value[4] == '1' ? 1 : 2;
        } else if (strcmp(value, "MODE2") == 0 || strcmp(value, "MODE2_FORM_MIX") == 0) {
            type = PSYZ_DISC_TRACK_MODE2;
            data_size = 2336;
            mode = 2;
        } else {
            set_error("unsupported CHD track type %s", value);
            return -1;
        }
        if (metadata_field(text, "PREGAP:", value, sizeof(value))) {
            char pgtype[16] = "";
            metadata_field(text, "PGTYPE:", pgtype, sizeof(pgtype));
            if (pgtype[0] == 'V') {
                pregap = atoi(value); // stored in the track's frames
            }
        }
        segment = add_segment(disc, NULL, data_size, (long)frames * data_size);
        if (!segment) {
            break;
        }
        segment->mode = mode;
        segment->chd_frame = frame;
        segment->swap_audio = type == PSYZ_DISC_TRACK_AUDIO;
        add_track(disc, number, type, segment->start + pregap);
        frame += (frames + 3) & ~3; // tracks are padded to four frames
    }
    if (disc->track_count == 0) {
        set_error("%s is not a CD image", path);
        return -1;
    }
    return 0;
}
#endif

PsyzDisc* Psyz_DiscOpen(const char* path) {
    PsyzDisc* disc;
    int result;
    last_error[0] = '\0';
    if (!path || !*path) {
        set_error("%s", "no disc image given");
        return NULL;
    }
    disc = calloc(1, sizeof(*disc));
    if (!disc) {
        set_error("%s", "out of memory");
        return NULL;
    }
    if (has_extension(path, ".cue")) {
        result = open_cue(disc, path);
    } else if (has_extension(path, ".chd")) {
#ifdef PSYZ_CHD
        result = open_chd(disc, path);
#else
        set_error("%s: CHD support is not built in", path);
        result = -1;
#endif
    } else if (has_extension(path, ".iso")) {
        result = open_single(disc, path, 2048);
    } else {
        result = open_single(disc, path, 2352);
    }
    if (result != 0) {
        Psyz_DiscClose(disc);
        return NULL;
    }
    return disc;
}

void Psyz_DiscClose(PsyzDisc* disc) {
    if (!disc) {
        return;
    }
    if (disc->file) {
        fclose(disc->file);
    }
#ifdef PSYZ_CHD
    if (disc->chd) {
        chd_close(disc->chd);
    }
    free(disc->hunk);
#endif
    free(disc);
}

int Psyz_DiscTrackCount(const PsyzDisc* disc) { return disc ? disc->track_count : 0; }

const PsyzDiscTrack* Psyz_DiscGetTrack(const PsyzDisc* disc, int index) {
    if (!disc || index < 0 || index >= disc->track_count) {
        return NULL;
    }
    return &disc->tracks[index];
}

const PsyzDiscTrack* Psyz_DiscFindTrack(const PsyzDisc* disc, int sector) {
    const PsyzDiscTrack* found = NULL;
    if (!disc || sector < 0 || sector >= Psyz_DiscLeadOut(disc)) {
        return NULL;
    }
    for (int i = 0; i < disc->track_count; i++) {
        if (disc->tracks[i].start <= sector) {
            found = &disc->tracks[i];
        }
    }
    return found ? found : &disc->tracks[0];
}

int Psyz_DiscLeadOut(const PsyzDisc* disc) {
    const Segment* last;
    if (!disc || disc->segment_count == 0) {
        return 0;
    }
    last = &disc->segments[disc->segment_count - 1];
    return last->start + last->count;
}

static void to_bcd_msf(int lba, unsigned char* out) {
    int value = lba + 150;
    int m = value / (60 * 75), s = (value / 75) % 60, f = value % 75;
    out[0] = (unsigned char)(((m / 10) << 4) | (m % 10));
    out[1] = (unsigned char)(((s / 10) << 4) | (s % 10));
    out[2] = (unsigned char)(((f / 10) << 4) | (f % 10));
}

// Builds the raw sector around 2048 or 2336 bytes of stored data.
static void synthesize(int sector, int mode, int data_size, const unsigned char* data,
                       unsigned char* out) {
    memset(out, 0, PSYZ_DISC_SECTOR_SIZE);
    memset(out + 1, 0xFF, 10);
    to_bcd_msf(sector, out + 12);
    out[15] = (unsigned char)mode;
    if (data_size == 2336) {
        memcpy(out + 16, data, 2336);
    } else if (mode == 2) {
        out[18] = out[22] = 0x08; // Form 1 data subheader
        memcpy(out + 24, data, 2048);
    } else {
        memcpy(out + 16, data, 2048);
    }
}

void Psyz_DiscMakeSector(int sector, int mode, const unsigned char* data, unsigned char* out) {
    synthesize(sector, mode, 2048, data, out);
}

int Psyz_DiscReadSector(PsyzDisc* disc, int sector, unsigned char* out) {
    unsigned char data[PSYZ_DISC_SECTOR_SIZE];
    const Segment* segment = NULL;
    int index;
    if (!disc || sector < 0) {
        return -1;
    }
    for (int i = 0; i < disc->segment_count; i++) {
        if (sector >= disc->segments[i].start &&
            sector < disc->segments[i].start + disc->segments[i].count) {
            segment = &disc->segments[i];
            break;
        }
    }
    if (!segment) {
        return -1;
    }
    index = sector - segment->start;
#ifdef PSYZ_CHD
    if (disc->chd) {
        int frame = segment->chd_frame + index;
        int hunk = frame / disc->hunk_frames;
        if (hunk != disc->hunk_cached) {
            if (chd_read(disc->chd, hunk, disc->hunk) != CHDERR_NONE) {
                disc->hunk_cached = -1;
                return -1;
            }
            disc->hunk_cached = hunk;
        }
        memcpy(data, disc->hunk + (frame % disc->hunk_frames) * CHD_FRAME_SIZE,
               segment->data_size);
        if (segment->swap_audio) {
            for (int i = 0; i + 1 < PSYZ_DISC_SECTOR_SIZE; i += 2) {
                unsigned char byte = data[i];
                data[i] = data[i + 1];
                data[i + 1] = byte;
            }
        }
    } else
#endif
    {
        if (disc->file_segment == NULL || strcmp(disc->file_segment->path, segment->path) != 0) {
            if (disc->file) {
                fclose(disc->file);
            }
            disc->file = fopen(segment->path, "rb");
            disc->file_segment = disc->file ? segment : NULL;
            if (!disc->file) {
                return -1;
            }
        }
        if (fseek(disc->file, segment->offset + (long)index * segment->data_size, SEEK_SET) != 0 ||
            fread(data, 1, segment->data_size, disc->file) != (size_t)segment->data_size) {
            return -1;
        }
    }
    if (segment->data_size == PSYZ_DISC_SECTOR_SIZE) {
        memcpy(out, data, PSYZ_DISC_SECTOR_SIZE);
    } else {
        synthesize(sector, segment->mode, segment->data_size, data, out);
    }
    return 0;
}
