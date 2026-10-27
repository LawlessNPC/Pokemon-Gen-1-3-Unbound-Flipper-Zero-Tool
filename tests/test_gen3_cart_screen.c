#include "test.h"
#include <stdbool.h>
#include <src/gen3/gen3_cart.h>
#include <src/gen3/gen3_cart_furi.h>
#include <src/gen3/gen3_cart_id.h>
#include <src/gen3/gen3_cart_screen.h>
#include <src/gen3/gen3_mb.h>
#include <src/gen3/gen3_save.h>

/* The Cartridge app's screen texts and choices: what the progress screen shows for each job and
 * step, the power-on hint, the hub's items, the results and their Retry, Cart info and the leave
 * warning; and that every text fits the screen. */

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

TEST(test_hub) {
    Gen3CartScreenHub h;
    memset(&st, 0, sizeof(st));

    st.link_up = true;
    gen3_cart_screen_hub(&st, true, true, &h);
    ASSERT_FALSE(h.reconnect);
    ASSERT_FALSE(h.backup);

    st.link_up = false;
    gen3_cart_screen_hub(&st, true, true, &h);
    ASSERT_TRUE(h.reconnect);
    ASSERT_FALSE(h.backup);

    st.link_up = true;
    gen3_cart_screen_hub(&st, false, true, &h);
    ASSERT_FALSE(h.reconnect);
    ASSERT_TRUE(h.backup);

    /* A cart whose save can't be read reaches the hub with no backup: nothing to back up */
    gen3_cart_screen_hub(&st, false, false, &h);
    ASSERT_FALSE(h.backup);

    /* A Reconnect that didn't run to its end leaves the cart unlinked, even when the helper
     * answered its last command (stopped after HELLO): offer it again */
    st.job = Gen3CartJobReconnect;
    st.result = Gen3CartCancelled;
    st.link_up = true;
    gen3_cart_screen_hub(&st, true, true, &h);
    ASSERT_TRUE(h.reconnect);
    st.result = Gen3CartBusyTimeout;
    gen3_cart_screen_hub(&st, true, true, &h);
    ASSERT_TRUE(h.reconnect);
    /* One that ran to its end is linked, whatever it found */
    st.result = Gen3CartChanged;
    gen3_cart_screen_hub(&st, false, true, &h);
    ASSERT_FALSE(h.reconnect);
    st.result = Gen3CartOk;
    gen3_cart_screen_hub(&st, true, true, &h);
    ASSERT_FALSE(h.reconnect);
    /* Another job stopped by Back leaves the link as it was */
    st.job = Gen3CartJobBackup;
    st.result = Gen3CartCancelled;
    gen3_cart_screen_hub(&st, false, true, &h);
    ASSERT_FALSE(h.reconnect);
    ASSERT_TRUE(h.backup);

    /* While a job runs (a Connect not yet taken, say), link_up isn't read */
    st.busy = true;
    st.job = Gen3CartJobConnect;
    st.link_up = false;
    gen3_cart_screen_hub(&st, false, true, &h);
    ASSERT_FALSE(h.reconnect);
    ASSERT_FALSE(h.backup);
}

TEST(test_result_retry) {
    Gen3CartScreenResult r;

    /* Connect: every failure goes back to the instructions */
    for(int res = Gen3CartLinkLost; res <= Gen3CartLinkBusy; res++) {
        gen3_cart_screen_result(
            Gen3CartJobConnect, Gen3CartStepMbWait, (Gen3CartResult)res, false, &r);
        ASSERT_EQ(r.retry, Gen3CartRetryConnect);
        ASSERT_STR_EQ(r.header, "Connect failed");
        ASSERT_STR_EQ(r.text, gen3_cart_result_text((Gen3CartResult)res));
    }
    gen3_cart_screen_result(Gen3CartJobConnect, Gen3CartStepAnalyze, Gen3CartOk, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);

    /* A lost link: Reconnect, and a Reconnect that fails sends the helper again */
    gen3_cart_screen_result(
        Gen3CartJobBackup, Gen3CartStepBackupRead, Gen3CartLinkLost, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryReconnect);
    ASSERT_STR_EQ(r.header, "Backup failed");
    gen3_cart_screen_result(Gen3CartJobWrite, Gen3CartStepErase, Gen3CartBusyTimeout, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryReconnect);
    ASSERT_STR_EQ(r.header, "Write failed");
    gen3_cart_screen_result(Gen3CartJobRom, Gen3CartStepRomRead, Gen3CartLinkLost, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryReconnect);
    gen3_cart_screen_result(
        Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartLinkLost, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryConnect);
    ASSERT_STR_EQ(r.header, "Reconnect failed");
    gen3_cart_screen_result(
        Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartBusyTimeout, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryConnect);
    gen3_cart_screen_result(
        Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartHelperError, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryConnect);

    /* A Reconnect that ran to its end and found another save, or was stopped: no Retry */
    gen3_cart_screen_result(
        Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartChanged, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(
        Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartCancelled, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartOk, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    ASSERT_STR_EQ(r.header, "Reconnected");
    ASSERT_STR_EQ(r.text, "Done.");

    /* Any other result: OK only */
    gen3_cart_screen_result(Gen3CartJobWrite, Gen3CartStepVerify, Gen3CartVerifyFailed, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(Gen3CartJobBackup, Gen3CartStepBackupRead, Gen3CartSdFull, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(
        Gen3CartJobBackup, Gen3CartStepBackupRead, Gen3CartCancelled, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    ASSERT_STR_EQ(r.text, "Stopped.");
    gen3_cart_screen_result(Gen3CartJobBackup, Gen3CartStepAnalyze, Gen3CartOk, false, &r);
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
            gen3_cart_screen_result(jobs[j], early[k], Gen3CartLinkLost, false, &r);
            ASSERT_STR_EQ(r.text, gen3_cart_result_text(Gen3CartLinkLost));
            gen3_cart_screen_result(jobs[j], early[k], Gen3CartBusyTimeout, false, &r);
            ASSERT_STR_EQ(r.text, gen3_cart_result_text(Gen3CartBusyTimeout));
        }
        /* After a commit it may load the new one: never claim the old one */
        for(size_t k = 0; k < 3; k++) {
            gen3_cart_screen_result(jobs[j], late[k], Gen3CartLinkLost, false, &r);
            ASSERT_STR_EQ(
                r.text, "Link lost. The cart\nmay load either save.\nRetry, then redo it.");
            ASSERT_EQ(r.retry, Gen3CartRetryReconnect);
            ASSERT_TRUE(strstr(r.text, "still") == NULL);
            gen3_cart_screen_result(jobs[j], late[k], Gen3CartBusyTimeout, false, &r);
            ASSERT_STR_EQ(
                r.text, "No answer. The cart\nmay load either save.\nRetry, then redo it.");
            ASSERT_EQ(r.retry, Gen3CartRetryReconnect);
            /* Other results keep their own text */
            gen3_cart_screen_result(jobs[j], late[k], Gen3CartVerifyFailed, false, &r);
            ASSERT_STR_EQ(r.text, gen3_cart_result_text(Gen3CartVerifyFailed));
        }
    }
    /* Only a write can have committed */
    gen3_cart_screen_result(Gen3CartJobBackup, Gen3CartStepVerify, Gen3CartLinkLost, false, &r);
    ASSERT_STR_EQ(r.text, gen3_cart_result_text(Gen3CartLinkLost));
}

TEST(test_result_dropped) {
    Gen3CartScreenResult r;
    gen3_cart_screen_result(Gen3CartJobConnect, Gen3CartStepAnalyze, Gen3CartOk, true, &r);
    ASSERT_STR_EQ(r.header, "Edits dropped");
    ASSERT_STR_EQ(r.text, "The save on the cart\nchanged.");
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    /* dropped is Connect's only */
    gen3_cart_screen_result(Gen3CartJobBackup, Gen3CartStepAnalyze, Gen3CartOk, true, &r);
    ASSERT_STR_EQ(r.header, "Backed up");
}

TEST(test_result_texts_fit) {
    for(size_t j = 0; j < sizeof(all_jobs) / sizeof(all_jobs[0]); j++) {
        for(int res = Gen3CartOk; res <= Gen3CartLinkBusy; res++) {
            for(int step = Gen3CartStepNone; step <= Gen3CartStepRomVerify; step++) {
                for(int dropped = 0; dropped < 2; dropped++) {
                    Gen3CartScreenResult r;
                    gen3_cart_screen_result(
                        all_jobs[j], (Gen3CartStep)step, (Gen3CartResult)res, dropped, &r);
                    ASSERT_TRUE(r.header && r.header[0] != '\0');
                    ASSERT_TRUE(fits(r.header, 1, GEN3_CART_SCREEN_TITLE_LEN));
                    ASSERT_TRUE(r.text && r.text[0] != '\0');
                    ASSERT_TRUE(fits(r.text, 3, GEN3_CART_SCREEN_LINE_LEN));
                }
            }
        }
    }
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
    RUN(test_info_ruby);
    RUN(test_info_saves);
    RUN(test_info_no_backup);
    RUN(test_info_unreadable);
    RUN(test_leave_text);
    TEST_EXIT();
}
