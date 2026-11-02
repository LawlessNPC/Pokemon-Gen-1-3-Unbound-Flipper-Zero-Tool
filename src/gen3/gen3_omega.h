#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/gen3_cart.h>
#include <src/gen3/gen3_hcli.h>
#include <src/gen3/gen3_ub_mon.h>
#include <src/gen3/gen3_ub_save.h>

/* The jobs for Pokemon Unbound on an EZ-Flash Omega Definitive Edition (multiboot-and-save.md §8):
 * after HELLO and CART_INFO found the Omega's kernel, find the game the Omega started last and its
 * save on the Omega's microSD, check the game file is Unbound 2.1.1.1, back the save up, and read
 * its party and boxes from the backup. Edits go into a working copy of the backup on the
 * Flipper's store (GEN3_OMEGA_WORK), never into the backup; the copy is made at the end of each
 * backup (if it can't be, the backup still succeeds, work_ok stays false and nothing can be
 * edited). gen3_omega_write is the one job that writes to the card: the save file's own 4 KB
 * sectors, in place, as the game saves (the slot it doesn't load, then the shared sectors an edit
 * changed), and only while the card still holds the backup's save. Pure C, no heap; the
 * worker-thread rules of gen3_cart.h hold (no job keeps more than 256 B of locals). */

#define GEN3_OMEGA_FLAG_ADDR 0x08790000u /* the DE kernel's pending-save flag (Read_sav_info(0)) */
#define GEN3_OMEGA_FLAG_PENDING 0x0011u
#define GEN3_OMEGA_RECENT "/SAVER/Recently play.txt"
#define GEN3_OMEGA_CACHE "omega_rom.txt" /* on the store: one line per game file checked */
#define GEN3_OMEGA_HASH_STEP 0x100000u /* bytes of the game file one SHA256 step hashes */
#define GEN3_OMEGA_WORK "omega_work.bin" /* the working copy; not .sav, so it is no backup */
#define GEN3_OMEGA_CHANGED_BYTES ((6u + GEN3_UB_BOXES * GEN3_UB_BOX_SLOTS + 7u) / 8u)
#define GEN3_OMEGA_SLOT_SAVE 0u
#define GEN3_OMEGA_SLOT_RECENT 1u
#define GEN3_OMEGA_SLOT_ROM 2u

/* Unbound 2.1.1.1's ROM, as PUSE verified it (unbound-2.1.1.1.json: rom_sha256) */
extern const uint8_t gen3_omega_unbound_sha[32];

typedef struct {
    Gen3Hcli* cli;
    Gen3Store store;
    uint8_t* buf; /* GEN3_CART_BUF_SIZE bytes */
    Gen3CartProgress progress;
    /* The last gen3_omega_connect's */
    bool connected; /* it ran to its end, and no command was lost since */
    bool flag_read; /* flag holds the DE's pending-save word, as read */
    uint16_t flag;
    bool mounted; /* info holds OMEGA_MOUNT's answer */
    Gen3HOmegaInfo info;
    char rom_path[GEN3_HCLI_OMEGA_PATH_MAX], save_path[GEN3_HCLI_OMEGA_PATH_MAX];
    Gen3HOmegaFile rom, save;
    bool hashed; /* sha holds the game file's SHA-256 */
    bool verified; /* and it is Unbound 2.1.1.1's */
    bool from_cache; /* the cache held it: not hashed this time */
    uint8_t sha[32];
    /* The backup */
    bool backup_current;
    char backup_name[GEN3_CART_NAME_MAX];
    uint32_t crc[GEN3_UB_SECTORS];
    Gen3UbSector infos[2u * GEN3_UB_SLOT_SECTORS];
    Gen3UbSlots slots;
    bool has_trainer;
    Gen3UbTrainer trainer;
    /* The working copy: a copy of the backup that edits go into */
    bool work_ok; /* it exists and matches the backup, edits aside */
    uint32_t dirty; /* bit k: physical sector k of the save was edited */
    uint8_t changed[GEN3_OMEGA_CHANGED_BYTES]; /* party slot i: bit i; box b slot s: 6 + b * 30 + s */
    /* The last gen3_omega_write's: each sector's CRC-32 as its writes should have left the card */
    uint32_t want_crc[GEN3_UB_SECTORS];
    char line[160]; /* a cache line being built or compared */
} Gen3Omega;

/* Not connected, no backup. cli was gen3_hcli_init'ed; store and *p are copied (p may be NULL);
 * buf is kept. */
void gen3_omega_init(
    Gen3Omega* o,
    Gen3Hcli* cli,
    const Gen3Store* store,
    uint8_t* buf,
    const Gen3CartProgress* p);
/* After HELLO and CART_INFO: the DE's pending-save flag (OmegaPending while it reads 0x0011, before
 * any Omega command), OMEGA_MOUNT, Recently play.txt's first line (the game file, "/....gba", 1..95
 * characters, trailing spaces dropped) and its save (/SAVER/<name>.sav, exactly 131072 bytes, with
 * an Unbound signature in sector 0's or sector 14's footer); NoGame for any of these missing or
 * outside the rules: another game's save, or an Unbound one never saved. Then the game file's identity: the cache's
 * line for it (path, size, first cluster, date and time), else SHA256 in GEN3_OMEGA_HASH_STEP steps
 * and a new line. Ok with verified true or false. The backup is no longer current. Progress
 * OmegaFind 0..4, RomHash per step; cancel between steps (Cancelled, no cache line). */
Gen3CartResult gen3_omega_connect(Gen3Omega* o);
/* The save to backups/UNBOUND_<tid | nosave>_<stamp>.sav (-2, -3 ... if taken), verified three
 * ways: the packet CRC, OMEGA_CRC32 of each 4 KB, and an SD read-back. Then the slots and trainer.
 * BadArg unless connected; SdFull before any link traffic unless 131072 + 4096 bytes are free. Any
 * failure leaves no current backup and no temporary file; LinkLost, BusyTimeout and HelperError
 * also end the connect (the helper may still be busy), so the next job needs a new Connect. Progress BackupRead per 4 KB,
 * BackupVerify, Analyze; cancel after each 4 KB read. */
Gen3CartResult gen3_omega_backup(Gen3Omega* o, const char* stamp);
/* A current backup whose save has a slot the game loads */
bool gen3_omega_browsable(const Gen3Omega* o);
/* From the backup; 0 if not browsable or unreadable */
uint8_t gen3_omega_party_count(Gen3Omega* o);
/* Slot (box, slot) of the backup's loaded save; box GEN3_SAVE_PARTY is the party. Ok, Empty (no
 * species there, or past the party's count), BadArg (not browsable, or no such slot), SdError. */
Gen3CartMon gen3_omega_summary(Gen3Omega* o, uint8_t box, uint8_t slot, Gen3UbSummary* out);

/* Slot (box, slot) of the working copy as a record, out->party set. Ok, Empty (no species there,
 * or past the party's count), BadArg (not browsable, no working copy, or no such slot), SdError. */
Gen3CartMon gen3_omega_mon_get(Gen3Omega* o, uint8_t box, uint8_t slot, Gen3UbMon* out);
/* Writes m into the working copy: a party slot below the count (m a party record, written as
 * given), or a box slot (m a box record). A box slot that is empty takes only a new Pokemon
 * (new_mon); an occupied one only an edit; else Taken. An egg, in the slot or as m, is Egg. Marks
 * the slot changed and its sectors dirty. Refused (BadArg, nothing written) unless browsable, the
 * game file verified and the working copy there. SdError if the copy couldn't be written; it is
 * then not trusted (work_ok false) until gen3_omega_discard rebuilds it. */
Gen3CartMon gen3_omega_mon_put(
    Gen3Omega* o,
    uint8_t box,
    uint8_t slot,
    const Gen3UbMon* m,
    bool new_mon);
bool gen3_omega_mon_changed(const Gen3Omega* o, uint8_t box, uint8_t slot);
uint16_t gen3_omega_changes(const Gen3Omega* o); /* slots changed */
/* The working copy rebuilt from the backup: no edits. BadArg without a current backup. */
Gen3CartResult gen3_omega_discard(Gen3Omega* o);
/* The record new Pokemon take their OT from: the first party Pokemon that is no egg and whose OT id
 * is the trainer's (TID | SID << 16), else the first that is no egg; false if there is none. */
bool gen3_omega_owner(Gen3Omega* o, Gen3UbMon* out);

/* The working copy's edits written to the card's save, as the game saves: the slot the game
 * doesn't load gets all 14 sections with counter N + 1, so that until its last sector is whole the
 * game still loads the old one. That holds while the slot it writes holds no part of a newer save:
 * a save of the game's own cut short there (its index 13 sound, its counter not older than N)
 * could be completed by a cut of this write into a slot of both, so that is refused. In order:
 * - Nothing without changes; Refused unless verified, browsable and the working copy there;
 *   OmegaSaveFirst for that cut save (a save in the game replaces it); BadArg without a stamp (at
 *   most GEN3_CART_STAMP_MAX characters); LinkLost unless connected; SdFull unless 131072 + 4096
 *   bytes are free. None of these sends anything.
 * - Prepare: slot 0 opened again on the save's path, with the connect's size and first cluster,
 *   and each sector's OMEGA_CRC32 the backup's; else Changed, the backup and the edits dropped.
 *   Then OMEGA_ARM for that file.
 * - Program, a sector at a time, each to come back with its image's CRC-32: the write test (the
 *   new slot's first sector, with the backup's bytes, which the card holds); the new slot, its
 *   sector k holding section (k + 14 - r) % 14, r one past the loaded slot's rotation, each
 *   section the working copy's with its footer remade (the id, the checksum, the loaded sector's
 *   signature, N + 1); then sectors 30 and 31, if an edit changed them.
 * - Verify: each sector's OMEGA_CRC32 against what the writes should have left (a write that
 *   landed in another sector of the file shows here).
 * - PostBackup: a new backup (gen3_omega_backup with stamp), which rebuilds the working copy.
 * Ok: written, verified and backed up. Until the new slot is whole the game loads the old one:
 * FlashError (a write the helper failed), VerifyFailed (a read-back that differs), LinkLost or
 * BusyTimeout; the backup and the edits stay, though a Write again finds the card Changed once
 * any of the new slot's bytes did change. After it the game loads the new save, and the backup
 * and the edits are dropped: WrittenShared (a write or a check of sector 30 or 31 failed, the
 * link included), VerifyFailed (the Verify step found a sector other than it should be), and
 * WrittenNoBackup when the Verify step or the new backup couldn't finish (a lost link, a helper
 * error) or the new backup failed. LinkLost, BusyTimeout and HelperError, wherever they come, or
 * an answer to a write the client couldn't take, end the connect: nothing more is sent before a
 * new Connect. Progress Prepare, Program per sector, Verify per sector, then PostBackup for the
 * new backup's steps; never cancelled. */
Gen3CartResult gen3_omega_write(Gen3Omega* o, const char* stamp);

#ifdef GEN3_TESTING
/* Host tests: the hash a game file must have to be verified (NULL: Unbound 2.1.1.1's) */
void gen3_omega_set_expected_sha(const uint8_t* sha);
#endif
