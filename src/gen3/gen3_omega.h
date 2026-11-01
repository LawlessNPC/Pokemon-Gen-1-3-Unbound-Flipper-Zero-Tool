#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/gen3_cart.h>
#include <src/gen3/gen3_hcli.h>
#include <src/gen3/gen3_ub_save.h>

/* The jobs for Pokemon Unbound on an EZ-Flash Omega Definitive Edition (multiboot-and-save.md §8):
 * after HELLO and CART_INFO found the Omega's kernel, find the game the Omega started last and its
 * save on the Omega's microSD, check the game file is Unbound 2.1.1.1, back the save up, and read
 * its party and boxes from the backup. Reads only: nothing is written to the card. Pure C, no heap;
 * the worker-thread rules of gen3_cart.h hold (no job keeps more than 256 B of locals). */

#define GEN3_OMEGA_FLAG_ADDR 0x08790000u /* the DE kernel's pending-save flag (Read_sav_info(0)) */
#define GEN3_OMEGA_FLAG_PENDING 0x0011u
#define GEN3_OMEGA_RECENT "/SAVER/Recently play.txt"
#define GEN3_OMEGA_CACHE "omega_rom.txt" /* on the store: one line per game file checked */
#define GEN3_OMEGA_HASH_STEP 0x100000u /* bytes of the game file one SHA256 step hashes */
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

#ifdef GEN3_TESTING
/* Host tests: the hash a game file must have to be verified (NULL: Unbound 2.1.1.1's) */
void gen3_omega_set_expected_sha(const uint8_t* sha);
#endif
