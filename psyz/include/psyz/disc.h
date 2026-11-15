#ifndef PSYZ_DISC_H
#define PSYZ_DISC_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file disc.h
 * @brief Read-only CD images: .cue/.bin, a lone .bin/.img, .iso and .chd
 *
 * Every image is presented as raw 2352-byte sectors numbered from the first
 * sector of the image (the numbering libcd uses for CdlLOC positions).
 * Images with 2048-byte sectors (.iso, cooked CHD tracks) get a synthesized
 * sync pattern and header: Mode 2 Form 1 when the ISO9660 system identifier
 * says PLAYSTATION, Mode 1 otherwise. Such images carry no XA audio.
 */

#define PSYZ_DISC_SECTOR_SIZE 2352

typedef enum {
    PSYZ_DISC_TRACK_AUDIO,
    PSYZ_DISC_TRACK_MODE1,
    PSYZ_DISC_TRACK_MODE2,
} PsyzDiscTrackType;

typedef struct {
    int number;             /**< Track number, 1-99 */
    PsyzDiscTrackType type; /**< Track type */
    int start;              /**< Sector of INDEX 01 */
} PsyzDiscTrack;

typedef struct PsyzDisc PsyzDisc;

/**
 * @brief Open a disc image, chosen by file extension
 *
 * .cue, .chd (when built with PSYZ_CHD), .iso; anything else is read as a
 * raw 2352-byte single-track image.
 *
 * @return The disc, or NULL (Psyz_DiscLastError describes why)
 */
PsyzDisc* Psyz_DiscOpen(const char* path);

/** @brief Close a disc opened with Psyz_DiscOpen (NULL is allowed) */
void Psyz_DiscClose(PsyzDisc* disc);

/** @brief Number of tracks */
int Psyz_DiscTrackCount(const PsyzDisc* disc);

/** @brief Track by zero-based index, or NULL when out of range */
const PsyzDiscTrack* Psyz_DiscGetTrack(const PsyzDisc* disc, int index);

/** @brief Track containing a sector, or NULL */
const PsyzDiscTrack* Psyz_DiscFindTrack(const PsyzDisc* disc, int sector);

/** @brief First sector past the end of the image (the lead-out) */
int Psyz_DiscLeadOut(const PsyzDisc* disc);

/**
 * @brief Read one raw sector
 *
 * @param out PSYZ_DISC_SECTOR_SIZE bytes
 * @return 0 on success, -1 past the end of the image or on a read error
 */
int Psyz_DiscReadSector(PsyzDisc* disc, int sector, unsigned char* out);

/** @brief Description of the last Psyz_DiscOpen failure on this thread */
const char* Psyz_DiscLastError(void);

/**
 * @brief Build a raw sector around 2048 bytes of user data
 *
 * @param mode 1 for Mode 1, 2 for Mode 2 Form 1
 * @param out PSYZ_DISC_SECTOR_SIZE bytes
 */
void Psyz_DiscMakeSector(int sector, int mode, const unsigned char* data, unsigned char* out);

#ifdef __cplusplus
}
#endif

#endif
