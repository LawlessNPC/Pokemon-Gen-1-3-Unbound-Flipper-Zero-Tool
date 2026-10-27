#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <src/gen3/gen3_cart_id.h>
#include <src/gen3/gen3_hcli.h>
#include <src/gen3/gen3_pkm.h>
#include <src/gen3/gen3_save.h>

/* The cartridge jobs: connect, back up the save, browse and edit it in a working copy, and copy
 * the ROM (plan 5 D5, D7, D8), on top of the helper client (gen3_hcli.h) and an SD store. Pure C,
 * no heap, no file-scope state. Not ISR code: a job blocks in the client's transport, so the jobs
 * run on a worker thread with a 2 KB stack, of which the store needs about 1 KB. So the big
 * working arrays are Gen3Cart fields, and no job keeps more than 256 B of locals, none over 128 B.
 *
 * The link: a job sends helper commands only after a connect's HELLO. Every LinkLost or
 * BusyTimeout needs a new gen3_cart_connect first, since the helper may still be busy with the
 * lost command, or sending its response; until then a job answers LinkLost with no link traffic.
 * That connect keeps trying for the lost command's BUSY budget plus GEN3_HCLI_SYNC_US.
 *
 * Files are on the store, named relative to its folder:
 * - backups/<code>_<tid | nosave>_<stamp>.sav: the whole 128 KB flash, all 32 sectors, as
 *   emulators and PKHeX load it. It is written as GEN3_CART_BACKUP_TMP and renamed only once the
 *   cart and the SD card both read it back the same; nothing writes a .sav after that.
 * - GEN3_CART_WORK_NAME: the working copy, the 14 sectors the game loads in id order, which the
 *   edits go into;
 * - roms/<code>_v<version>.gba, written as the same name plus ".part". */

#define GEN3_CART_BUF_SIZE 4096u /* the buffer gen3_cart_init takes: one sector */
#define GEN3_CART_NAME_MAX 48u /* a store name with its terminator */
#define GEN3_CART_WORK_NAME "cart_work.bin"
#define GEN3_CART_BACKUP_DIR "backups"
#define GEN3_CART_ROM_DIR "roms"
/* The one temporary backup name: Create truncates it, and every failure removes it, so a power
 * cut leaves at most this file, which the next backup overwrites. */
#define GEN3_CART_BACKUP_TMP "backups/backup.tmp"
#define GEN3_CART_ROM_REGION 0x40000u /* a ROM copy is checked, and kept, in regions this long */
/* A backup's free-space check: the .sav, the working copy and a sector to spare */
#define GEN3_CART_BACKUP_SPACE (131072u + 57344u + 4096u)

typedef enum {
    Gen3StoreRead, /* an existing file, to read */
    Gen3StoreCreate, /* a new or truncated file, to read and write; its folder is made */
    Gen3StoreUpdate, /* an existing file, to read and write, kept as it is */
} Gen3StoreMode;

/* The SD card as the jobs need it. Initialise it with designated initializers. At most 2 files
 * are open at once, never one file twice. */
typedef struct {
    void* ctx;
    /* NULL on failure, or if a Read or Update file is absent */
    void* (*open)(void* ctx, const char* name, Gen3StoreMode mode);
    /* Exactly len bytes at off, all inside the file; false otherwise */
    bool (*read)(void* ctx, void* f, uint32_t off, void* buf, uint32_t len);
    /* len bytes at off (at most the file's size), the file growing as needed; false if they
     * weren't all written */
    bool (*write)(void* ctx, void* f, uint32_t off, const void* buf, uint32_t len);
    bool (*truncate)(void* ctx, void* f, uint32_t len); /* to len, at most its size */
    bool (*close)(void* ctx, void* f); /* false: the flush failed; f is closed either way */
    bool (*remove)(void* ctx, const char* name); /* false if absent */
    bool (*rename)(void* ctx, const char* from, const char* to); /* false if `to` exists */
    bool (*size)(void* ctx, const char* name, uint32_t* size); /* false if absent */
    uint64_t (*free_bytes)(void* ctx);
} Gen3Store;

typedef enum {
    Gen3CartStepNone,
    Gen3CartStepMbWait,
    Gen3CartStepMbSend,
    Gen3CartStepIdentify,
    Gen3CartStepBackupRead,
    Gen3CartStepBackupVerify,
    Gen3CartStepAnalyze,
    Gen3CartStepPrepare,
    Gen3CartStepErase,
    Gen3CartStepProgram,
    Gen3CartStepCommit,
    Gen3CartStepVerify,
    Gen3CartStepPostBackup,
    Gen3CartStepRestoreCheck,
    Gen3CartStepRomRead,
    Gen3CartStepRomVerify,
} Gen3CartStep;

/* How a job reports, from its own thread. Initialise it with designated initializers. cb may be
 * NULL; so may cancel, which a job reads between steps (each job says where). */
typedef struct {
    void (*cb)(void* ctx, Gen3CartStep step, uint32_t done, uint32_t total);
    void* ctx;
    volatile bool* cancel;
} Gen3CartProgress;

typedef enum {
    Gen3CartOk,
    Gen3CartWrittenNoBackup,
    Gen3CartNothing,
    Gen3CartCancelled,
    Gen3CartLinkLost,
    Gen3CartBusyTimeout,
    Gen3CartHelperError,
    Gen3CartRefused,
    Gen3CartChanged,
    Gen3CartSdFull,
    Gen3CartSdError,
    Gen3CartVerifyFailed,
    Gen3CartFlashError,
    Gen3CartBadBackup,
    Gen3CartBadArg,
    Gen3CartNoHelper,
    Gen3CartMultibootFailed,
    Gen3CartLinkBusy,
} Gen3CartResult;

/* At most 3 lines of at most 21 characters, broken with '\n'; "" for a value outside the enum */
const char* gen3_cart_result_text(Gen3CartResult r);

/* Pokemon slots of a save: the 6 of the party, then the 420 of the boxes */
#define GEN3_CART_MON_SLOTS (GEN3_SAVE_PARTY_MAX + GEN3_SAVE_BOXES * GEN3_SAVE_BOX_SLOTS)

typedef struct Gen3Cart Gen3Cart;

/* Fields are private to gen3_cart.c: read them through the functions below. About 1.8 KB: a field
 * of the session's state, or heap, never a stack local. */
struct Gen3Cart {
    Gen3Hcli* cli;
    Gen3Store store;
    uint8_t* buf; /* GEN3_CART_BUF_SIZE bytes */
    Gen3CartProgress progress;
    /* The link */
    bool linked; /* a connect's HELLO came, and no command was lost since */
    uint32_t relink_us; /* how long the next connect keeps trying */
    Gen3HHello hello;
    /* The cart, as the last connect found it */
    Gen3HCartInfo info;
    Gen3CartId id; /* id.why: cart_why, or Save from the backup's analysis */
    Gen3CartRefuse cart_why; /* the identification's own */
    /* The backup */
    bool backup_current; /* the .sav and its analysis are the cart's save as it is */
    uint8_t header[0xC0]; /* the header of the cart it came from */
    uint16_t flash_id; /* and that cart's flash id */
    char backup_name[GEN3_CART_NAME_MAX];
    uint32_t crc[GEN3_SAVE_SECTORS]; /* each sector's CRC-32 (gen3_crc32) */
    Gen3SaveSectorInfo infos[GEN3_SAVE_SLOT_SECTORS * 2u]; /* sectors 0..27 in the cart's layout */
    bool analyzed; /* slots holds the analysis of infos (can_browse) */
    Gen3SaveSlots slots;
    bool has_trainer;
    Gen3SaveTrainer trainer;
    /* The working copy */
    bool work_ok; /* it holds the slot the game loads, plus exactly the edits recorded below */
    uint16_t dirty; /* bit k: section id k was edited */
    uint8_t changed[(GEN3_CART_MON_SLOTS + 7u) / 8u]; /* bit per Pokemon slot */
    /* The jobs' scratch */
    Gen3HFooter footers[GEN3_SAVE_SECTORS];
    uint32_t exp_crc[GEN3_SAVE_SLOT_SECTORS];
};

/* No cart and no backup yet, and no link: gen3_cart_connect comes first. cli was just
 * gen3_hcli_init'ed; store and *p are copied (p may be NULL); buf is GEN3_CART_BUF_SIZE bytes,
 * kept. */
void gen3_cart_init(
    Gen3Cart* c,
    Gen3Hcli* cli,
    const Gen3Store* store,
    uint8_t* buf,
    const Gen3CartProgress* p);

/* HELLO (tried again for as long as a lost command may keep the helper busy), CART_INFO,
 * SAVE_PROBE, then gen3_cart_identify. A cart with another header or flash id than the backup's
 * makes the backup not current. Progress Identify; cancel between commands. */
Gen3CartResult gen3_cart_connect(Gen3Cart* c);
/* After gen3_cart_connect: keeps the current backup and working copy, edits included, if this
 * cart's header and flash id (the connect checked them) and all 28 footers (READ_FOOTERS) still
 * match them; otherwise runs gen3_cart_backup, and on Ok sets *dropped = (changes were pending). */
Gen3CartResult gen3_cart_attach(Gen3Cart* c, const char* stamp, bool* dropped);
/* Reads all 32 sectors to a new .sav, verified three ways: the packet CRC, the cart's
 * SECTOR_CRC32 of every sector, and an SD read-back. Then analyzes it and, if browsable, rebuilds
 * the working copy; the edits are gone. Refused unless can_read_save; SdFull before any link
 * traffic unless GEN3_CART_BACKUP_SPACE is free. Any failure removes every file it made and leaves
 * no current backup. stamp: YYYYMMDD-HHMMSS (at most 20 characters). Progress BackupRead per
 * sector, then BackupVerify and Analyze; cancel after each sector. */
Gen3CartResult gen3_cart_backup(Gen3Cart* c, const char* stamp);
/* Rebuilds the working copy from the backup: the edits are gone. BadArg without a current
 * backup of a save the game loads. */
Gen3CartResult gen3_cart_discard(Gen3Cart* c);

const Gen3CartId* gen3_cart_id(const Gen3Cart* c);
/* The backup's analysis, while gen3_cart_backup_current and the cart can_browse */
const Gen3SaveSlots* gen3_cart_slots(const Gen3Cart* c);
bool gen3_cart_trainer(const Gen3Cart* c, Gen3SaveTrainer* out); /* false: no valid save */
const char* gen3_cart_backup_name(const Gen3Cart* c); /* "" without a current backup */
bool gen3_cart_backup_current(const Gen3Cart* c);
/* can_browse, a current backup whose save has a slot the game loads (all 14 ids), and a working
 * copy holding it */
bool gen3_cart_browsable(const Gen3Cart* c);
/* can_write, and a current backup whose save is strict_ok and not padded, with its working copy.
 * When false, *why (why may be NULL) is the identification's reason, else Gen3CartRefuseSave: the
 * save as backed up isn't one to write, or there is no current backup of it. */
bool gen3_cart_writable(const Gen3Cart* c, Gen3CartRefuse* why);

/* The working copy, while browsable; box GEN3_SAVE_PARTY is the party. Each call reads or writes
 * the store. */
uint8_t gen3_cart_party_count(Gen3Cart* c); /* at most 6; 0 if not browsable */
/* A box's name, decoded; false (out "") if not browsable or box > 13 */
bool gen3_cart_box_name(Gen3Cart* c, uint8_t box, char out[9]);
/* The slot's game-form bytes, *len 100 (party) or 80 (box). BadArg if not browsable or out of
 * range. */
Gen3CartResult gen3_cart_mon_get(
    Gen3Cart* c,
    uint8_t box,
    uint8_t slot,
    uint8_t out[GEN3_PARTY_SIZE],
    size_t* len);
/* Encrypts p into the slot (gen3_save_pkm_to_mon) and marks its sections dirty and the slot
 * changed. A party slot equal to the count appends: the count goes up by one. BadArg if not
 * browsable, out of range, or a party slot over the count. The caller checks p
 * (gen3_trade_offer_check) first. A store failure leaves the working copy untrusted: not browsable
 * until gen3_cart_discard. */
Gen3CartResult gen3_cart_mon_put(Gen3Cart* c, uint8_t box, uint8_t slot, const Gen3Pkm* p);
bool gen3_cart_mon_changed(const Gen3Cart* c, uint8_t box, uint8_t slot);
uint16_t gen3_cart_changes(const Gen3Cart* c); /* Pokemon slots changed */

uint32_t gen3_cart_rom_size(const Gen3Cart* c); /* CART_INFO's rom_size; 0: no ROM copy */
void gen3_cart_rom_name(const Gen3Cart* c, char out[GEN3_CART_NAME_MAX]); /* "roms/AXVE_v1.gba" */
/* A .part of this ROM holds at least one region: *done_bytes, its length rounded down to a region,
 * is where a resumed copy starts */
bool gen3_cart_rom_part(Gen3Cart* c, uint32_t* done_bytes);
/* Copies the ROM in GEN3_CART_ROM_REGION regions (plan 5 D8): ROM_READs of 2048 halfwords
 * appended to the .part, then the region's ROM_CRC32 and an SD read-back must both equal the CRC
 * of what came; a region that differs is read once more, then VerifyFailed. The .part keeps only
 * checked regions: any failure, cancel included, cuts it back to the last one. resume starts at
 * gen3_cart_rom_part's length, after checking the last region it keeps again. SdFull before any
 * ROM_READ unless rom_size - that length + 4096 is free. At the end the .part is renamed to
 * gen3_cart_rom_name's, replacing an older copy. Refused when rom_size is 0. Progress RomRead and
 * RomVerify in bytes; cancel between ROM_READs. */
Gen3CartResult gen3_cart_rom_dump(Gen3Cart* c, bool resume);
