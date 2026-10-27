#include <src/gen3/gen3_cart_screen.h>

#include <stdio.h>
#include <string.h>

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
    case Gen3CartJobConnect:
    case Gen3CartJobBackup:
    case Gen3CartJobRestoreCheck:
    case Gen3CartJobRom:
    case Gen3CartJobReconnect:
        break;
    }
    return Gen3CartBackStop;
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
    if(gen3_cart_screen_back(st->job) == Gen3CartBackStop) out->hint = CART_SCREEN_BACK_STOP;
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

void gen3_cart_screen_hub(
    const Gen3CartSessionStatus* st,
    bool backup_current,
    bool can_read_save,
    Gen3CartScreenHub* out) {
    const bool idle = !st->busy;
    /* Only a reconnect that ran to its end (Ok, or Changed for another save) links the cart. One
     * stopped or failed after the helper answered its HELLO leaves link_up set. */
    const bool unlinked = st->job == Gen3CartJobReconnect && st->result != Gen3CartOk &&
                          st->result != Gen3CartChanged;
    out->reconnect = idle && (!st->link_up || unlinked);
    out->backup = idle && !backup_current && can_read_save;
}

static const char* cart_screen_header(Gen3CartJob job, Gen3CartResult r) {
    const bool ok = r == Gen3CartOk;
    const bool written = ok || r == Gen3CartWrittenNoBackup;
    switch(job) {
    case Gen3CartJobConnect:
        return ok ? "Connected" : "Connect failed";
    case Gen3CartJobBackup:
        return ok ? "Backed up" : "Backup failed";
    case Gen3CartJobWrite:
        return written ? "Written" : "Write failed";
    case Gen3CartJobDiscard:
        return ok ? "Discarded" : "Discard failed";
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

static Gen3CartRetry cart_screen_retry(Gen3CartJob job, Gen3CartResult r) {
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
    /* The helper may still be busy with the lost command: a Reconnect waits for it */
    if(r == Gen3CartLinkLost || r == Gen3CartBusyTimeout) return Gen3CartRetryReconnect;
    return Gen3CartRetryNone;
}

void gen3_cart_screen_result(
    Gen3CartJob job,
    Gen3CartStep step,
    Gen3CartResult r,
    bool dropped,
    Gen3CartScreenResult* out) {
    out->header = cart_screen_header(job, r);
    out->text = gen3_cart_result_text(r);
    out->retry = cart_screen_retry(job, r);
    if(job == Gen3CartJobConnect && r == Gen3CartOk && dropped) {
        out->header = "Edits dropped";
        out->text = "The save on the cart\nchanged.";
    }
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

void gen3_cart_screen_leave(uint16_t changes, char out[GEN3_CART_SCREEN_LEAVE_SIZE]) {
    snprintf(
        out,
        GEN3_CART_SCREEN_LEAVE_SIZE,
        "%u %s not\nwritten.",
        (unsigned)changes,
        changes == 1 ? "change" : "changes");
}
