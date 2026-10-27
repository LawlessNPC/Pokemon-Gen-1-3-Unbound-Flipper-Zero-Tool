#include "test.h"
#include "fake_bios.h"
#include <stdbool.h>
#include <src/gen3/gen3_mb.h>
#include <src/gen3/mlink_types.h>

/* The BIOS slave (fake_bios, child 1) and the sender (gen3_mb) on one simulated stream-mode
 * link: each driver frame is fake_bios_frame then gen3_mb_frame, timed by mlink_frame_us. */

#define BENCH_LOG_MAX 100000u
#define BENCH_SEED 0x2468ACE1u

typedef struct {
    MlinkTx tx; /* what the sender asked for */
    uint32_t us; /* that frame's time */
    uint16_t reply; /* the first word's reply */
    bool replied; /* the first word came back unmarked */
    Gen3MbPhase phase; /* the sender's phase after the frame: the one that made the next tx */
} BenchFrame;

static BenchFrame bench_log[BENCH_LOG_MAX];
static uint32_t bench_n;
static MlinkTx bench_pending;
static bool bench_started;
static Gen3Mb mb;
static FakeBios bios;
static uint8_t image[GEN3_MB_MAX_LEN + 0x10];

/* A valid image: the logo's first word, the rest of 0x04..0x9F zero, 0x96, version 1 and the
 * complement; payload bytes from an LCG. */
static void make_image(uint8_t* buf, uint32_t len) {
    memset(buf, 0, GEN3_MB_HEADER_LEN);
    buf[4] = 0x24;
    buf[5] = 0xFF;
    buf[6] = 0xAE;
    buf[7] = 0x51;
    buf[0xB2] = 0x96;
    buf[0xBC] = 1;
    uint8_t chk = 0;
    for(uint32_t i = 0xA0; i <= 0xBC; i++) chk -= buf[i];
    buf[0xBD] = (uint8_t)(chk - 0x19);
    uint32_t x = len;
    for(uint32_t i = GEN3_MB_HEADER_LEN; i < len; i++) {
        x = x * 1103515245u + 12345u;
        buf[i] = (uint8_t)(x >> 16);
    }
}

static void bench_init(uint32_t len, uint8_t cc, uint8_t rr) {
    make_image(image, len);
    gen3_mb_init(&mb, image, len, BENCH_SEED);
    fake_bios_init(&bios, cc, rr);
    bench_n = 0;
    bench_started = false;
}

static Gen3MbStatus status(void) {
    Gen3MbStatus st;
    gen3_mb_status(&mb, &st);
    return st;
}

/* Runs at most max_frames driver frames, recording each; stops at Done or Failed. A later call
 * continues where the last one stopped. */
static void run(Gen3Mb* m, FakeBios* b, uint32_t max_frames) {
    if(!bench_started) {
        gen3_mb_frame(m, NULL, &bench_pending, 0);
        bench_started = true;
    }
    for(uint32_t i = 0; i < max_frames; i++) {
        Gen3MbStatus st;
        gen3_mb_status(m, &st);
        if(st.phase == Gen3MbPhaseDone || st.phase == Gen3MbPhaseFailed) return;
        MlinkRx rx;
        const MlinkTx sent = bench_pending;
        fake_bios_frame(b, &sent, &rx);
        const uint32_t us = mlink_frame_us(MlinkModeStream, sent.count);
        gen3_mb_frame(m, &rx, &bench_pending, us);
        gen3_mb_status(m, &st);
        if(bench_n < BENCH_LOG_MAX) {
            bench_log[bench_n++] = (BenchFrame){
                .tx = sent,
                .us = us,
                .reply = rx.word[0],
                .replied = rx.count && !((rx.timeout_mask | rx.framing_mask) & 1u),
                .phase = st.phase};
        }
    }
}

/* Runs until the sender is in phase; false if it ends (Done, Failed) or max_frames pass first. */
static bool run_until(Gen3MbPhase phase, uint32_t max_frames) {
    for(uint32_t i = 0; i < max_frames && status().phase != phase; i++) {
        if(status().phase == Gen3MbPhaseDone || status().phase == Gen3MbPhaseFailed) break;
        run(&mb, &bios, 1);
    }
    return status().phase == phase;
}

/* The frame in flight never reaches the fake (lost on the wire); its first word replies reply. */
static void inject(uint16_t reply) {
    MlinkRx rx;
    memset(&rx, 0, sizeof(rx));
    rx.count = bench_pending.count;
    rx.word[0] = reply;
    gen3_mb_frame(&mb, &rx, &bench_pending, mlink_frame_us(MlinkModeStream, rx.count));
}

static bool is_frame(const BenchFrame* f, uint16_t word) {
    return f->tx.count == 1 && f->tx.word[0] == word;
}

/* After Done or Failed the sender only sends idle ticks. */
static bool idles_forever(void) {
    for(int i = 0; i < 50; i++) {
        MlinkRx rx;
        memset(&rx, 0, sizeof(rx));
        rx.count = bench_pending.count;
        gen3_mb_frame(&mb, &rx, &bench_pending, MLINK_TICK_US);
        if(bench_pending.count != 0) return false;
    }
    return true;
}

static bool image_received(uint32_t len) {
    uint32_t got = 0;
    const uint8_t* img = fake_bios_image(&bios, &got);
    return got == len && memcmp(img, image, len) == 0;
}

TEST(test_mlink_frame_us) {
    ASSERT_EQ(mlink_frame_us(MlinkModeStream, 0), 1073);
    ASSERT_EQ(mlink_frame_us(MlinkModeStream, 1), 1073);
    ASSERT_EQ(mlink_frame_us(MlinkModeStream, 9), 9657);
    ASSERT_EQ(mlink_frame_us(MlinkModeStream, 255), 273615);
    ASSERT_EQ(mlink_frame_us(MlinkModeFrame, 1), 16740);
    ASSERT_EQ(mlink_frame_us(MlinkModeFrame, 9), 16740);
}

TEST(test_mb_crc_step) {
    ASSERT_EQ(gen3_mb_crc_step(0xFFF8, 0), 0x0749);
    ASSERT_EQ(gen3_mb_crc_step(0xFFF8, 0xFFFFFFFFu), 0x8723);
}

/* multiboot-and-save.md §2.3's test vector. */
static const uint16_t doc_first[8] =
    {0x3525, 0x4645, 0x0D28, 0xFD10, 0xCF51, 0x4E63, 0xC51C, 0x2615};
static const uint16_t doc_last[4] = {0xDAF9, 0x5C4D, 0x4DC4, 0xA30A};

static void doc_vector_halfwords(uint16_t out[128], uint16_t* crc) {
    uint8_t payload[0x100];
    for(int i = 0; i < 0x100; i++) payload[i] = (uint8_t)i;
    Gen3MbCrypt k;
    gen3_mb_crypt_init(&k, GEN3_MB_PALETTE, 0x5A);
    for(uint32_t i = 0; i < 64; i++) {
        const uint8_t* p = payload + 4 * i;
        const uint32_t w = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
                           (uint32_t)p[3] << 24;
        const uint32_t e = gen3_mb_crypt_word(&k, 0xC0 + 4 * i, w);
        out[2 * i] = (uint16_t)e;
        out[2 * i + 1] = (uint16_t)(e >> 16);
    }
    *crc = gen3_mb_crypt_final(&k, gen3_mb_hh(0x5A), 0xC3);
}

TEST(test_mb_doc_vector) {
    ASSERT_EQ(gen3_mb_hh(0x5A), 0x69);
    ASSERT_EQ((0x1C0 - 0x190) / 4, 0x000C);
    Gen3MbCrypt k;
    gen3_mb_crypt_init(&k, 0x93, 0x5A);
    ASSERT_EQ(k.seed, 0xFFFF5A93u);
    ASSERT_EQ(k.crc, 0xFFF8);
    uint16_t hw[128];
    uint16_t crc;
    doc_vector_halfwords(hw, &crc);
    for(int i = 0; i < 8; i++) ASSERT_EQ(hw[i], doc_first[i]);
    for(int i = 0; i < 4; i++) ASSERT_EQ(hw[124 + i], doc_last[i]);
    ASSERT_EQ(crc, 0x2A28);
}

TEST(test_mb_image_check) {
    make_image(image, 0x1C0);
    ASSERT_EQ(gen3_mb_image_check(image, 0x1C0, 1), Gen3MbImageOk);
    make_image(image, 0x2010);
    ASSERT_EQ(gen3_mb_image_check(image, 0x2010, 1), Gen3MbImageOk);
    make_image(image, 0x3000);
    ASSERT_EQ(gen3_mb_image_check(image, 0x3000, 1), Gen3MbImageOk);
    make_image(image, 0x3010);
    ASSERT_EQ(gen3_mb_image_check(image, 0x1B0, 1), Gen3MbImageBadLen);
    ASSERT_EQ(gen3_mb_image_check(image, 0x1C8, 1), Gen3MbImageBadLen);
    ASSERT_EQ(gen3_mb_image_check(image, 0x3010, 1), Gen3MbImageBadLen);
    make_image(image, 0x1C0);
    image[0xB2] = 0;
    ASSERT_EQ(gen3_mb_image_check(image, 0x1C0, 1), Gen3MbImageBadHeader);
    make_image(image, 0x1C0);
    image[0xBD]++;
    ASSERT_EQ(gen3_mb_image_check(image, 0x1C0, 1), Gen3MbImageBadHeader);
    make_image(image, 0x1C0);
    image[4] ^= 1;
    ASSERT_EQ(gen3_mb_image_check(image, 0x1C0, 1), Gen3MbImageBadHeader);
    make_image(image, 0x1C0);
    ASSERT_EQ(gen3_mb_image_check(image, 0x1C0, 2), Gen3MbImageBadVersion);
}

TEST(test_mb_boot) {
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 20000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.last_fail, Gen3MbFailNone);
    ASSERT_EQ(st.total, 0x1000 - GEN3_MB_HEADER_LEN);
    ASSERT_EQ(st.sent, st.total);
    ASSERT_EQ(fake_bios_phase(&bios), FakeBiosBooted);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
    ASSERT_TRUE(image_received(0x1000));
    for(uint32_t i = 0; i < bench_n; i++) ASSERT_TRUE(bench_log[i].tx.count <= 9);
    ASSERT_TRUE(idles_forever());
}

TEST(test_mb_waits_for_gba) {
    bench_init(0x1000, 0x5A, 0xC3);
    run(&mb, &bios, 4000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDetect);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_TRUE(st.waiting_us >= 3000000u);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.waiting_us, 0);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
}

TEST(test_mb_length_delay) {
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 20000);
    ASSERT_EQ(status().phase, Gen3MbPhaseDone);
    const uint16_t handshake = 0x6400 | gen3_mb_hh(0x5A);
    const uint16_t length = (0x1000 - 0x190) / 4;
    uint32_t i = 0;
    while(i < bench_n && !(bench_log[i].tx.count == 1 && bench_log[i].tx.word[0] == handshake))
        i++;
    ASSERT_TRUE(i < bench_n);
    uint32_t idle_us = 0;
    for(i++; i < bench_n && bench_log[i].tx.count == 0; i++) idle_us += bench_log[i].us;
    ASSERT_TRUE(i < bench_n);
    ASSERT_EQ(bench_log[i].tx.count, 1);
    ASSERT_EQ(bench_log[i].tx.word[0], length);
    ASSERT_TRUE(idle_us >= 100000u);
}

/* Detect and Palette poll back to back, one word per tick, as afska's LinkCableMultiboot does;
 * Final's 0x0065 retries come at most once per GBA frame, with idle ticks between, as afska's
 * do (once per VBlank). */
TEST(test_mb_poll_spacing) {
    ASSERT_EQ(GEN3_MB_POLL_US, MLINK_FRAME_US);
    bench_init(0x1000, 0x5A, 0xC3);
    run(&mb, &bios, 300);
    fake_bios_set_busy(&bios, 50000, 3500000);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    ASSERT_EQ(status().phase, Gen3MbPhaseDone);
    uint32_t detect = 0;
    uint32_t palette = 0;
    uint32_t final = 0;
    uint64_t now = 0;
    uint64_t last_final = 0;
    for(uint32_t i = 0; i < bench_n; i++) {
        const Gen3MbPhase from = i ? bench_log[i - 1].phase : Gen3MbPhaseDetect;
        const BenchFrame* f = &bench_log[i];
        if(from == Gen3MbPhaseDetect) {
            ASSERT_TRUE(is_frame(f, 0x6200));
            detect++;
        } else if(from == Gen3MbPhasePalette) {
            ASSERT_TRUE(is_frame(f, 0x6300 | GEN3_MB_PALETTE));
            palette++;
        } else if(from == Gen3MbPhaseFinal && f->tx.count) {
            ASSERT_TRUE(is_frame(f, 0x0065));
            if(final) ASSERT_TRUE(now - last_final >= GEN3_MB_POLL_US);
            last_final = now;
            final++;
        }
        now += f->us;
    }
    /* 300 frames off, then 2 timeouts, 0x0000 and 0x7202; 50 ms busy; 3.5 s busy */
    ASSERT_TRUE(detect >= 300 + 4);
    ASSERT_TRUE(palette >= 40);
    ASSERT_TRUE(final >= 200);
}

/* A slow slave: at least 40 busy 0x7202 replies to the palette word, and 200 busy 0x0074 replies
 * to 0x0065. */
TEST(test_mb_busy_slave) {
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_set_busy(&bios, 50000, 3500000);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.last_fail, Gen3MbFailNone);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
    ASSERT_TRUE(image_received(0x1000));
    uint32_t busy_palette = 0;
    uint32_t busy_final = 0;
    for(uint32_t i = 0; i < bench_n; i++) {
        const BenchFrame* f = &bench_log[i];
        if(is_frame(f, 0x6300 | GEN3_MB_PALETTE) && f->replied && f->reply == 0x7202) {
            busy_palette++;
        }
        if(is_frame(f, 0x0065) && f->replied && f->reply == 0x0074) busy_final++;
    }
    ASSERT_TRUE(busy_palette >= 40);
    ASSERT_TRUE(busy_final >= 200);
}

/* The palette budget, one 0x63pp per tick: the last poll it allows is the one whose reply comes
 * at or past GEN3_MB_PALETTE_US. A slave whose 0x73cc comes on that poll is taken; one busy for
 * one poll longer times the wait out. That wait saw only valid 0x7202 replies, so it is not
 * counted: the sender keeps trying, and last_fail says why. */
TEST(test_mb_palette_timeout) {
    const uint32_t polls = (GEN3_MB_PALETTE_US + MLINK_TICK_US - 1u) / MLINK_TICK_US;
    ASSERT_TRUE(GEN3_MB_PALETTE_US >= 3500000u && GEN3_MB_PALETTE_US <= 4500000u);
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_set_busy(&bios, (polls - 1u) * MLINK_TICK_US, 0);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.last_fail, Gen3MbFailNone);

    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_set_busy(&bios, polls * MLINK_TICK_US, 0);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 3u * (polls + 400u));
    st = status();
    ASSERT_TRUE(st.phase != Gen3MbPhaseDone && st.phase != Gen3MbPhaseFailed);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.last_fail, Gen3MbFailPalette);
    ASSERT_EQ(fake_bios_boots(&bios), 0);
    uint32_t timeouts = 0;
    for(uint32_t i = 0; i < bench_n; i++) {
        const BenchFrame* f = &bench_log[i];
        if(is_frame(f, 0x6300 | GEN3_MB_PALETTE)) ASSERT_EQ(f->reply, 0x7202);
        if(i && bench_log[i - 1].phase == Gen3MbPhasePalette && f->phase == Gen3MbPhaseWait) {
            timeouts++;
        }
    }
    ASSERT_TRUE(timeouts >= 3);
}

/* A palette wait that saw anything but valid 0x7202 replies counts: here the GBA resets in the
 * middle of it (two timeouts, then 0x0000). */
TEST(test_mb_palette_reset_counts) {
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_set_busy(&bios, 1000000, 0);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(run_until(Gen3MbPhasePalette, 1000));
    run(&mb, &bios, 10);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(st.last_fail, Gen3MbFailPalette);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
}

TEST(test_mb_final_timeout) {
    bench_init(0x1C0, 0x5A, 0xC3);
    fake_bios_set_busy(&bios, 0, 6000000);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseFailed);
    ASSERT_EQ(st.restarts, 4);
    ASSERT_EQ(st.last_fail, Gen3MbFailFinal);
    ASSERT_EQ(fake_bios_boots(&bios), 0);
}

TEST(test_mb_header_glitch) {
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_corrupt_reply(&bios, 50);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(st.last_fail, Gen3MbFailHeader);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
    ASSERT_TRUE(image_received(0x1000));
    /* The restart wait (multiboot-and-save.md §2.4): 70 ms plus up to 170 ms of idle ticks, at
     * least GBATEK's 1/16 s, then detection polls again. */
    uint32_t i = 0;
    while(i < bench_n && bench_log[i].phase != Gen3MbPhaseWait) i++;
    ASSERT_TRUE(i < bench_n);
    uint32_t idle_us = 0;
    for(i++; i < bench_n && bench_log[i].tx.count == 0; i++) idle_us += bench_log[i].us;
    ASSERT_TRUE(i < bench_n);
    ASSERT_EQ(bench_log[i].tx.word[0], 0x6200);
    ASSERT_TRUE(idle_us >= 70000u);
    ASSERT_TRUE(idle_us < 70000u + 170000u + MLINK_TICK_US);
}

TEST(test_mb_data_glitch) {
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_corrupt_reply(&bios, 1000);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(st.last_fail, Gen3MbFailData);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
    ASSERT_TRUE(image_received(0x1000));
}

TEST(test_mb_bad_crc_gives_up) {
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_force_bad_crc(&bios, true);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseFailed);
    ASSERT_EQ(st.restarts, 4);
    ASSERT_EQ(st.last_fail, Gen3MbFailCrc);
    ASSERT_EQ(fake_bios_boots(&bios), 0);
    ASSERT_EQ(fake_bios_phase(&bios), FakeBiosWaiting);
    ASSERT_TRUE(idles_forever());
}

/* A word the driver marks (timeout or framing) is ignored while polling, and fails any other
 * phase. */
TEST(test_mb_masked_word) {
    make_image(image, 0x1C0);
    gen3_mb_init(&mb, image, 0x1C0, BENCH_SEED);
    MlinkTx tx;
    gen3_mb_frame(&mb, NULL, &tx, 0);
    ASSERT_EQ(tx.count, 1);
    ASSERT_EQ(tx.word[0], 0x6200);
    MlinkRx rx;
    memset(&rx, 0, sizeof(rx));
    rx.count = 1;
    rx.word[0] = 0x7202;
    rx.framing_mask = 1;
    gen3_mb_frame(&mb, &rx, &tx, MLINK_TICK_US);
    ASSERT_EQ(status().phase, Gen3MbPhaseDetect);
    ASSERT_EQ(tx.count, 1);
    ASSERT_EQ(tx.word[0], 0x6200);
    memset(&rx, 0, sizeof(rx));
    rx.count = 1;
    rx.word[0] = 0x7202;
    gen3_mb_frame(&mb, &rx, &tx, MLINK_TICK_US);
    ASSERT_EQ(status().phase, Gen3MbPhaseHello);
    ASSERT_EQ(tx.count, 1);
    ASSERT_EQ(tx.word[0], 0x6102);
    rx.word[0] = 0x7202;
    rx.timeout_mask = 1;
    gen3_mb_frame(&mb, &rx, &tx, MLINK_TICK_US);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseWait);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(st.last_fail, Gen3MbFailHello);
    ASSERT_EQ(tx.count, 0);
}

/* A length gen3_mb_image_check refuses starts Failed: idle ticks only, and the image is never
 * read. */
TEST(test_mb_bad_len) {
    static const uint32_t lens[] = {0, 0x100, 0x1B0, 0x1C8, 0x3010};
    for(size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        gen3_mb_init(&mb, NULL, lens[i], BENCH_SEED);
        Gen3MbStatus st = status();
        ASSERT_EQ(st.phase, Gen3MbPhaseFailed);
        ASSERT_EQ(st.restarts, 0);
        ASSERT_EQ(st.last_fail, Gen3MbFailNone);
        MlinkTx tx;
        gen3_mb_frame(&mb, NULL, &tx, 0);
        ASSERT_EQ(tx.count, 0);
        MlinkRx rx;
        memset(&rx, 0, sizeof(rx));
        gen3_mb_frame(&mb, &rx, &tx, MLINK_TICK_US);
        ASSERT_EQ(tx.count, 0);
        ASSERT_EQ(status().phase, Gen3MbPhaseFailed);
    }
}

/* A valid reply that isn't 0x73xx to 0x64hh is a counted Handshake failure. The fake never saw
 * the word, so the restart finds it in its palette step: it refuses that and resyncs. */
TEST(test_mb_wrong_handshake_reply) {
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(run_until(Gen3MbPhaseHandshake, 1000));
    ASSERT_TRUE(bench_pending.count == 1 && bench_pending.word[0] == (0x6400 | gen3_mb_hh(0x5A)));
    inject(0x7202);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseWait);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(st.last_fail, Gen3MbFailHandshake);
    run(&mb, &bios, 100000);
    st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
}

/* The same for the length word: a valid reply that isn't 0x73rr is a counted Length failure. The
 * fake never saw the word and still waits for one, so it is power-cycled, as the user would. */
TEST(test_mb_wrong_length_reply) {
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(run_until(Gen3MbPhaseLength, 1000));
    ASSERT_TRUE(bench_pending.count == 1 && bench_pending.word[0] == (0x1000 - 0x190) / 4);
    inject(0x7202);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseWait);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(st.last_fail, Gen3MbFailLength);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
}

/* Detection: 16 replies that are neither 0x0000, 0xFFFF nor 0x72x2 start a restart wait, not
 * counted: 70 ms plus up to 170 ms of idle ticks (multiboot-and-save.md §2.4). waiting_us counts
 * the waits too. */
TEST(test_mb_junk_wait) {
    static const uint16_t replies[3] = {0x0000, 0xFFFF, 0x1234};
    make_image(image, 0x1C0);
    gen3_mb_init(&mb, image, 0x1C0, BENCH_SEED);
    MlinkTx tx;
    MlinkRx rx;
    gen3_mb_frame(&mb, NULL, &tx, 0);
    uint32_t total_us = 0;
    uint32_t min_us = UINT32_MAX;
    uint32_t max_us = 0;
    for(int wait = 0; wait < 200; wait++) {
        uint32_t polls = 0;
        while(status().phase == Gen3MbPhaseDetect) {
            memset(&rx, 0, sizeof(rx));
            rx.count = tx.count;
            if(tx.count) rx.word[0] = replies[polls++ % 3];
            const uint32_t us = mlink_frame_us(MlinkModeStream, rx.count);
            total_us += us;
            gen3_mb_frame(&mb, &rx, &tx, us);
        }
        ASSERT_EQ(status().phase, Gen3MbPhaseWait);
        ASSERT_EQ(polls, 3 * GEN3_MB_JUNK_LIMIT);
        ASSERT_EQ(GEN3_MB_JUNK_LIMIT, 16);
        uint32_t idle_us = 0;
        while(tx.count == 0) {
            memset(&rx, 0, sizeof(rx));
            idle_us += MLINK_TICK_US;
            total_us += MLINK_TICK_US;
            gen3_mb_frame(&mb, &rx, &tx, MLINK_TICK_US);
        }
        ASSERT_EQ(status().phase, Gen3MbPhaseDetect);
        ASSERT_EQ(tx.word[0], 0x6200);
        if(idle_us < min_us) min_us = idle_us;
        if(idle_us > max_us) max_us = idle_us;
    }
    Gen3MbStatus st = status();
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.last_fail, Gen3MbFailNone);
    ASSERT_EQ(st.waiting_us, total_us);
    ASSERT_TRUE(min_us >= 70000u && min_us < 80000u);
    ASSERT_TRUE(max_us > 230000u && max_us < 240000u + MLINK_TICK_US);
}

/* The fake on its own, driven word by word with multiboot-and-save.md §2.2's replies and the
 * payload of the multiboot-and-save.md §2.3 test vector. */
static uint16_t xfer(FakeBios* b, uint16_t word, bool* timed_out) {
    MlinkTx tx = {.count = 1, .word = {word}};
    MlinkRx rx;
    fake_bios_frame(b, &tx, &rx);
    if(timed_out) *timed_out = (rx.timeout_mask & 1u) != 0;
    return rx.word[0];
}

static void idle(FakeBios* b, uint32_t ticks) {
    MlinkTx tx = {.count = 0};
    MlinkRx rx;
    for(uint32_t i = 0; i < ticks; i++) fake_bios_frame(b, &tx, &rx);
}

/* Power-on through the 96 header halfwords; false on the first unexpected reply. */
static bool fake_header(FakeBios* b, const uint8_t* hdr) {
    bool to;
    xfer(b, 0x6200, &to);
    if(!to) return false;
    xfer(b, 0x6200, &to);
    if(!to) return false;
    if(xfer(b, 0x6200, &to) != 0x0000 || to) return false;
    if(xfer(b, 0x6200, NULL) != 0x7202) return false;
    if(xfer(b, 0x6102, NULL) != 0x7202) return false;
    for(uint32_t k = 0; k < 0x60; k++) {
        const uint16_t w = hdr[2 * k] | (uint16_t)(hdr[2 * k + 1] << 8);
        if(xfer(b, w, NULL) != (((0x60 - k) << 8) | 0x02)) return false;
    }
    return fake_bios_error(b) == NULL;
}

/* Power-on through the 0x64hh transfer; false on the first unexpected reply. */
static bool fake_handshake(FakeBios* b, const uint8_t* hdr, uint8_t hh) {
    if(!fake_header(b, hdr)) return false;
    if(xfer(b, 0x6200, NULL) != 0x0002) return false;
    if(xfer(b, 0x6202, NULL) != 0x7202) return false;
    if(xfer(b, 0x6393, NULL) != 0x735A) return false;
    if(xfer(b, 0x6400 | hh, NULL) != 0x735A) return false;
    return fake_bios_error(b) == NULL;
}

/* Power-on through the last payload halfword of the multiboot-and-save.md §2.3 test vector,
 * which it writes into image; false on the first unexpected reply. */
static bool fake_doc_vector_payload(FakeBios* b) {
    make_image(image, 0x1C0);
    for(int i = 0; i < 0x100; i++) image[0xC0 + i] = (uint8_t)i;
    uint16_t hw[128];
    uint16_t crc;
    doc_vector_halfwords(hw, &crc);
    if(!fake_handshake(b, image, 0x69)) return false;
    idle(b, 94);
    if(xfer(b, 0x000C, NULL) != 0x73C3) return false;
    for(uint32_t k = 0; k < 128; k++) {
        if(xfer(b, hw[k], NULL) != 0xC0 + 2 * k) return false;
    }
    return fake_bios_error(b) == NULL;
}

TEST(test_fake_bios_doc_vector) {
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(fake_doc_vector_payload(&bios));
    ASSERT_TRUE(xfer(&bios, 0x0065, NULL) != 0x0075);
    ASSERT_EQ(xfer(&bios, 0x0065, NULL), 0x0075);
    ASSERT_EQ(xfer(&bios, 0x0066, NULL), 0x0075);
    ASSERT_EQ(xfer(&bios, 0x2A28, NULL), 0x2A28);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
    ASSERT_EQ(fake_bios_phase(&bios), FakeBiosBooted);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
    ASSERT_TRUE(image_received(0x1C0));
}

/* The fake refuses what multiboot-and-save.md §2.2 forbids, and goes back to detection. */
TEST(test_fake_bios_checks) {
    make_image(image, 0x1C0);
    /* the length word inside the 1/16 s wait */
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(fake_handshake(&bios, image, 0x69));
    idle(&bios, 50);
    ASSERT_EQ(xfer(&bios, 0x000C, NULL), 0x73C3);
    ASSERT_TRUE(fake_bios_error(&bios) != NULL);
    ASSERT_EQ(xfer(&bios, 0x6200, NULL), 0x0000);
    /* a wrong handshake byte */
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(!fake_handshake(&bios, image, 0x6A));
    ASSERT_TRUE(fake_bios_error(&bios) != NULL);
    /* a bad header: no 0x96 */
    image[0xB2] = 0;
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(!fake_handshake(&bios, image, 0x69));
    ASSERT_TRUE(fake_bios_error(&bios) != NULL);
    make_image(image, 0x1C0);
    /* 0x6102 before the slave answered 0x7202 */
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    xfer(&bios, 0x6200, NULL);
    xfer(&bios, 0x6200, NULL);
    ASSERT_EQ(xfer(&bios, 0x6102, NULL), 0x0000);
    ASSERT_TRUE(fake_bios_error(&bios) != NULL);
    /* an undocumented palette byte */
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(fake_header(&bios, image));
    ASSERT_EQ(xfer(&bios, 0x6200, NULL), 0x0002);
    ASSERT_EQ(xfer(&bios, 0x6202, NULL), 0x7202);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
    xfer(&bios, 0x6380, NULL);
    ASSERT_TRUE(fake_bios_error(&bios) != NULL);
    /* after the header, 0x6202 where 0x6200 belongs */
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(fake_header(&bios, image));
    ASSERT_EQ(xfer(&bios, 0x6202, NULL), 0x0002);
    ASSERT_TRUE(fake_bios_error(&bios) != NULL);
    ASSERT_EQ(xfer(&bios, 0x6200, NULL), 0x0000);
    /* then 0x6200 where 0x6202 belongs */
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(fake_header(&bios, image));
    ASSERT_EQ(xfer(&bios, 0x6200, NULL), 0x0002);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
    ASSERT_EQ(xfer(&bios, 0x6200, NULL), 0x7202);
    ASSERT_TRUE(fake_bios_error(&bios) != NULL);
    ASSERT_EQ(xfer(&bios, 0x6200, NULL), 0x0000);
    /* 0x0066 without a 0x0075 reply: right after the first 0x0065 */
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(fake_doc_vector_payload(&bios));
    ASSERT_TRUE(xfer(&bios, 0x0065, NULL) != 0x0075);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
    ASSERT_EQ(xfer(&bios, 0x0066, NULL), 0x0075);
    ASSERT_TRUE(fake_bios_error(&bios) != NULL);
    ASSERT_EQ(xfer(&bios, 0x6200, NULL), 0x0000);
    ASSERT_EQ(fake_bios_boots(&bios), 0);
}

int main(void) {
    RUN(test_mlink_frame_us);
    RUN(test_mb_crc_step);
    RUN(test_mb_doc_vector);
    RUN(test_mb_image_check);
    RUN(test_mb_boot);
    RUN(test_mb_waits_for_gba);
    RUN(test_mb_length_delay);
    RUN(test_mb_poll_spacing);
    RUN(test_mb_busy_slave);
    RUN(test_mb_palette_timeout);
    RUN(test_mb_palette_reset_counts);
    RUN(test_mb_final_timeout);
    RUN(test_mb_header_glitch);
    RUN(test_mb_data_glitch);
    RUN(test_mb_bad_crc_gives_up);
    RUN(test_mb_masked_word);
    RUN(test_mb_bad_len);
    RUN(test_mb_wrong_handshake_reply);
    RUN(test_mb_wrong_length_reply);
    RUN(test_mb_junk_wait);
    RUN(test_fake_bios_doc_vector);
    RUN(test_fake_bios_checks);
    TEST_EXIT();
}
