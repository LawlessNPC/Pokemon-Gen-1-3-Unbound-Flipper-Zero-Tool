#include <src/gen3/gen3_cart_screen.h>

#include <stdio.h>
#include <string.h>

#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_text.h>

#define CART_SCREEN_CABLE "Cable: plug in/swap ends"
#define CART_SCREEN_BACK_STOP "Back: stop"
#define CART_SCREEN_NO_POWER_OFF "Do not power off"
/* A backup's stamp, YYYYMMDD-HHMMSS, after the name's last '_' */
#define CART_SCREEN_STAMP_LEN 15u

Gen3CartBack gen3_cart_screen_back(Gen3CartJob job) {
    switch(job) {
    case Gen3CartJobWrite:
    case Gen3CartJobRestore:
    case Gen3CartJobDiscard:
        return Gen3CartBackIgnore;
    case Gen3CartJobRom:
        return Gen3CartBackAsk;
    case Gen3CartJobConnect:
    case Gen3CartJobBackup:
    case Gen3CartJobRestoreCheck:
    case Gen3CartJobReconnect:
        break;
    }
    return Gen3CartBackStop;
}

bool gen3_cart_screen_new_trainer(Gen3CartJob job, Gen3CartResult r) {
    if(r != Gen3CartOk) return false;
    switch(job) {
    case Gen3CartJobConnect:
    case Gen3CartJobBackup:
    case Gen3CartJobRestore:
        return true;
    /* A write changes only the party and the boxes; the others keep the backup they had */
    case Gen3CartJobWrite:
    case Gen3CartJobDiscard:
    case Gen3CartJobRestoreCheck:
    case Gen3CartJobRom:
    case Gen3CartJobReconnect:
        break;
    }
    return false;
}

Gen3CartJob
    gen3_cart_screen_result_job(Gen3CartJob job, Gen3CartResult r, Gen3CartFinished finished) {
    if(job != Gen3CartJobConnect && job != Gen3CartJobReconnect) return job;
    if(r != Gen3CartOk && r != Gen3CartWrittenNoBackup) return job;
    switch(finished) {
    case Gen3CartFinishedWrite:
        return Gen3CartJobWrite;
    case Gen3CartFinishedRestore:
        return Gen3CartJobRestore;
    case Gen3CartFinishedNone:
        break;
    }
    return job;
}

/* A write's or restore's flash steps: a power cut there leaves the slot being written half done */
static bool cart_screen_flashing(Gen3CartJob job, Gen3CartStep step) {
    if(job != Gen3CartJobWrite && job != Gen3CartJobRestore) return false;
    return step == Gen3CartStepErase || step == Gen3CartStepProgram ||
           step == Gen3CartStepCommit || step == Gen3CartStepVerify;
}

static const char* cart_screen_title(Gen3CartStep step) {
    switch(step) {
    case Gen3CartStepNone:
        return "Starting";
    case Gen3CartStepMbWait:
        return "Waiting for GBA";
    case Gen3CartStepMbSend:
        return "Sending helper";
    case Gen3CartStepIdentify:
        return "Reading cart";
    case Gen3CartStepBackupRead:
        return "Backing up save";
    case Gen3CartStepBackupVerify:
        return "Checking backup";
    case Gen3CartStepAnalyze:
        return "Reading save";
    case Gen3CartStepPrepare:
        return "Preparing";
    case Gen3CartStepErase:
    case Gen3CartStepProgram:
    case Gen3CartStepCommit:
        return "Writing";
    case Gen3CartStepVerify:
        return "Verifying";
    case Gen3CartStepPostBackup:
        return "Saving backup";
    case Gen3CartStepRestoreCheck:
        return "Checking file";
    case Gen3CartStepRomRead:
        return "Copying ROM";
    case Gen3CartStepRomVerify:
        return "Checking ROM";
    case Gen3CartStepOmegaFind:
        return "Finding the game";
    case Gen3CartStepRomHash:
        return "Checking game file";
    }
    return "Working";
}

static int8_t cart_screen_percent(uint64_t done, uint64_t total) {
    if(total == 0) return -1;
    if(done > total) done = total;
    return (int8_t)(done * 100u / total);
}

void gen3_cart_screen_progress(
    const Gen3CartSessionStatus* st,
    Gen3CartScreenState state,
    Gen3CartScreenProgress* out) {
    out->title = "Starting";
    out->line1 = "";
    out->line2 = "";
    out->percent = -1;
    out->hint = "";
    if(state == Gen3CartScreenStopping) {
        out->title = "Stopping";
        return;
    }
    if(gen3_cart_screen_back(st->job) != Gen3CartBackIgnore) out->hint = CART_SCREEN_BACK_STOP;
    /* Until the worker takes the job, its step and cable flag may be the last job's */
    if(state == Gen3CartScreenStarting) return;

    const Gen3CartStep step = st->step;
    out->title = cart_screen_title(step);
    if(cart_screen_flashing(st->job, step)) out->hint = CART_SCREEN_NO_POWER_OFF;
    switch(step) {
    case Gen3CartStepNone:
    case Gen3CartStepMbWait:
        break;
    case Gen3CartStepErase:
    case Gen3CartStepProgram:
    case Gen3CartStepCommit: {
        /* Erase, program and commit are each a pass over the same sectors: one bar for all three */
        const uint64_t pass = (uint64_t)(step - Gen3CartStepErase);
        out->percent = cart_screen_percent(pass * st->total + st->done, 3u * (uint64_t)st->total);
        break;
    }
    default:
        out->percent = cart_screen_percent(st->done, st->total);
        break;
    }
    if(step == Gen3CartStepBackupRead) out->line2 = "about 70 s";
    if(step != Gen3CartStepNone && !st->cable_ok) {
        out->line1 = CART_SCREEN_CABLE;
    } else if(gen3_cart_screen_power_hint(st)) {
        out->line1 = "Hold START+SELECT";
        out->line2 = "and power on the GBA";
    }
}

bool gen3_cart_screen_power_hint(const Gen3CartSessionStatus* st) {
    return st->busy && st->job == Gen3CartJobConnect && st->step == Gen3CartStepMbWait &&
           st->cable_ok && st->mb.waiting_us >= GEN3_CART_SCREEN_HINT_US && st->mb.restarts == 0 &&
           st->mb.last_fail == Gen3MbFailNone;
}

void gen3_cart_screen_hub_in(const Gen3Cart* c, Gen3CartScreenHubIn* in) {
    memset(in, 0, sizeof(*in));
    in->busy = c == NULL;
    if(!c) return;
    in->linked = gen3_cart_linked(c);
    in->backup_current = gen3_cart_backup_current(c);
    in->can_read_save = gen3_cart_id(c)->can_read_save;
    in->browsable = gen3_cart_browsable(c);
    in->writable = gen3_cart_writable(c, &in->why);
    in->work_ok = gen3_cart_work_ok(c);
    in->restorable = gen3_cart_restorable(c);
    in->has_rom = gen3_cart_rom_size(c) > 0;
    in->changes = gen3_cart_changes(c);
    in->pending = gen3_cart_pending(c);
}

void gen3_cart_screen_hub(const Gen3CartScreenHubIn* in, Gen3CartScreenHub* out) {
    memset(out, 0, sizeof(*out));
    if(in->busy) return;
    out->browse = in->browsable;
    /* a write needs the helper: unlinked, Reconnect comes first */
    out->write = in->linked && in->writable && in->changes > 0;
    /* The save as backed up is Save's cause only with a current backup, and a working copy unless
     * there are no edits: an empty or corrupt save has neither, and is why */
    out->writing_off = !in->writable &&
                       (in->why != Gen3CartRefuseSave ||
                        (in->backup_current && (in->work_ok || in->changes == 0)));
    /* a discard rebuilds the working copy from the backup, and ends a write pending, edits or not
     * (a restore's), which keeps the list's Pokemon shut; a cart with no list (no save the game
     * loads) has nothing it keeps shut, nor a save for a discard to rebuild */
    out->discard = in->backup_current && (in->changes > 0 || (in->pending && in->browsable));
    out->reconnect = !in->linked;
    out->backup = !in->backup_current && in->can_read_save;
    out->restore = in->linked && in->restorable;
    out->rom = in->linked && in->has_rom;
}

/* A job Back stopped: what it was, not that it failed */
static const char* cart_screen_stopped(Gen3CartJob job) {
    switch(job) {
    case Gen3CartJobConnect:
        return "Connect stopped";
    case Gen3CartJobBackup:
        return "Backup stopped";
    case Gen3CartJobWrite:
        return "Write stopped";
    case Gen3CartJobDiscard:
        return "Discard stopped";
    case Gen3CartJobRestoreCheck:
        return "Check stopped";
    case Gen3CartJobRestore:
        return "Restore stopped";
    case Gen3CartJobRom:
        return "ROM copy stopped";
    case Gen3CartJobReconnect:
        return "Reconnect halted"; /* "Reconnect stopped" is over 16 characters */
    }
    return "Stopped";
}

static const char* cart_screen_header(Gen3CartJob job, Gen3CartResult r) {
    const bool ok = r == Gen3CartOk;
    const bool written = ok || r == Gen3CartWrittenNoBackup;
    if(r == Gen3CartCancelled) return cart_screen_stopped(job);
    switch(job) {
    case Gen3CartJobConnect:
        return ok ? "Connected" : "Connect failed";
    case Gen3CartJobBackup:
        return ok ? "Backed up" : "Backup failed";
    case Gen3CartJobWrite:
        return written ? "Written" : "Write failed";
    case Gen3CartJobDiscard:
        /* Changed: the cart wasn't as backed up, and the edits went with the backup */
        return ok || r == Gen3CartChanged ? "Discarded" : "Discard failed";
    case Gen3CartJobRestoreCheck:
        return ok ? "Backup checked" : "Can't restore";
    case Gen3CartJobRestore:
        return written ? "Restored" : "Restore failed";
    case Gen3CartJobRom:
        return ok ? "ROM copied" : "ROM copy failed";
    case Gen3CartJobReconnect:
        return ok ? "Reconnected" : "Reconnect failed";
    }
    return "Cartridge";
}

static Gen3CartRetry cart_screen_retry(Gen3CartJob job, Gen3CartResult r, bool linked) {
    switch(r) {
    case Gen3CartOk:
    case Gen3CartWrittenNoBackup:
    case Gen3CartNothing:
    case Gen3CartCancelled:
        return Gen3CartRetryNone;
    default:
        break;
    }
    /* A Connect that failed, or a Reconnect the helper didn't answer: send the helper again. A
     * Reconnect that found another save ran to its end: the hub offers a new backup. */
    if(job == Gen3CartJobConnect) return Gen3CartRetryConnect;
    if(job == Gen3CartJobReconnect) {
        return r == Gen3CartChanged ? Gen3CartRetryNone : Gen3CartRetryConnect;
    }
    /* The helper may still be busy with the lost command, or did something the client couldn't
     * take in: a Reconnect waits for it, and the job's next run reads the cart */
    if(r == Gen3CartLinkLost || r == Gen3CartBusyTimeout || !linked) {
        return Gen3CartRetryReconnect;
    }
    return Gen3CartRetryNone;
}

void gen3_cart_screen_result(
    Gen3CartJob job,
    Gen3CartStep step,
    Gen3CartResult r,
    bool dropped,
    bool linked,
    Gen3CartScreenResult* out) {
    out->header = cart_screen_header(job, r);
    out->text = gen3_cart_result_text(r);
    out->retry = cart_screen_retry(job, r, linked);
    if(job == Gen3CartJobConnect && r == Gen3CartOk && dropped) {
        out->header = "Edits dropped";
        out->text = "The save on the cart\nchanged.";
    }
    if(job == Gen3CartJobWrite && r == Gen3CartOk) {
        out->text = "Written and verified.\nPower-cycle the GBA\nto play.";
    }
    if(job == Gen3CartJobRestore && r == Gen3CartOk) {
        out->text = "Restored and\nverified. Power-cycle\nthe GBA to play.";
    }
    /* Its name follows on screen (gen3_cart_screen_rom_done) */
    if(job == Gen3CartJobRom && r == Gen3CartOk) out->text = "Verified.";
    /* The .part keeps every region already checked */
    if(job == Gen3CartJobRom && r == Gen3CartCancelled) out->text = "Copy ROM again\nto resume.";
    /* Each commit makes one more sector of the new slot valid, and after the last the game loads
     * that slot: once a commit may have gone out, the cart may load either save, and the write's
     * next run, after a Reconnect, finds which */
    const bool may_be_written = (job == Gen3CartJobWrite || job == Gen3CartJobRestore) &&
                                (step == Gen3CartStepCommit || step == Gen3CartStepVerify ||
                                 step == Gen3CartStepPostBackup);
    if(may_be_written && r == Gen3CartLinkLost) {
        out->text = "Link lost. The cart\nmay load either save.\nRetry, then redo it.";
    } else if(may_be_written && r == Gen3CartBusyTimeout) {
        out->text = "No answer. The cart\nmay load either save.\nRetry, then redo it.";
    }
}

/* Appends one line, cut to GEN3_CART_SCREEN_LINE_LEN, and a '\n' unless it is the last */
static size_t cart_screen_line(char* text, size_t pos, const char* line, bool last) {
    const int n = snprintf(
        text + pos,
        GEN3_CART_SCREEN_INFO_SIZE - pos,
        "%.*s%s",
        (int)GEN3_CART_SCREEN_LINE_LEN,
        line,
        last ? "" : "\n");
    if(n < 0) return pos;
    pos += (size_t)n;
    return pos < GEN3_CART_SCREEN_INFO_SIZE ? pos : GEN3_CART_SCREEN_INFO_SIZE - 1u;
}

static const char* cart_screen_chip(uint16_t flash_id) {
    switch(flash_id) {
    case GEN3_FLASH_ID_MX:
        return "Macronix";
    case GEN3_FLASH_ID_SANYO:
        return "Sanyo";
    default:
        return "?";
    }
}

/* "Save: B #41": the slot the game loads and its counter */
static void cart_screen_save(const Gen3CartScreenInfo* in, char line[24]) {
    const char* what = NULL;
    if(!in->id->can_read_save) {
        what = "Save: not read";
    } else if(!in->backup_current) {
        what = "Save: no backup";
    } else if(!in->slots) {
        what = "Save: layout unknown";
    } else {
        switch(in->slots->result) {
        case Gen3SaveOk:
        case Gen3SaveError:
            snprintf(
                line,
                24,
                "Save: %c #%lu",
                in->slots->active ? 'B' : 'A',
                (unsigned long)in->slots->save_counter);
            return;
        case Gen3SaveEmpty:
            what = "Save: empty";
            break;
        case Gen3SaveCorrupt:
            what = "Save: corrupt";
            break;
        }
    }
    snprintf(line, 24, "%s", what ? what : "Save: ?");
}

/* "Backup 1027-093015": MMDD-HHMMSS of the stamp in backups/<code>_<tid>_<stamp>.sav */
static void cart_screen_backup(const Gen3CartScreenInfo* in, char line[24]) {
    if(!in->backup_current) {
        snprintf(line, 24, "No backup");
        return;
    }
    const char* stamp = in->backup_name ? strrchr(in->backup_name, '_') : NULL;
    if(stamp && strlen(stamp + 1) >= CART_SCREEN_STAMP_LEN && stamp[9] == '-') {
        snprintf(line, 24, "Backup %.11s", stamp + 5);
    } else {
        snprintf(line, 24, "Backup made");
    }
}

void gen3_cart_screen_info(
    const Gen3CartScreenInfo* in,
    char header[GEN3_CART_SCREEN_HEADER_SIZE],
    char text[GEN3_CART_SCREEN_INFO_SIZE]) {
    const Gen3CartId* id = in->id;
    char line[24];
    size_t pos = 0;
    text[0] = '\0';

    snprintf(header, GEN3_CART_SCREEN_HEADER_SIZE, "%.4s v%u", id->code, (unsigned)id->version);

    snprintf(
        line,
        sizeof(line),
        "Flash %04X %s",
        (unsigned)id->flash_id,
        cart_screen_chip(id->flash_id));
    pos = cart_screen_line(text, pos, line, false);

    cart_screen_save(in, line);
    pos = cart_screen_line(text, pos, line, false);

    if(in->trainer) {
        snprintf(line, sizeof(line), "%.7s %05u", in->trainer->name, (unsigned)in->trainer->tid);
    } else {
        snprintf(line, sizeof(line), "No trainer");
    }
    pos = cart_screen_line(text, pos, line, false);

    pos = cart_screen_line(
        text, pos, in->writable ? "Writing on" : gen3_cart_refuse_text(in->why), false);

    cart_screen_backup(in, line);
    cart_screen_line(text, pos, line, true);
}

Gen3CartRetry gen3_cart_screen_omega_retry(Gen3CartJob job, Gen3CartResult r) {
    if(r == Gen3CartOk) return Gen3CartRetryNone;
    if(job == Gen3CartJobConnect) return Gen3CartRetryConnect;
    return r == Gen3CartLinkLost || r == Gen3CartBusyTimeout || r == Gen3CartHelperError ?
               Gen3CartRetryConnect :
               Gen3CartRetryNone;
}

void gen3_cart_screen_omega_info(
    const Gen3Omega* o,
    char header[GEN3_CART_SCREEN_HEADER_SIZE],
    char text[GEN3_CART_SCREEN_INFO_SIZE]) {
    char line[24];
    size_t pos = 0;
    text[0] = '\0';
    snprintf(header, GEN3_CART_SCREEN_HEADER_SIZE, "EZ-Flash Omega");

    pos = cart_screen_line(
        text,
        pos,
        !o->hashed  ? "Version: unknown" :
        o->verified ? "Unbound 2.1.1.1: yes" :
                      "Version: not 2.1.1.1",
        false);
    if(o->backup_current && o->slots.loaded >= 0) {
        snprintf(
            line,
            sizeof(line),
            "Save: %c #%lu",
            o->slots.loaded ? 'B' : 'A',
            (unsigned long)o->slots.counter[o->slots.loaded]);
    } else {
        snprintf(line, sizeof(line), "Save: none");
    }
    pos = cart_screen_line(text, pos, line, false);
    if(o->backup_current && o->has_trainer) {
        snprintf(line, sizeof(line), "%.7s %05u", o->trainer.name, (unsigned)o->trainer.tid);
    } else {
        snprintf(line, sizeof(line), "No trainer");
    }
    pos = cart_screen_line(text, pos, line, false);
    /* What the hub's Write changes needs: the game file 2.1.1.1, and a backup to write against */
    pos = cart_screen_line(
        text, pos, o->verified && gen3_omega_browsable(o) ? "Writing on" : "Writing off", false);
    const Gen3CartScreenInfo in = {
        .backup_current = o->backup_current,
        .backup_name = o->backup_name,
    };
    cart_screen_backup(&in, line);
    cart_screen_line(text, pos, line, true);
}

void gen3_cart_screen_ub_row(
    uint8_t k,
    const Gen3UbSummary* m,
    const char* name,
    Gen3CartMon r,
    char out[GEN3_CART_SCREEN_ROW_SIZE]) {
    const unsigned n = (unsigned)k + 1u;
    if(r == Gen3CartMonEmpty) {
        snprintf(out, GEN3_CART_SCREEN_ROW_SIZE, "%02u -- empty --", n);
    } else if(r != Gen3CartMonOk) {
        snprintf(out, GEN3_CART_SCREEN_ROW_SIZE, "%02u ?", n);
    } else if(m->egg) {
        snprintf(out, GEN3_CART_SCREEN_ROW_SIZE, "%02u EGG", n);
    } else if(m->level == 0) {
        snprintf(out, GEN3_CART_SCREEN_ROW_SIZE, "%02u %.10s Lv?", n, name);
    } else {
        snprintf(out, GEN3_CART_SCREEN_ROW_SIZE, "%02u %.10s Lv%u", n, name, (unsigned)m->level);
    }
}

static const char* cart_screen_changes(uint16_t changes) {
    return changes == 1 ? "change" : "changes";
}

void gen3_cart_screen_leave(uint16_t changes, char out[GEN3_CART_SCREEN_LEAVE_SIZE]) {
    snprintf(
        out,
        GEN3_CART_SCREEN_LEAVE_SIZE,
        "%u %s not\nwritten.",
        (unsigned)changes,
        cart_screen_changes(changes));
}

/* ---- The party and box screens ---- */

/* What settles a write or restore pending, or ends it, as the hub offers it: Reconnect (unlinked:
 * the reconnect finishes one done on the cart); Write changes, which writes the edits again; with
 * neither, while linked, what is pending has no edits, so it is a restore (a write keeps its edits
 * until it ends), which Restore backup redoes; and Discard changes, which ends either. */
static const char* cart_screen_pending(const Gen3CartScreenHub* hub) {
    if(!hub || !hub->discard) return "Finish the write\nfirst.";
    if(hub->reconnect) return "Finish the write\nfirst: Reconnect or\nDiscard changes.";
    if(hub->write) return "Finish the write\nfirst: Write changes\nor Discard changes.";
    if(hub->restore) return "Finish the restore\nfirst: Restore backup\nor Discard changes.";
    return "Finish the write\nfirst, or choose\nDiscard changes.";
}

const char* gen3_cart_screen_mon_text(Gen3CartMon m, const Gen3CartScreenHub* hub) {
    switch(m) {
    case Gen3CartMonOk:
    case Gen3CartMonEmpty:
        return "";
    case Gen3CartMonBadArg:
        return gen3_cart_result_text(Gen3CartBadArg);
    case Gen3CartMonSdError:
        return gen3_cart_result_text(Gen3CartSdError);
    case Gen3CartMonTaken:
        return "That slot isn't\nfree.";
    case Gen3CartMonMail:
        return "Take the Mail off\nin the game first.";
    case Gen3CartMonItem:
        return "It holds an item\nthe Flipper can't\nwrite back.";
    case Gen3CartMonEgg:
        return "Eggs can't be\nedited.";
    case Gen3CartMonBadEgg:
        return "Bad Egg: left as is.";
    case Gen3CartMonBadData:
        return "Unusual data:\nleft as is.";
    case Gen3CartMonNoData:
        return "Gen III data\nunreadable.";
    case Gen3CartMonPending:
        return cart_screen_pending(hub);
    }
    return "";
}

void gen3_cart_screen_row(
    uint8_t slot,
    const Gen3MonSummary* s,
    const char* species,
    bool changed,
    char out[GEN3_CART_SCREEN_ROW_SIZE]) {
    const unsigned k = (unsigned)slot + 1u;
    const char* mark = changed ? "*" : "";
    if(!s) {
        snprintf(out, GEN3_CART_SCREEN_ROW_SIZE, "%02u ?%s", k, mark);
        return;
    }
    switch(s->kind) {
    case Gen3MonOk:
        snprintf(
            out,
            GEN3_CART_SCREEN_ROW_SIZE,
            "%02u %.10s Lv%u%s",
            k,
            species,
            (unsigned)s->level,
            mark);
        return;
    case Gen3MonEmpty:
        snprintf(out, GEN3_CART_SCREEN_ROW_SIZE, "%02u -- empty --%s", k, mark);
        return;
    case Gen3MonEgg:
        snprintf(out, GEN3_CART_SCREEN_ROW_SIZE, "%02u EGG%s", k, mark);
        return;
    case Gen3MonBad:
        break;
    }
    snprintf(out, GEN3_CART_SCREEN_ROW_SIZE, "%02u BAD EGG%s", k, mark);
}

void gen3_cart_screen_put(
    const char* species,
    uint8_t level,
    const char* box,
    uint8_t slot,
    char header[GEN3_CART_SCREEN_HEADER_SIZE],
    char text[GEN3_CART_SCREEN_PUT_SIZE]) {
    const unsigned k = (unsigned)slot + 1u;
    char lv[8];
    char whole[24]; /* "Put CHARMANDER Lv100": the header with the level, if it fits */
    snprintf(lv, sizeof(lv), "Lv%u", (unsigned)level);
    const int n = snprintf(whole, sizeof(whole), "Put %.10s %s", species, lv);
    if(n > 0 && (size_t)n <= GEN3_CART_SCREEN_TITLE_LEN) {
        memcpy(header, whole, (size_t)n + 1u);
        snprintf(text, GEN3_CART_SCREEN_PUT_SIZE, "in %.8s\nslot %u?", box, k);
    } else {
        snprintf(header, GEN3_CART_SCREEN_HEADER_SIZE, "Put %.10s", species);
        snprintf(text, GEN3_CART_SCREEN_PUT_SIZE, "%s in %.8s\nslot %u?", lv, box, k);
    }
}

void gen3_cart_screen_write_ask(uint16_t changes, char out[GEN3_CART_SCREEN_ASK_SIZE]) {
    snprintf(
        out,
        GEN3_CART_SCREEN_ASK_SIZE,
        "%u %s. The save\nis backed up. Keep\nthe GBA on.",
        (unsigned)changes,
        cart_screen_changes(changes));
}

void gen3_cart_screen_discard_ask(uint16_t changes, char out[GEN3_CART_SCREEN_ASK_SIZE]) {
    snprintf(
        out, GEN3_CART_SCREEN_ASK_SIZE, "%u %s.", (unsigned)changes, cart_screen_changes(changes));
}

const char* gen3_cart_screen_check_text(Gen3Check c) {
    /* the one check text over a line: "Gen III data unreadable" */
    if(c == Gen3CheckNoData) return "Gen III data\nunreadable";
    return gen3_check_text(c);
}

Gen3CartSlotDo gen3_cart_screen_slot_entry(uint32_t state, Gen3CartEditMode mode) {
    bool made = false;
    switch(mode) {
    case Gen3CartEditExisting:
        if(state == Gen3CartSlotEdit) return Gen3CartSlotDoClose;
        return Gen3CartSlotDoOpen;
    case Gen3CartEditNew:
    case Gen3CartEditLoaded:
        made = true;
        break;
    case Gen3CartEditNone:
        break;
    }
    if(made && state == Gen3CartSlotEdit) return Gen3CartSlotDoPut;
    if(made && state == Gen3CartSlotReedit) return Gen3CartSlotDoEdit;
    return Gen3CartSlotDoOpen;
}

/* ---- Restore and ROM copy ---- */

Gen3CartScreenName gen3_cart_screen_restore_name(
    const char* path,
    const char* dir,
    char name[GEN3_CART_NAME_MAX]) {
    const size_t n = strlen(dir);
    if(strncmp(path, dir, n) != 0 || path[n] != '/' || path[n + 1u] == '\0') {
        return Gen3CartScreenNameOutside;
    }
    const char* rest = path + n + 1u;
    const size_t len = strlen(rest);
    if(len >= GEN3_CART_NAME_MAX) return Gen3CartScreenNameTooLong;
    memcpy(name, rest, len + 1u);
    return Gen3CartScreenNameOk;
}

const char* gen3_cart_screen_restore_name_text(Gen3CartScreenName n) {
    switch(n) {
    case Gen3CartScreenNameOk:
        break;
    case Gen3CartScreenNameOutside:
        return "Pick a file in\ngen3/backups.";
    case Gen3CartScreenNameTooLong:
        return "File name too long.";
    }
    return "";
}

/* "Cart: BRENDAN 12345", or "Cart: no save" */
static int
    cart_screen_trainer(char* out, size_t size, const char* what, const Gen3SaveTrainer* t) {
    if(!t) return snprintf(out, size, "%s: no save", what);
    return snprintf(out, size, "%s: %.7s %05u", what, t->name, (unsigned)t->tid);
}

/* Appends what snprintf wrote at out + pos, keeping pos inside the buffer */
static size_t cart_screen_at(size_t pos, int n, size_t size) {
    if(n < 0) return pos;
    pos += (size_t)n;
    return pos < size ? pos : size - 1u;
}

void gen3_cart_screen_restore_ask(
    const Gen3SaveTrainer* cart,
    const Gen3SaveTrainer* file,
    uint16_t changes,
    char out[GEN3_CART_SCREEN_ASK_SIZE]) {
    const size_t size = GEN3_CART_SCREEN_ASK_SIZE;
    size_t pos = cart_screen_at(0, cart_screen_trainer(out, size, "Cart", cart), size);
    pos = cart_screen_at(pos, snprintf(out + pos, size - pos, "\n"), size);
    pos = cart_screen_at(pos, cart_screen_trainer(out + pos, size - pos, "File", file), size);
    pos = cart_screen_at(pos, snprintf(out + pos, size - pos, "\nHall of Fame is kept."), size);
    if(changes > 0) {
        snprintf(
            out + pos,
            size - pos,
            "\n%u %s lost.",
            (unsigned)changes,
            changes == 1 ? "change is" : "changes are");
    }
}

/* "2 h 30 min", or "40 min" under an hour */
static void cart_screen_h_min(char* out, size_t size, uint32_t minutes) {
    if(minutes < 60u) {
        snprintf(out, size, "%lu min", (unsigned long)minutes);
    } else {
        snprintf(
            out,
            size,
            "%lu h %lu min",
            (unsigned long)(minutes / 60u),
            (unsigned long)(minutes % 60u));
    }
}

void gen3_cart_screen_rom_ask(uint32_t rom_size, char out[GEN3_CART_SCREEN_ASK_SIZE]) {
    const uint32_t s = rom_size / GEN3_CART_SCREEN_ROM_RATE;
    char time[16];
    cart_screen_h_min(time, sizeof(time), (s + 300u) / 600u * 10u);
    snprintf(
        out,
        GEN3_CART_SCREEN_ASK_SIZE,
        "%lu MB, about\n%s.\nKeep both powered.",
        (unsigned long)(rom_size / 1048576u),
        time);
}

void gen3_cart_screen_rom_resume(
    uint32_t done,
    uint32_t rom_size,
    char out[GEN3_CART_SCREEN_ASK_SIZE]) {
    snprintf(
        out,
        GEN3_CART_SCREEN_ASK_SIZE,
        "%u%% done",
        (unsigned)(rom_size ? (uint64_t)done * 100u / rom_size : 0u));
}

void gen3_cart_screen_rom_restart(
    uint32_t done,
    uint32_t rom_size,
    char out[GEN3_CART_SCREEN_ASK_SIZE]) {
    snprintf(
        out,
        GEN3_CART_SCREEN_ASK_SIZE,
        "%u%% done is lost:\nthe copy starts\nagain from 0%%.",
        (unsigned)(rom_size ? (uint64_t)done * 100u / rom_size : 0u));
}

void gen3_cart_screen_rom_done(const char* name, char out[GEN3_CART_SCREEN_ASK_SIZE]) {
    snprintf(out, GEN3_CART_SCREEN_ASK_SIZE, "Verified.\n%.21s", name);
}

void gen3_cart_screen_time_left(
    uint32_t done0,
    uint32_t done,
    uint32_t total,
    uint32_t elapsed_ms,
    char out[GEN3_CART_SCREEN_LEFT_SIZE]) {
    out[0] = '\0';
    if(done <= done0 || done >= total || elapsed_ms < GEN3_CART_SCREEN_RATE_MS) return;
    /* the bytes left at the rate so far, in ms rounded up: under 2^32 * 2^32, so no overflow */
    const uint64_t copied = done - done0;
    const uint64_t left_ms = ((uint64_t)(total - done) * elapsed_ms + copied - 1u) / copied;
    const uint64_t minutes = (left_ms + 59999u) / 60000u;
    if(minutes >= 100u * 60u) {
        snprintf(out, GEN3_CART_SCREEN_LEFT_SIZE, "over 99 h left");
        return;
    }
    char time[16];
    cart_screen_h_min(time, sizeof(time), (uint32_t)minutes);
    snprintf(out, GEN3_CART_SCREEN_LEFT_SIZE, "%s left", time);
}

void gen3_cart_screen_wrap(char* text) {
    size_t skip = 0;
    while(text[skip] == '\n') skip++;
    if(skip) memmove(text, text + skip, strlen(text + skip) + 1u);
    /* the last space seen on this line, where it may break */
    size_t line = 0;
    char* space = NULL;
    for(char* p = text; *p; p++) {
        if(*p == '\n') {
            line = 0;
            space = NULL;
            continue;
        }
        if(*p == ' ') space = p;
        if(++line > GEN3_CART_SCREEN_LINE_LEN && space) {
            *space = '\n';
            line = (size_t)(p - space);
            space = NULL;
        }
    }
}

/* ---- An EZ-Flash Omega: the Unbound editor, and writing its changes ---- */

void gen3_cart_screen_omega_hub(const Gen3Omega* o, Gen3CartScreenHub* out) {
    memset(out, 0, sizeof(*out));
    if(!o) return;
    const bool changes = gen3_omega_changes(o) > 0;
    out->browse = gen3_omega_browsable(o);
    out->write = changes;
    out->discard = changes;
    /* a new backup drops the edits (gen3_omega_backup), so not while there are any */
    out->backup = o->connected && !changes;
    out->restore = gen3_omega_browsable(o);
}

void gen3_cart_screen_omega_result(Gen3CartJob job, Gen3CartResult r, Gen3CartScreenResult* out) {
    out->header = cart_screen_header(job, r);
    out->text = gen3_cart_result_text(r);
    out->retry = gen3_cart_screen_omega_retry(job, r);
    const bool slot_job = job == Gen3CartJobWrite || job == Gen3CartJobRestore;
    if(slot_job && r == Gen3CartWrittenShared) {
        out->header = job == Gen3CartJobWrite ? "Written" : "Restored";
        out->retry = Gen3CartRetryRestore;
    }
    if(job == Gen3CartJobWrite && r == Gen3CartOk) out->text = GEN3_CART_SCREEN_UB_WRITTEN;
    if(job == Gen3CartJobRestore && r == Gen3CartOk) out->text = GEN3_CART_SCREEN_UB_RESTORED;
}

/* The ability the record has: the species' in its slot, or its first where it has none there */
static void cart_screen_ub_ability(const Gen3UbMon* m, char out[GEN3_CART_SCREEN_UB_NAME_SIZE]) {
    uint8_t a[3] = {0, 0, 0};
    const uint8_t slot = gen3_ub_ability_slot(m);
    uint8_t ability = 0;
    if(gen3_ub_species_abilities(gen3_ub_mon_species(m), a)) {
        ability = slot < 3u && a[slot] != 0 ? a[slot] : a[0];
    }
    gen3_ub_ability_name(ability, out);
}

static const char* cart_screen_ub_ivs(const Gen3UbMon* m) {
    uint8_t iv[6];
    bool all31 = true, all0 = true;
    gen3_ub_mon_ivs(m, iv);
    for(uint8_t i = 0; i < 6u; i++) {
        all31 = all31 && iv[i] == 31u;
        all0 = all0 && iv[i] == 0u;
    }
    return all31 ? "all 31" : all0 ? "all 0" : "as loaded";
}

void gen3_cart_screen_ub_edit_rows(
    const Gen3UbEditor* e,
    char rows[GEN3_CART_SCREEN_UB_ROWS][GEN3_CART_SCREEN_ROW_SIZE]) {
    const size_t size = GEN3_CART_SCREEN_ROW_SIZE;
    const Gen3UbMon* m = &e->mon;
    char name[GEN3_CART_SCREEN_UB_NAME_SIZE];
    uint8_t nick[GEN3_UB_MON_NICK];
    uint16_t moves[4];
    uint8_t evs[6];

    gen3_ub_species_name(gen3_ub_mon_species(m), name);
    snprintf(rows[0], size, "Pokemon: %.10s", name);
    const uint8_t level = gen3_ub_mon_level(m);
    if(level) {
        snprintf(rows[1], size, "Level: %u", (unsigned)level);
    } else {
        snprintf(rows[1], size, "Level: ?");
    }
    gen3_ub_mon_nickname(m, nick);
    gen3_text_decode(nick, sizeof(nick), name, GEN3_UB_MON_NICK + 1u);
    snprintf(rows[2], size, "Nickname: %.10s", name);
    const uint16_t item = gen3_ub_mon_item(m);
    if(item) {
        gen3_ub_item_name(item, name);
    } else {
        snprintf(name, sizeof(name), "None");
    }
    snprintf(rows[3], size, "Held item: %.12s", name);
    gen3_ub_mon_moves(m, moves);
    for(uint8_t i = 0; i < 4u; i++) {
        if(moves[i]) {
            gen3_ub_move_name(moves[i], name);
        } else {
            snprintf(name, sizeof(name), "-");
        }
        snprintf(rows[4u + i], size, "Move %c: %.12s", (char)('1' + i), name);
    }
    snprintf(rows[8], size, "Nature: %.15s", gen3_nature_name(gen3_ub_nature(m)));
    cart_screen_ub_ability(m, name);
    snprintf(rows[9], size, "Ability: %.14s", name);
    snprintf(rows[10], size, "Shiny: %s", gen3_ub_shiny(m) ? "Yes" : "No");
    snprintf(rows[11], size, "IVs: %s", cart_screen_ub_ivs(m));
    gen3_ub_mon_evs(m, evs);
    unsigned total = 0;
    for(uint8_t i = 0; i < 6u; i++) total += evs[i];
    snprintf(rows[12], size, "EVs: %u", total);
}

void gen3_cart_screen_ub_mark(bool changed, char row[GEN3_CART_SCREEN_ROW_SIZE]) {
    const size_t n = strlen(row);
    if(!changed || n + 1u >= GEN3_CART_SCREEN_ROW_SIZE) return;
    row[n] = '*';
    row[n + 1u] = '\0';
}

void gen3_cart_screen_ub_ev_label(uint8_t stat, uint8_t value, char out[GEN3_CART_SCREEN_ROW_SIZE]) {
    static const char names[6][4] = {"HP", "Atk", "Def", "Spe", "SpA", "SpD"};
    snprintf(
        out, GEN3_CART_SCREEN_ROW_SIZE, "%.3s: %u", stat < 6u ? names[stat] : "?", (unsigned)value);
}

void gen3_cart_screen_ub_trainer(const Gen3UbTrainer* in, Gen3SaveTrainer* out) {
    memset(out, 0, sizeof(*out));
    memcpy(out->name, in->name, sizeof(out->name));
    out->name[sizeof(out->name) - 1u] = '\0';
    out->gender = in->gender;
    out->tid = in->tid;
    out->sid = in->sid;
}

bool gen3_cart_screen_ub_sorted_name(
    Gen3UbSort s,
    uint16_t index,
    uint16_t* id,
    char out[GEN3_CART_SCREEN_UB_NAME_SIZE]) {
    out[0] = '\0';
    *id = gen3_ub_sorted(s, index);
    if(*id == 0) return false;
    switch(s) {
    case Gen3UbSortSpecies:
        return gen3_ub_species_name(*id, out);
    case Gen3UbSortMoves:
        return gen3_ub_move_name(*id, out);
    case Gen3UbSortItems:
        return gen3_ub_item_name(*id, out);
    }
    return false;
}

/* The first character of the name at index; 0 if it can't be read */
static char
    cart_screen_ub_first(Gen3UbSort s, uint16_t index, char name[GEN3_CART_SCREEN_UB_NAME_SIZE]) {
    uint16_t id;
    return gen3_cart_screen_ub_sorted_name(s, index, &id, name) ? name[0] : '\0';
}

/* The pages of the names from first to end, all starting with the same character; false if a name
 * can't be read or there is no room */
static bool cart_screen_ub_split(
    Gen3UbSort s,
    uint16_t first,
    uint16_t end,
    Gen3CartScreenUbPage* out,
    size_t max,
    size_t* n) {
    const uint32_t names = (uint32_t)end - first;
    const uint32_t pages =
        (names + GEN3_CART_SCREEN_UB_PAGE_MAX - 1u) / GEN3_CART_SCREEN_UB_PAGE_MAX;
    char a[GEN3_CART_SCREEN_UB_NAME_SIZE], b[GEN3_CART_SCREEN_UB_NAME_SIZE];
    for(uint32_t k = 0; k < pages; k++) {
        if(*n >= max) return false;
        Gen3CartScreenUbPage* p = &out[(*n)++];
        p->first = (uint16_t)(first + names * k / pages);
        p->count = (uint16_t)(first + names * (k + 1u) / pages - p->first);
        if(!cart_screen_ub_first(s, p->first, a)) return false;
        if(pages == 1u) {
            snprintf(p->label, sizeof(p->label), "%c", a[0]);
        } else {
            if(!cart_screen_ub_first(s, (uint16_t)(p->first + p->count - 1u), b)) return false;
            snprintf(p->label, sizeof(p->label), "%.3s-%.3s", a, b);
        }
    }
    return true;
}

size_t gen3_cart_screen_ub_pages(Gen3UbSort s, Gen3CartScreenUbPage* out, size_t max) {
    const uint16_t count = gen3_ub_sorted_count(s);
    char name[GEN3_CART_SCREEN_UB_NAME_SIZE];
    size_t n = 0;
    uint16_t i = 0;
    while(i < count) {
        const char c = cart_screen_ub_first(s, i, name);
        if(!c) return 0;
        /* The list is sorted by the names' bytes: c's names end where the first character changes */
        uint16_t lo = (uint16_t)(i + 1u), hi = count;
        while(lo < hi) {
            const uint16_t mid = (uint16_t)(lo + (hi - lo) / 2u);
            const char m = cart_screen_ub_first(s, mid, name);
            if(!m) return 0;
            if(m == c) {
                lo = (uint16_t)(mid + 1u);
            } else {
                hi = mid;
            }
        }
        if(!cart_screen_ub_split(s, i, lo, out, max, &n)) return 0;
        i = lo;
    }
    return n;
}

#define CART_SCREEN_UB_PREFIX "UNBOUND_"
#define CART_SCREEN_UB_SUFFIX ".sav"

bool gen3_cart_screen_ub_backup_ok(const char* name) {
    const size_t n = strlen(name);
    const size_t pre = sizeof(CART_SCREEN_UB_PREFIX) - 1u;
    const size_t suf = sizeof(CART_SCREEN_UB_SUFFIX) - 1u;
    const size_t dir = sizeof(GEN3_CART_BACKUP_DIR "/") - 1u;
    return n > pre + suf && dir + n < GEN3_CART_NAME_MAX &&
           strncmp(name, CART_SCREEN_UB_PREFIX, pre) == 0 &&
           strcmp(name + n - suf, CART_SCREEN_UB_SUFFIX) == 0 && strchr(name, '/') == NULL;
}

/* Where the stamp starts (after the last '_'), and its length up to ".sav" */
static const char* cart_screen_ub_stamp(const char* name, size_t* len) {
    const char* u = strrchr(name, '_');
    const char* start = u ? u + 1 : name;
    const char* dot = strrchr(start, '.');
    *len = dot ? (size_t)(dot - start) : strlen(start);
    return start;
}

void gen3_cart_screen_ub_backup_label(const char* name, char out[GEN3_CART_SCREEN_ROW_SIZE]) {
    const size_t pre = sizeof(CART_SCREEN_UB_PREFIX) - 1u;
    const char* mid = strncmp(name, CART_SCREEN_UB_PREFIX, pre) == 0 ? name + pre : name;
    const char* dot = strrchr(mid, '.');
    const int mid_len = (int)(dot ? (size_t)(dot - mid) : strlen(mid));
    const char* u = memchr(mid, '_', (size_t)mid_len);
    size_t len = 0;
    const char* stamp = cart_screen_ub_stamp(mid, &len);
    /* YYYYMMDD-HHMMSS, then "-2" .. if that name was taken */
    if(u && stamp > u && len >= CART_SCREEN_STAMP_LEN && stamp[8] == '-') {
        snprintf(
            out,
            GEN3_CART_SCREEN_ROW_SIZE,
            "%.*s %.*s",
            (int)(len - 4u > 13u ? 13u : len - 4u),
            stamp + 4,
            (int)(u - mid > 8 ? 8 : u - mid),
            mid);
    } else {
        snprintf(out, GEN3_CART_SCREEN_ROW_SIZE, "%.*s", mid_len, mid);
    }
}

bool gen3_cart_screen_ub_backup_newer(const char* a, const char* b) {
    size_t la = 0, lb = 0;
    const char* sa = cart_screen_ub_stamp(a, &la);
    const char* sb = cart_screen_ub_stamp(b, &lb);
    const int c = strncmp(sa, sb, la < lb ? la : lb);
    if(c != 0) return c > 0;
    if(la != lb) return la > lb;
    return strcmp(a, b) > 0;
}
