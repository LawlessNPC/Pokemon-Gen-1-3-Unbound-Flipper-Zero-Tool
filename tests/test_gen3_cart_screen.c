#include "test.h"
#include <stdbool.h>
#include <stdio.h>
#include <src/gen3/gen3_cart.h>
#include <src/gen3/gen3_cart_id.h>
#include <src/gen3/gen3_cart_screen.h>
#include <src/gen3/gen3_mb.h>
#include <src/gen3/gen3_pk3.h>
#include <src/gen3/gen3_save.h>

/* The Cartridge app's screen texts and choices: what the progress screen shows for each job and
 * step, the power-on hint, the hub's items, the results and their Retry, Cart info, the leave
 * warning, and the party and box screens; and that every text fits the screen. */

#define ASSERT_FALSE(x) ASSERT_TRUE(!(x))
#define CABLE_TEXT "Cable: plug in/swap ends"

static const Gen3CartJob all_jobs[] = {
    Gen3CartJobConnect,
    Gen3CartJobBackup,
    Gen3CartJobWrite,
    Gen3CartJobDiscard,
    Gen3CartJobRestoreCheck,
    Gen3CartJobRestore,
    Gen3CartJobRom,
    Gen3CartJobReconnect,
};

/* The status is about 100 B: static, as the app keeps it */
static Gen3CartSessionStatus st;

static void status(Gen3CartJob job, Gen3CartStep step, uint32_t done, uint32_t total) {
    memset(&st, 0, sizeof(st));
    st.busy = true;
    st.job = job;
    st.step = step;
    st.done = done;
    st.total = total;
    st.cable_ok = true;
}

/* Lines broken with '\n': at most max_lines, each at most max_len characters, none empty unless
 * the whole text is */
static bool fits(const char* t, unsigned max_lines, size_t max_len) {
    if(!t) return false;
    unsigned lines = 1;
    size_t len = 0;
    for(const char* p = t; *p; p++) {
        if(*p == '\n') {
            if(len == 0) return false;
            lines++;
            len = 0;
        } else if(++len > max_len) {
            return false;
        }
    }
    return lines <= max_lines && (t[0] == '\0' || len > 0);
}

static unsigned line_count(const char* t) {
    unsigned n = 1;
    for(; *t; t++) n += *t == '\n';
    return n;
}

/* Line k (0-based) of t into out */
static void line_of(const char* t, unsigned k, char out[64]) {
    for(; k && *t; t++) {
        if(*t == '\n') k--;
    }
    size_t n = 0;
    while(t[n] && t[n] != '\n' && n < 63) n++;
    memcpy(out, t, n);
    out[n] = '\0';
}

TEST(test_back) {
    ASSERT_EQ(gen3_cart_screen_back(Gen3CartJobWrite), Gen3CartBackIgnore);
    ASSERT_EQ(gen3_cart_screen_back(Gen3CartJobRestore), Gen3CartBackIgnore);
    ASSERT_EQ(gen3_cart_screen_back(Gen3CartJobDiscard), Gen3CartBackIgnore);
    ASSERT_EQ(gen3_cart_screen_back(Gen3CartJobConnect), Gen3CartBackStop);
    ASSERT_EQ(gen3_cart_screen_back(Gen3CartJobBackup), Gen3CartBackStop);
    ASSERT_EQ(gen3_cart_screen_back(Gen3CartJobReconnect), Gen3CartBackStop);
    ASSERT_EQ(gen3_cart_screen_back(Gen3CartJobRestoreCheck), Gen3CartBackStop);
    ASSERT_EQ(gen3_cart_screen_back(Gen3CartJobRom), Gen3CartBackStop);
}

TEST(test_progress_connect_steps) {
    Gen3CartScreenProgress p;

    status(Gen3CartJobConnect, Gen3CartStepMbWait, 0, 8000);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Waiting for GBA");
    ASSERT_STR_EQ(p.line1, "");
    ASSERT_STR_EQ(p.line2, "");
    ASSERT_EQ(p.percent, -1);
    ASSERT_STR_EQ(p.hint, "Back: stop");

    status(Gen3CartJobConnect, Gen3CartStepMbSend, 4000, 8000);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Sending helper");
    ASSERT_EQ(p.percent, 50);
    ASSERT_STR_EQ(p.hint, "Back: stop");

    status(Gen3CartJobConnect, Gen3CartStepIdentify, 1, 3);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Reading cart");
    ASSERT_EQ(p.percent, 33);

    status(Gen3CartJobConnect, Gen3CartStepBackupRead, 8, 32);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Backing up save");
    ASSERT_STR_EQ(p.line2, "about 70 s");
    ASSERT_EQ(p.percent, 25);
    ASSERT_STR_EQ(p.hint, "Back: stop");

    status(Gen3CartJobConnect, Gen3CartStepBackupVerify, 64, 64);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Checking backup");
    ASSERT_EQ(p.percent, 100);

    status(Gen3CartJobConnect, Gen3CartStepAnalyze, 0, 1);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Reading save");

    /* No total yet: no bar */
    status(Gen3CartJobConnect, Gen3CartStepMbSend, 0, 0);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_EQ(p.percent, -1);
}

TEST(test_progress_cable) {
    Gen3CartScreenProgress p;
    status(Gen3CartJobConnect, Gen3CartStepMbWait, 0, 0);
    st.cable_ok = false;
    st.mb.waiting_us = 10000000u; /* even with the hint's wait over */
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Waiting for GBA");
    ASSERT_STR_EQ(p.line1, CABLE_TEXT);
    ASSERT_STR_EQ(p.line2, "");
    ASSERT_FALSE(gen3_cart_screen_power_hint(&st));

    /* While the driver runs, its cable guard: any step */
    status(Gen3CartJobBackup, Gen3CartStepBackupRead, 3, 32);
    st.cable_ok = false;
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.line1, CABLE_TEXT);
    ASSERT_STR_EQ(p.line2, "about 70 s");

    /* Starting: the cable flag may be the last job's */
    gen3_cart_screen_progress(&st, Gen3CartScreenStarting, &p);
    ASSERT_STR_EQ(p.line1, "");
}

TEST(test_power_hint) {
    Gen3CartScreenProgress p;

    status(Gen3CartJobConnect, Gen3CartStepMbWait, 0, 8000);
    st.mb.phase = Gen3MbPhaseDetect;
    st.mb.waiting_us = GEN3_CART_SCREEN_HINT_US - 1u;
    ASSERT_FALSE(gen3_cart_screen_power_hint(&st));
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.line1, "");

    st.mb.waiting_us = GEN3_CART_SCREEN_HINT_US;
    ASSERT_TRUE(gen3_cart_screen_power_hint(&st));
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Waiting for GBA");
    ASSERT_STR_EQ(p.line1, "Hold START+SELECT");
    ASSERT_STR_EQ(p.line2, "and power on the GBA");
    ASSERT_EQ(p.percent, -1);

    /* After any failure the GBA is on: never tell it to power on */
    st.mb.restarts = 1;
    ASSERT_FALSE(gen3_cart_screen_power_hint(&st));
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.line1, "");
    ASSERT_STR_EQ(p.line2, "");
    st.mb.restarts = 0;
    st.mb.last_fail = Gen3MbFailPalette; /* an uncounted one */
    ASSERT_FALSE(gen3_cart_screen_power_hint(&st));
    st.mb.last_fail = Gen3MbFailNone;
    ASSERT_TRUE(gen3_cart_screen_power_hint(&st));

    /* Only while waiting, and only for Connect */
    st.step = Gen3CartStepMbSend;
    ASSERT_FALSE(gen3_cart_screen_power_hint(&st));
    st.step = Gen3CartStepMbWait;
    st.job = Gen3CartJobReconnect;
    ASSERT_FALSE(gen3_cart_screen_power_hint(&st));
    st.job = Gen3CartJobConnect;
    st.busy = false;
    ASSERT_FALSE(gen3_cart_screen_power_hint(&st));
    st.busy = true;

    /* Starting: a Connect not yet taken shows nothing of the sender */
    gen3_cart_screen_progress(&st, Gen3CartScreenStarting, &p);
    ASSERT_STR_EQ(p.title, "Starting");
    ASSERT_STR_EQ(p.line1, "");
    ASSERT_STR_EQ(p.line2, "");
}

TEST(test_progress_ignores_link_up) {
    for(size_t j = 0; j < sizeof(all_jobs) / sizeof(all_jobs[0]); j++) {
        for(int step = Gen3CartStepNone; step <= Gen3CartStepRomVerify; step++) {
            for(int state = Gen3CartScreenStarting; state <= Gen3CartScreenStopping; state++) {
                Gen3CartScreenProgress a, b;
                status(all_jobs[j], (Gen3CartStep)step, 5, 10);
                st.mb.waiting_us = 5000000u;
                st.link_up = false;
                gen3_cart_screen_progress(&st, (Gen3CartScreenState)state, &a);
                st.link_up = true;
                gen3_cart_screen_progress(&st, (Gen3CartScreenState)state, &b);
                ASSERT_TRUE(a.title == b.title && a.line1 == b.line1 && a.line2 == b.line2);
                ASSERT_TRUE(a.hint == b.hint && a.percent == b.percent);
            }
        }
    }
}

TEST(test_progress_write) {
    Gen3CartScreenProgress p;
    const Gen3CartJob jobs[] = {Gen3CartJobWrite, Gen3CartJobRestore};
    for(size_t j = 0; j < 2; j++) {
        status(jobs[j], Gen3CartStepPrepare, 15, 15);
        gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
        ASSERT_STR_EQ(p.title, "Preparing");
        ASSERT_STR_EQ(p.hint, "");

        /* One bar for the three passes */
        status(jobs[j], Gen3CartStepErase, 7, 14);
        gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
        ASSERT_STR_EQ(p.title, "Writing");
        ASSERT_STR_EQ(p.hint, "Do not power off");
        ASSERT_EQ(p.percent, 16);
        status(jobs[j], Gen3CartStepProgram, 0, 14);
        gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
        ASSERT_STR_EQ(p.title, "Writing");
        ASSERT_STR_EQ(p.hint, "Do not power off");
        ASSERT_EQ(p.percent, 33);
        status(jobs[j], Gen3CartStepCommit, 14, 14);
        gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
        ASSERT_STR_EQ(p.title, "Writing");
        ASSERT_STR_EQ(p.hint, "Do not power off");
        ASSERT_EQ(p.percent, 100);

        status(jobs[j], Gen3CartStepVerify, 3, 15);
        gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
        ASSERT_STR_EQ(p.title, "Verifying");
        ASSERT_STR_EQ(p.hint, "Do not power off");
        ASSERT_EQ(p.percent, 20);

        status(jobs[j], Gen3CartStepPostBackup, 0, 96);
        gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
        ASSERT_STR_EQ(p.title, "Saving backup");
        ASSERT_STR_EQ(p.hint, "");
    }

    status(Gen3CartJobRestoreCheck, Gen3CartStepRestoreCheck, 14, 28);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Checking file");
    ASSERT_STR_EQ(p.hint, "Back: stop");
    ASSERT_EQ(p.percent, 50);

    status(Gen3CartJobRom, Gen3CartStepRomRead, 0x200000u, 0x1000000u);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Copying ROM");
    ASSERT_EQ(p.percent, 12);
    /* 32 MiB in bytes: no overflow */
    status(Gen3CartJobRom, Gen3CartStepRomVerify, 0x2000000u, 0x2000000u);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Checking ROM");
    ASSERT_EQ(p.percent, 100);

    status(Gen3CartJobDiscard, Gen3CartStepNone, 0, 0);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.hint, "");
    status(Gen3CartJobReconnect, Gen3CartStepIdentify, 0, 3);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Reading cart");
    ASSERT_STR_EQ(p.hint, "Back: stop");
}

TEST(test_progress_starting_stopping) {
    Gen3CartScreenProgress p;
    /* The step is the last job's until the worker takes the new one */
    status(Gen3CartJobBackup, Gen3CartStepIdentify, 3, 3);
    gen3_cart_screen_progress(&st, Gen3CartScreenStarting, &p);
    ASSERT_STR_EQ(p.title, "Starting");
    ASSERT_STR_EQ(p.line1, "");
    ASSERT_STR_EQ(p.line2, "");
    ASSERT_EQ(p.percent, -1);
    ASSERT_STR_EQ(p.hint, "Back: stop");
    status(Gen3CartJobWrite, Gen3CartStepIdentify, 3, 3);
    gen3_cart_screen_progress(&st, Gen3CartScreenStarting, &p);
    ASSERT_STR_EQ(p.hint, "");

    status(Gen3CartJobConnect, Gen3CartStepBackupRead, 8, 32);
    gen3_cart_screen_progress(&st, Gen3CartScreenStopping, &p);
    ASSERT_STR_EQ(p.title, "Stopping");
    ASSERT_STR_EQ(p.line1, "");
    ASSERT_STR_EQ(p.line2, "");
    ASSERT_EQ(p.percent, -1);
    ASSERT_STR_EQ(p.hint, "");
}

TEST(test_progress_texts_fit) {
    for(size_t j = 0; j < sizeof(all_jobs) / sizeof(all_jobs[0]); j++) {
        for(int step = Gen3CartStepNone; step <= Gen3CartStepRomVerify; step++) {
            for(int state = Gen3CartScreenStarting; state <= Gen3CartScreenStopping; state++) {
                for(int k = 0; k < 4; k++) {
                    Gen3CartScreenProgress p;
                    status(all_jobs[j], (Gen3CartStep)step, 10, 10);
                    st.cable_ok = k & 1;
                    st.mb.waiting_us = (k & 2) ? GEN3_CART_SCREEN_HINT_US : 0;
                    gen3_cart_screen_progress(&st, (Gen3CartScreenState)state, &p);
                    ASSERT_TRUE(p.title[0] != '\0');
                    ASSERT_TRUE(fits(p.title, 1, GEN3_CART_SCREEN_TITLE_LEN));
                    ASSERT_TRUE(
                        strcmp(p.line1, CABLE_TEXT) == 0 ||
                        fits(p.line1, 1, GEN3_CART_SCREEN_LINE_LEN));
                    ASSERT_TRUE(fits(p.line2, 1, GEN3_CART_SCREEN_LINE_LEN));
                    ASSERT_TRUE(fits(p.hint, 1, GEN3_CART_SCREEN_LINE_LEN));
                    ASSERT_TRUE(p.percent >= -1 && p.percent <= 100);
                }
            }
        }
    }
}

/* A linked cart, backed up, browsable and writable, with no change */
static Gen3CartScreenHubIn hub_in(void) {
    return (Gen3CartScreenHubIn){
        .busy = false,
        .linked = true,
        .backup_current = true,
        .can_read_save = true,
        .browsable = true,
        .writable = true,
        .changes = 0,
    };
}

static void hub(const Gen3CartScreenHubIn* in, Gen3CartScreenHub* h) {
    memset(h, 0xA5, sizeof(*h));
    gen3_cart_screen_hub(in, h);
}

TEST(test_hub) {
    Gen3CartScreenHub h;
    Gen3CartScreenHubIn in = hub_in();

    hub(&in, &h);
    ASSERT_TRUE(h.browse);
    ASSERT_FALSE(h.write);
    ASSERT_FALSE(h.writing_off);
    ASSERT_FALSE(h.discard);
    ASSERT_FALSE(h.reconnect);
    ASSERT_FALSE(h.backup);

    /* Changes: write them, or discard them */
    in.changes = 2;
    hub(&in, &h);
    ASSERT_TRUE(h.write);
    ASSERT_TRUE(h.discard);
    ASSERT_FALSE(h.writing_off);

    /* Not writable: why not, instead of Write, and Discard still */
    in.writable = false;
    hub(&in, &h);
    ASSERT_FALSE(h.write);
    ASSERT_TRUE(h.writing_off);
    ASSERT_TRUE(h.discard);
    in.changes = 0;
    hub(&in, &h);
    ASSERT_TRUE(h.writing_off);
    ASSERT_FALSE(h.discard);

    /* Not browsable (no working copy, or no layout): no list */
    in = hub_in();
    in.browsable = false;
    hub(&in, &h);
    ASSERT_FALSE(h.browse);

    /* Not linked (a command lost, or a Reconnect stopped before its end): Reconnect */
    in = hub_in();
    in.linked = false;
    hub(&in, &h);
    ASSERT_TRUE(h.reconnect);
    ASSERT_TRUE(h.browse);

    /* No current backup of a save the helper can read: Back up again */
    in = hub_in();
    in.backup_current = false;
    in.browsable = false;
    in.writable = false;
    hub(&in, &h);
    ASSERT_TRUE(h.backup);
    ASSERT_FALSE(h.reconnect);
    /* A cart whose save can't be read reaches the hub with no backup: nothing to back up */
    in.can_read_save = false;
    hub(&in, &h);
    ASSERT_FALSE(h.backup);
    ASSERT_TRUE(h.writing_off);

    /* While a job runs, the cart isn't read: none of them */
    in = hub_in();
    in.busy = true;
    in.linked = false;
    in.backup_current = false;
    in.changes = 3;
    hub(&in, &h);
    ASSERT_FALSE(h.browse);
    ASSERT_FALSE(h.write);
    ASSERT_FALSE(h.writing_off);
    ASSERT_FALSE(h.discard);
    ASSERT_FALSE(h.reconnect);
    ASSERT_FALSE(h.backup);
}

TEST(test_result_retry) {
    Gen3CartScreenResult r;

    /* Connect: every failure goes back to the instructions */
    for(int res = Gen3CartLinkLost; res <= Gen3CartLinkBusy; res++) {
        gen3_cart_screen_result(
            Gen3CartJobConnect, Gen3CartStepMbWait, (Gen3CartResult)res, false, true, &r);
        ASSERT_EQ(r.retry, Gen3CartRetryConnect);
        ASSERT_STR_EQ(r.header, "Connect failed");
        ASSERT_STR_EQ(r.text, gen3_cart_result_text((Gen3CartResult)res));
    }
    gen3_cart_screen_result(Gen3CartJobConnect, Gen3CartStepAnalyze, Gen3CartOk, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);

    /* A lost link: Reconnect, and a Reconnect that fails sends the helper again */
    gen3_cart_screen_result(
        Gen3CartJobBackup, Gen3CartStepBackupRead, Gen3CartLinkLost, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryReconnect);
    ASSERT_STR_EQ(r.header, "Backup failed");
    gen3_cart_screen_result(
        Gen3CartJobWrite, Gen3CartStepErase, Gen3CartBusyTimeout, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryReconnect);
    ASSERT_STR_EQ(r.header, "Write failed");
    gen3_cart_screen_result(
        Gen3CartJobRom, Gen3CartStepRomRead, Gen3CartLinkLost, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryReconnect);
    gen3_cart_screen_result(
        Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartLinkLost, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryConnect);
    ASSERT_STR_EQ(r.header, "Reconnect failed");
    gen3_cart_screen_result(
        Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartBusyTimeout, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryConnect);
    gen3_cart_screen_result(
        Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartHelperError, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryConnect);

    /* A Reconnect that ran to its end and found another save, or was stopped: no Retry */
    gen3_cart_screen_result(
        Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartChanged, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(
        Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartCancelled, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(
        Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartOk, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    ASSERT_STR_EQ(r.header, "Reconnected");
    ASSERT_STR_EQ(r.text, "Done.");

    /* Any other result: OK only */
    gen3_cart_screen_result(
        Gen3CartJobWrite, Gen3CartStepVerify, Gen3CartVerifyFailed, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(
        Gen3CartJobBackup, Gen3CartStepBackupRead, Gen3CartSdFull, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(
        Gen3CartJobBackup, Gen3CartStepBackupRead, Gen3CartCancelled, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    ASSERT_STR_EQ(r.text, "Stopped.");
    gen3_cart_screen_result(Gen3CartJobBackup, Gen3CartStepAnalyze, Gen3CartOk, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    ASSERT_STR_EQ(r.header, "Backed up");
}

TEST(test_result_after_commit) {
    Gen3CartScreenResult r;
    const Gen3CartJob jobs[] = {Gen3CartJobWrite, Gen3CartJobRestore};
    const Gen3CartStep late[] = {Gen3CartStepCommit, Gen3CartStepVerify, Gen3CartStepPostBackup};
    const Gen3CartStep early[] = {Gen3CartStepPrepare, Gen3CartStepErase, Gen3CartStepProgram};
    for(size_t j = 0; j < 2; j++) {
        /* Before any commit the game still loads the old slot: the job layer's own text */
        for(size_t k = 0; k < 3; k++) {
            gen3_cart_screen_result(jobs[j], early[k], Gen3CartLinkLost, false, true, &r);
            ASSERT_STR_EQ(r.text, gen3_cart_result_text(Gen3CartLinkLost));
            gen3_cart_screen_result(jobs[j], early[k], Gen3CartBusyTimeout, false, true, &r);
            ASSERT_STR_EQ(r.text, gen3_cart_result_text(Gen3CartBusyTimeout));
        }
        /* After a commit it may load the new one: never claim the old one */
        for(size_t k = 0; k < 3; k++) {
            gen3_cart_screen_result(jobs[j], late[k], Gen3CartLinkLost, false, true, &r);
            ASSERT_STR_EQ(
                r.text, "Link lost. The cart\nmay load either save.\nRetry, then redo it.");
            ASSERT_EQ(r.retry, Gen3CartRetryReconnect);
            ASSERT_TRUE(strstr(r.text, "still") == NULL);
            gen3_cart_screen_result(jobs[j], late[k], Gen3CartBusyTimeout, false, true, &r);
            ASSERT_STR_EQ(
                r.text, "No answer. The cart\nmay load either save.\nRetry, then redo it.");
            ASSERT_EQ(r.retry, Gen3CartRetryReconnect);
            /* Other results keep their own text */
            gen3_cart_screen_result(jobs[j], late[k], Gen3CartVerifyFailed, false, true, &r);
            ASSERT_STR_EQ(r.text, gen3_cart_result_text(Gen3CartVerifyFailed));
        }
    }
    /* Only a write can have committed */
    gen3_cart_screen_result(
        Gen3CartJobBackup, Gen3CartStepVerify, Gen3CartLinkLost, false, true, &r);
    ASSERT_STR_EQ(r.text, gen3_cart_result_text(Gen3CartLinkLost));
}

TEST(test_result_dropped) {
    Gen3CartScreenResult r;
    gen3_cart_screen_result(Gen3CartJobConnect, Gen3CartStepAnalyze, Gen3CartOk, true, true, &r);
    ASSERT_STR_EQ(r.header, "Edits dropped");
    ASSERT_STR_EQ(r.text, "The save on the cart\nchanged.");
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    /* dropped is Connect's only */
    gen3_cart_screen_result(Gen3CartJobBackup, Gen3CartStepAnalyze, Gen3CartOk, true, true, &r);
    ASSERT_STR_EQ(r.header, "Backed up");
}

TEST(test_result_texts_fit) {
    for(size_t j = 0; j < sizeof(all_jobs) / sizeof(all_jobs[0]); j++) {
        for(int res = Gen3CartOk; res <= Gen3CartLinkBusy; res++) {
            for(int step = Gen3CartStepNone; step <= Gen3CartStepRomVerify; step++) {
                for(int flags = 0; flags < 4; flags++) {
                    Gen3CartScreenResult r;
                    gen3_cart_screen_result(
                        all_jobs[j],
                        (Gen3CartStep)step,
                        (Gen3CartResult)res,
                        flags & 1,
                        flags & 2,
                        &r);
                    ASSERT_TRUE(r.header && r.header[0] != '\0');
                    ASSERT_TRUE(fits(r.header, 1, GEN3_CART_SCREEN_TITLE_LEN));
                    ASSERT_TRUE(r.text && r.text[0] != '\0');
                    ASSERT_TRUE(fits(r.text, 3, GEN3_CART_SCREEN_LINE_LEN));
                }
            }
        }
    }
}

/* A job that left the cart unlinked (a write-class response the client couldn't take, say) offers
 * Reconnect, whatever its result; one stopped by Back offers nothing, and the hub has Reconnect */
TEST(test_result_retry_unlinked) {
    Gen3CartScreenResult r;
    gen3_cart_screen_result(
        Gen3CartJobWrite, Gen3CartStepProgram, Gen3CartHelperError, false, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryReconnect);
    ASSERT_STR_EQ(r.header, "Write failed");
    gen3_cart_screen_result(
        Gen3CartJobWrite, Gen3CartStepProgram, Gen3CartHelperError, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(
        Gen3CartJobBackup, Gen3CartStepBackupRead, Gen3CartCancelled, false, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(Gen3CartJobDiscard, Gen3CartStepNone, Gen3CartOk, false, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    /* Connect and Reconnect keep their own */
    gen3_cart_screen_result(
        Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartChanged, false, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(
        Gen3CartJobConnect, Gen3CartStepMbWait, Gen3CartMultibootFailed, false, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryConnect);
}

/* Back stopped it: a neutral header, not "failed" */
TEST(test_result_stopped) {
    static const struct {
        Gen3CartJob job;
        const char* header;
    } want[] = {
        {Gen3CartJobConnect, "Connect stopped"},
        {Gen3CartJobBackup, "Backup stopped"},
        {Gen3CartJobRestoreCheck, "Check stopped"},
        {Gen3CartJobRom, "ROM copy stopped"},
        {Gen3CartJobReconnect, "Reconnect halted"},
    };
    Gen3CartScreenResult r;
    for(size_t k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
        gen3_cart_screen_result(
            want[k].job, Gen3CartStepIdentify, Gen3CartCancelled, false, true, &r);
        ASSERT_STR_EQ(r.header, want[k].header);
        ASSERT_STR_EQ(r.text, "Stopped.");
        ASSERT_TRUE(strstr(r.header, "failed") == NULL);
    }
}

TEST(test_result_written) {
    Gen3CartScreenResult r;
    gen3_cart_screen_result(Gen3CartJobWrite, Gen3CartStepPostBackup, Gen3CartOk, false, true, &r);
    ASSERT_STR_EQ(r.header, "Written");
    ASSERT_STR_EQ(r.text, "Written and verified.\nPower-cycle the GBA\nto play.");
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(
        Gen3CartJobWrite, Gen3CartStepPostBackup, Gen3CartWrittenNoBackup, false, true, &r);
    ASSERT_STR_EQ(r.header, "Written");
    ASSERT_STR_EQ(r.text, gen3_cart_result_text(Gen3CartWrittenNoBackup));
    gen3_cart_screen_result(
        Gen3CartJobWrite, Gen3CartStepPrepare, Gen3CartChanged, false, true, &r);
    ASSERT_STR_EQ(r.header, "Write failed");
    ASSERT_STR_EQ(r.text, "The cart changed\nsince the backup.\nBack up again.");
    /* A discard that found the cart changed dropped the edits all the same */
    gen3_cart_screen_result(Gen3CartJobDiscard, Gen3CartStepNone, Gen3CartOk, false, true, &r);
    ASSERT_STR_EQ(r.header, "Discarded");
    gen3_cart_screen_result(
        Gen3CartJobDiscard, Gen3CartStepNone, Gen3CartChanged, false, true, &r);
    ASSERT_STR_EQ(r.header, "Discarded");
    ASSERT_STR_EQ(r.text, gen3_cart_result_text(Gen3CartChanged));
    gen3_cart_screen_result(
        Gen3CartJobDiscard, Gen3CartStepNone, Gen3CartSdError, false, true, &r);
    ASSERT_STR_EQ(r.header, "Discard failed");
}

/* ---- The party and box screens ---- */

/* Why a Pokemon isn't opened or put, under "Can't edit" or "Can't put" */
TEST(test_mon_texts) {
    static const struct {
        Gen3CartMon m;
        const char* text;
    } want[] = {
        {Gen3CartMonMail, "Take the Mail off\nin the game first."},
        {Gen3CartMonItem, "It holds an item\nthe Flipper can't\nwrite back."},
        {Gen3CartMonBadData, "Unusual data:\nleft as is."},
        {Gen3CartMonEgg, "Eggs can't be\nedited."},
        {Gen3CartMonBadEgg, "Bad Egg: left as is."},
        {Gen3CartMonTaken, "That slot isn't\nfree."},
        {Gen3CartMonSdError, "SD card error."},
        {Gen3CartMonBadArg, "Invalid request."},
        {Gen3CartMonNoData, "Gen III data\nunreadable."},
    };
    for(size_t k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
        ASSERT_STR_EQ(gen3_cart_screen_mon_text(want[k].m), want[k].text);
        ASSERT_TRUE(fits(want[k].text, 3, GEN3_CART_SCREEN_LINE_LEN));
    }
    ASSERT_STR_EQ(gen3_cart_screen_mon_text(Gen3CartMonOk), "");
    ASSERT_STR_EQ(gen3_cart_screen_mon_text(Gen3CartMonEmpty), "");
    ASSERT_STR_EQ(gen3_cart_screen_mon_text((Gen3CartMon)(Gen3CartMonNoData + 1)), "");
    ASSERT_TRUE(fits(GEN3_CART_SCREEN_CANT_EDIT, 1, GEN3_CART_SCREEN_TITLE_LEN));
    ASSERT_TRUE(fits(GEN3_CART_SCREEN_CANT_PUT, 1, GEN3_CART_SCREEN_TITLE_LEN));
}

static Gen3MonSummary summary(Gen3MonKind kind, uint16_t national, uint8_t level) {
    Gen3MonSummary s;
    memset(&s, 0, sizeof(s));
    s.kind = kind;
    s.national = national;
    s.level = level;
    return s;
}

TEST(test_rows) {
    char row[GEN3_CART_SCREEN_ROW_SIZE];
    Gen3MonSummary s = summary(Gen3MonOk, 252, 5);
    gen3_cart_screen_row(0, &s, "TREECKO", false, row);
    ASSERT_STR_EQ(row, "01 TREECKO Lv5");
    gen3_cart_screen_row(29, &s, "TREECKO", true, row);
    ASSERT_STR_EQ(row, "30 TREECKO Lv5*");
    s = summary(Gen3MonOk, 6, 100);
    gen3_cart_screen_row(9, &s, "CHARMANDER", true, row);
    ASSERT_STR_EQ(row, "10 CHARMANDER Lv100*");
    s = summary(Gen3MonEmpty, 0, 0);
    gen3_cart_screen_row(3, &s, "", false, row);
    ASSERT_STR_EQ(row, "04 -- empty --");
    s = summary(Gen3MonEgg, 252, 5);
    gen3_cart_screen_row(4, &s, "TREECKO", false, row);
    ASSERT_STR_EQ(row, "05 EGG");
    s = summary(Gen3MonBad, 0, 0);
    gen3_cart_screen_row(5, &s, "", false, row);
    ASSERT_STR_EQ(row, "06 BAD EGG");
    /* a slot the working copy couldn't give */
    gen3_cart_screen_row(6, NULL, "", false, row);
    ASSERT_STR_EQ(row, "07 ?");
}

/* Put <SPECIES> Lv<n> / in <box>\nslot <k>?, the level in the text when the header would be over
 * 16 characters */
TEST(test_put_text) {
    char h[GEN3_CART_SCREEN_HEADER_SIZE];
    char t[GEN3_CART_SCREEN_PUT_SIZE];
    gen3_cart_screen_put("TREECKO", 10, "BOX 2", 0, h, t);
    ASSERT_STR_EQ(h, "Put TREECKO Lv10");
    ASSERT_STR_EQ(t, "in BOX 2\nslot 1?");
    gen3_cart_screen_put("CHARMANDER", 100, "BOX 14", 29, h, t);
    ASSERT_STR_EQ(h, "Put CHARMANDER");
    ASSERT_STR_EQ(t, "Lv100 in BOX 14\nslot 30?");
    gen3_cart_screen_put("MEW", 5, "Party", 3, h, t);
    ASSERT_STR_EQ(h, "Put MEW Lv5");
    ASSERT_STR_EQ(t, "in Party\nslot 4?");
    /* every length fits */
    static const char* const names[] = {"A", "ABCDEFG", "ABCDEFGH", "ABCDEFGHI", "ABCDEFGHIJ"};
    static const char* const boxes[] = {"", "B", "ABCDEFGH"};
    static const uint8_t levels[] = {1, 9, 10, 99, 100};
    for(size_t a = 0; a < 5; a++) {
        for(size_t b = 0; b < 3; b++) {
            for(size_t c = 0; c < 5; c++) {
                for(uint8_t slot = 0; slot < 30; slot += 9) {
                    gen3_cart_screen_put(names[a], levels[c], boxes[b], slot, h, t);
                    ASSERT_TRUE(fits(h, 1, GEN3_CART_SCREEN_TITLE_LEN));
                    ASSERT_TRUE(fits(t, 2, GEN3_CART_SCREEN_LINE_LEN));
                    ASSERT_TRUE(strstr(h, names[a]) != NULL);
                }
            }
        }
    }
}

TEST(test_ask_texts) {
    char t[GEN3_CART_SCREEN_ASK_SIZE];
    gen3_cart_screen_write_ask(1, t);
    ASSERT_STR_EQ(t, "1 change. The save\nis backed up. Keep\nthe GBA on.");
    gen3_cart_screen_write_ask(3, t);
    ASSERT_STR_EQ(t, "3 changes. The save\nis backed up. Keep\nthe GBA on.");
    gen3_cart_screen_write_ask(GEN3_CART_MON_SLOTS, t);
    ASSERT_TRUE(fits(t, 4, GEN3_CART_SCREEN_LINE_LEN));
    gen3_cart_screen_discard_ask(1, t);
    ASSERT_STR_EQ(t, "1 change.");
    gen3_cart_screen_discard_ask(GEN3_CART_MON_SLOTS, t);
    ASSERT_STR_EQ(t, "426 changes.");
    ASSERT_TRUE(fits(t, 4, GEN3_CART_SCREEN_LINE_LEN));
    ASSERT_TRUE(fits("Write changes?", 1, GEN3_CART_SCREEN_TITLE_LEN));
    ASSERT_TRUE(fits("Discard changes?", 1, GEN3_CART_SCREEN_TITLE_LEN));
}

/* A .pk3's check, as Load failed shows it */
TEST(test_check_texts) {
    for(int c = Gen3CheckOk; c <= Gen3CheckNoData; c++) {
        const char* t = gen3_cart_screen_check_text((Gen3Check)c);
        ASSERT_TRUE(t[0] != '\0');
        ASSERT_TRUE(fits(t, 3, GEN3_CART_SCREEN_LINE_LEN));
    }
    ASSERT_STR_EQ(gen3_cart_screen_check_text(Gen3CheckNoData), "Gen III data\nunreadable");
    ASSERT_STR_EQ(
        gen3_cart_screen_check_text(Gen3CheckBadSize), gen3_check_text(Gen3CheckBadSize));
}

/* What a loaded .pk3 had fixed, broken into lines that fit */
TEST(test_wrap) {
    char t[40];
    snprintf(t, sizeof(t), "%s", "\nFixed: mail moves level ability");
    gen3_cart_screen_wrap(t);
    ASSERT_STR_EQ(t, "Fixed: mail moves\nlevel ability");
    snprintf(t, sizeof(t), "%s", "\nFixed: level");
    gen3_cart_screen_wrap(t);
    ASSERT_STR_EQ(t, "Fixed: level");
    snprintf(t, sizeof(t), "%s", "");
    gen3_cart_screen_wrap(t);
    ASSERT_STR_EQ(t, "");
    snprintf(t, sizeof(t), "%s", "aaaaaaaaaa bbbbbbbbbb cccc dd");
    gen3_cart_screen_wrap(t);
    ASSERT_STR_EQ(t, "aaaaaaaaaa bbbbbbbbbb\ncccc dd");
    ASSERT_TRUE(fits(t, 2, GEN3_CART_SCREEN_LINE_LEN));
}

/* Cart info */

static Gen3CartId cid;
static Gen3SaveSlots slots;
static Gen3SaveTrainer trainer;
static Gen3CartScreenInfo in;
static char header[GEN3_CART_SCREEN_HEADER_SIZE];
static char text[GEN3_CART_SCREEN_INFO_SIZE];

/* A readable, writable Ruby with an Ok save in slot B and a current backup */
static void ruby(void) {
    memset(&cid, 0, sizeof(cid));
    memcpy(cid.code, "AXVE", 5);
    memcpy(cid.title, "POKEMON RUBY", 13);
    cid.version = 1;
    cid.game = Gen3LinkGameRS;
    cid.met_game = 2;
    cid.flash_id = 0x09C2u;
    cid.can_read_save = true;
    cid.can_browse = true;
    cid.can_write = true;
    cid.why = Gen3CartRefuseNone;
    memset(&slots, 0, sizeof(slots));
    slots.slot[0] = Gen3SlotOk;
    slots.slot[1] = Gen3SlotOk;
    slots.counter[0] = 40;
    slots.counter[1] = 41;
    slots.result = Gen3SaveOk;
    slots.save_counter = 41;
    slots.active = 1;
    slots.strict_ok = true;
    memset(&trainer, 0, sizeof(trainer));
    memcpy(trainer.name, "BRENDAN", 8);
    trainer.tid = 42;
    in.id = &cid;
    in.backup_current = true;
    in.backup_name = "backups/AXVE_00042_20261027-093015.sav";
    in.slots = &slots;
    in.trainer = &trainer;
    in.writable = true;
    in.why = Gen3CartRefuseNone;
}

static void info(void) {
    memset(header, 0xA5, sizeof(header));
    memset(text, 0xA5, sizeof(text));
    gen3_cart_screen_info(&in, header, text);
}

static bool info_line(unsigned k, const char* want) {
    char got[64];
    line_of(text, k, got);
    if(strcmp(got, want) == 0) return true;
    printf("  line %u: \"%s\" != \"%s\"\n", k, got, want);
    return false;
}

static bool info_fits(void) {
    return fits(header, 1, GEN3_CART_SCREEN_TITLE_LEN) && line_count(text) == 5 &&
           fits(text, 5, GEN3_CART_SCREEN_LINE_LEN);
}

TEST(test_info_ruby) {
    ruby();
    info();
    ASSERT_STR_EQ(header, "AXVE v1");
    ASSERT_TRUE(info_line(0, "Flash 09C2 Macronix"));
    ASSERT_TRUE(info_line(1, "Save: B #41"));
    ASSERT_TRUE(info_line(2, "BRENDAN 00042"));
    ASSERT_TRUE(info_line(3, "Writing on"));
    ASSERT_TRUE(info_line(4, "Backup 1027-093015"));
    ASSERT_TRUE(info_fits());
}

TEST(test_info_saves) {
    ruby();
    cid.flash_id = 0x1362u;
    slots.active = 0;
    slots.save_counter = 4294967295u;
    trainer.tid = 65535;
    info();
    ASSERT_TRUE(info_line(0, "Flash 1362 Sanyo"));
    ASSERT_TRUE(info_line(1, "Save: A #4294967295"));
    ASSERT_TRUE(info_line(2, "BRENDAN 65535"));
    ASSERT_TRUE(info_fits());

    /* One slot damaged: the game loads the other */
    slots.result = Gen3SaveError;
    slots.active = 1;
    slots.save_counter = 7;
    info();
    ASSERT_TRUE(info_line(1, "Save: B #7"));

    ruby();
    slots.result = Gen3SaveEmpty;
    in.trainer = NULL;
    in.writable = false;
    in.why = Gen3CartRefuseSave;
    info();
    ASSERT_TRUE(info_line(1, "Save: empty"));
    ASSERT_TRUE(info_line(2, "No trainer"));
    ASSERT_TRUE(info_line(3, "Save not recognised"));
    ASSERT_TRUE(info_fits());

    slots.result = Gen3SaveCorrupt;
    info();
    ASSERT_TRUE(info_line(1, "Save: corrupt"));
    ASSERT_TRUE(info_fits());
}

TEST(test_info_no_backup) {
    /* Japanese: backed up, but its layout isn't known */
    ruby();
    memcpy(cid.code, "AXVJ", 5);
    cid.can_browse = false;
    cid.can_write = false;
    cid.why = Gen3CartRefuseJapanese;
    in.slots = NULL;
    in.trainer = NULL;
    in.writable = false;
    in.why = Gen3CartRefuseJapanese;
    info();
    ASSERT_STR_EQ(header, "AXVJ v1");
    ASSERT_TRUE(info_line(1, "Save: layout unknown"));
    ASSERT_TRUE(info_line(3, "Japanese: backup only"));
    ASSERT_TRUE(info_line(4, "Backup 1027-093015"));
    ASSERT_TRUE(info_fits());

    /* The backup went (a cart that changed): no save to show */
    ruby();
    in.backup_current = false;
    in.backup_name = "";
    in.slots = NULL;
    in.trainer = NULL;
    in.writable = false;
    in.why = Gen3CartRefuseSave;
    info();
    ASSERT_TRUE(info_line(1, "Save: no backup"));
    ASSERT_TRUE(info_line(4, "No backup"));
    ASSERT_TRUE(info_fits());

    /* A name without a stamp in it */
    ruby();
    in.backup_name = "backups/x.sav";
    info();
    ASSERT_TRUE(info_line(4, "Backup made"));
    ASSERT_TRUE(info_fits());
}

TEST(test_info_unreadable) {
    /* A save the helper can't read still reaches the hub: why, and no save */
    static const Gen3CartRefuse whys[] = {
        Gen3CartRefuseHeader, Gen3CartRefuseNotPokemon, Gen3CartRefuseSram, Gen3CartRefuseChip};
    for(size_t k = 0; k < sizeof(whys) / sizeof(whys[0]); k++) {
        ruby();
        memcpy(cid.code, "BPEE", 5);
        cid.version = 255;
        cid.flash_id = 0xD4BFu;
        cid.can_read_save = false;
        cid.can_browse = false;
        cid.can_write = false;
        cid.why = whys[k];
        in.backup_current = false;
        in.backup_name = "";
        in.slots = NULL;
        in.trainer = NULL;
        in.writable = false;
        in.why = whys[k];
        info();
        ASSERT_STR_EQ(header, "BPEE v255");
        ASSERT_TRUE(info_line(0, "Flash D4BF ?"));
        ASSERT_TRUE(info_line(1, "Save: not read"));
        ASSERT_TRUE(info_line(2, "No trainer"));
        ASSERT_TRUE(info_line(3, gen3_cart_refuse_text(whys[k])));
        ASSERT_TRUE(info_line(4, "No backup"));
        ASSERT_TRUE(info_fits());
    }
}

TEST(test_leave_text) {
    char t[GEN3_CART_SCREEN_LEAVE_SIZE];
    gen3_cart_screen_leave(1, t);
    ASSERT_STR_EQ(t, "1 change not\nwritten.");
    gen3_cart_screen_leave(2, t);
    ASSERT_STR_EQ(t, "2 changes not\nwritten.");
    gen3_cart_screen_leave(GEN3_CART_MON_SLOTS, t);
    ASSERT_STR_EQ(t, "426 changes not\nwritten.");
    ASSERT_TRUE(fits(t, 2, GEN3_CART_SCREEN_LINE_LEN));
    gen3_cart_screen_leave(65535, t);
    ASSERT_TRUE(fits(t, 2, GEN3_CART_SCREEN_LINE_LEN));
}

int main(void) {
    RUN(test_back);
    RUN(test_progress_connect_steps);
    RUN(test_progress_cable);
    RUN(test_power_hint);
    RUN(test_progress_ignores_link_up);
    RUN(test_progress_write);
    RUN(test_progress_starting_stopping);
    RUN(test_progress_texts_fit);
    RUN(test_hub);
    RUN(test_result_retry);
    RUN(test_result_after_commit);
    RUN(test_result_dropped);
    RUN(test_result_texts_fit);
    RUN(test_result_retry_unlinked);
    RUN(test_result_stopped);
    RUN(test_result_written);
    RUN(test_mon_texts);
    RUN(test_rows);
    RUN(test_put_text);
    RUN(test_ask_texts);
    RUN(test_check_texts);
    RUN(test_wrap);
    RUN(test_info_ruby);
    RUN(test_info_saves);
    RUN(test_info_no_backup);
    RUN(test_info_unreadable);
    RUN(test_leave_text);
    TEST_EXIT();
}
