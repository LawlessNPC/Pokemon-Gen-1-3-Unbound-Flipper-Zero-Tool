#include "test.h"
#include "bulk_file.h"
#include "gen3_golden.h"
#include <stdlib.h>
#include <src/gen3/gen3_club.h>
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_link_player.h>
#include <src/gen3/gen3_pk3.h>
#include <src/gen3/gen3_rules.h>
#include <src/gen3/gen3_trade_block.h>
#include <src/gen3/gen3_trade_sync.h>

#define G(f) gen3_pkm_get(&ed.pkm, Gen3Field##f)

/* The GBA player: RUBY, TID 12345, SID 54321 (OT ID 0xD4313039), female */
static const uint8_t RUBY_NAME[GEN3_OT_NAME_LEN] = {0xCC, 0xCF, 0xBC, 0xD3, 0xFF, 0xFF, 0xFF};
#define RUBY_OT_ID 0xD4313039u

#define ENIGMA_FIXTURE "../docs/gen3/fixtures/enigma-berry.pk3"

static void status_reset(Gen3ClubStatus* st, Gen3ClubPhase phase) {
    memset(st, 0, sizeof(*st));
    st->phase = phase;
}

/* A status whose trainer (the GBA's LinkPlayer from its 0x1133 linkup) is RUBY */
static void status_ruby(Gen3ClubStatus* st) {
    status_reset(st, Gen3ClubPhaseRoom);
    st->trainer_valid = true;
    st->trainer.version = GEN3_VERSION_RUBY;
    st->trainer.trainer_id = RUBY_OT_ID;
    memset(st->trainer.name, 0xFF, sizeof(st->trainer.name));
    memcpy(st->trainer.name, RUBY_NAME, GEN3_OT_NAME_LEN);
    st->trainer.gender = 1;
    st->trainer.link_type = GEN3_LINKTYPE_TRADE_SETUP;
    st->trainer.language = GEN3_LANGUAGE_ENGLISH;
}

static unsigned poll(
    Gen3TradeSync* sync,
    Gen3Editor* ed,
    const Gen3ClubStatus* st,
    const uint8_t received[GEN3_PARTY_SIZE],
    uint32_t count) {
    return gen3_trade_sync_poll(sync, ed, st, received, count, false);
}

/* The offer is the editor's Pokemon, game form */
static bool offer_is_editor(const Gen3TradeSync* sync, const Gen3Editor* ed) {
    uint8_t want[GEN3_PARTY_SIZE];
    gen3_pkm_encrypt(&ed->pkm, want);
    return sync->offer_valid && memcmp(sync->offer, want, GEN3_PARTY_SIZE) == 0;
}

/* GOLDEN with no move, game form: loading it needs the default moveset from the bulk file */
static void moveless_golden_game(uint8_t out[GEN3_PARTY_SIZE]) {
    Gen3Pkm p;
    golden_build(&p);
    for(int s = 0; s < 4; s++) {
        gen3_pkm_set(&p, Gen3FieldMove1 + s, 0);
        gen3_pkm_set(&p, Gen3FieldPp1 + s, 0);
    }
    gen3_pkm_fix_checksum(&p);
    gen3_pkm_encrypt(&p, out);
}

TEST(test_trainer_from_link_player) {
    Gen3LinkPlayer lp;
    memset(&lp, 0, sizeof(lp));
    for(int i = 0; i < 11; i++) lp.name[i] = (uint8_t)(0xA0 + i);
    lp.trainer_id = 0xD4313039;
    lp.gender = 5;
    Gen3Trainer t;
    memset(&t, 0x5A, sizeof(t));
    gen3_trainer_from_link_player(&lp, &t);
    ASSERT_MEM_EQ(t.name, lp.name, GEN3_OT_NAME_LEN);
    ASSERT_EQ(t.tid, 12345);
    ASSERT_EQ(t.sid, 54321);
    ASSERT_EQ(t.gender, 1);

    /* Bytes 7..10 of the LinkPlayer name aren't part of the OT name */
    for(int i = 7; i < 11; i++) lp.name[i] = 0x00;
    lp.gender = 0;
    Gen3Trainer u;
    gen3_trainer_from_link_player(&lp, &u);
    ASSERT_MEM_EQ(u.name, t.name, GEN3_OT_NAME_LEN);
    ASSERT_EQ(u.tid, 12345);
    ASSERT_EQ(u.sid, 54321);
    ASSERT_EQ(u.gender, 0);
}

TEST(test_sync_init) {
    Gen3TradeSync sync;
    memset(&sync, 0xA5, sizeof(sync));
    gen3_trade_sync_init(&sync);
    ASSERT_EQ(sync.trades_seen, 0);
    ASSERT_TRUE(!sync.trainer_known);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNone);
    ASSERT_TRUE(!sync.taken);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupOk);
    ASSERT_TRUE(!sync.gone_valid);
}

TEST(test_sync_first_offer) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_reset(&st, Gen3ClubPhaseWaiting);

    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNone);
    ASSERT_EQ(sync.trades_seen, 0);
    ASSERT_TRUE(ed.pristine); /* the sync itself changes nothing */
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);

    /* An edit is offered on the next poll */
    gen3_edit_set_level(&ed, 6);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    /* Not taken (the session refused it): offered again */
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);
    /* An edit undone before the next poll: the offer taken still holds */
    gen3_edit_set_level(&ed, 7);
    gen3_edit_set_level(&ed, 6);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);
}

TEST(test_sync_trainer) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_ruby(&st);

    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncTrainer | Gen3SyncOffer);
    ASSERT_MEM_EQ(ed.pkm.raw + 0x14, RUBY_NAME, GEN3_OT_NAME_LEN);
    ASSERT_EQ(G(OtId), RUBY_OT_ID);
    ASSERT_EQ(G(OtGender), 1);
    ASSERT_TRUE(ed.pristine);
    ASSERT_TRUE(gen3_edit_ot_is_trainer(&ed));
    ASSERT_TRUE(sync.trainer_known);
    ASSERT_MEM_EQ(sync.trainer.name, RUBY_NAME, GEN3_OT_NAME_LEN);
    ASSERT_EQ(sync.trainer.tid, 12345);
    ASSERT_EQ(sync.trainer.sid, 54321);
    ASSERT_EQ(sync.trainer.gender, 1);
    ASSERT_TRUE(offer_is_editor(&sync, &ed)); /* with the player's OT */
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);

    /* Another player: the still-default Pokemon becomes theirs */
    st.trainer.trainer_id = 0x00020001;
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncTrainer | Gen3SyncOffer);
    ASSERT_EQ(G(OtId), 0x00020001);
    ASSERT_TRUE(ed.pristine);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    gen3_trade_sync_offer_taken(&sync);

    /* No trainer in the status: the editor keeps the last one */
    st.trainer_valid = false;
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);
    ASSERT_EQ(G(OtId), 0x00020001);

    /* An edited Pokemon keeps its OT: the player is handed over, the offer stays */
    gen3_edit_set_level(&ed, 10);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);
    status_ruby(&st);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncTrainer);
    ASSERT_EQ(G(OtId), 0x00020001);
    ASSERT_EQ(gen3_edit_trainer(&ed)->tid, 12345);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);
}

TEST(test_sync_pickup) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_reset(&st, Gen3ClubPhaseSave);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);

    golden_game(rx);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickedUp | Gen3SyncOffer);
    ASSERT_MEM_EQ(ed.pkm.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
    ASSERT_EQ(sync.trades_seen, 1);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupOk);
    ASSERT_TRUE(!sync.gone_valid);
    ASSERT_TRUE(sync.offer_valid);
    ASSERT_MEM_EQ(sync.offer, rx, GEN3_PARTY_SIZE);
    gen3_trade_sync_offer_taken(&sync);
    /* Taken once: the same count is no new trade */
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), 0);
    ASSERT_MEM_EQ(ed.pkm.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);

    /* The next trade */
    Gen3Pkm p;
    golden_build(&p);
    gen3_pkm_set(&p, Gen3FieldExp, gen3_exp_for_level(gen3_species(252)->growth, 10));
    gen3_recalc_party(&p);
    gen3_pkm_fix_checksum(&p);
    gen3_pkm_encrypt(&p, rx);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 2), Gen3SyncPickedUp | Gen3SyncOffer);
    ASSERT_EQ(sync.trades_seen, 2);
    ASSERT_EQ(G(Level), 10);
    ASSERT_MEM_EQ(sync.offer, rx, GEN3_PARTY_SIZE);
}

/* T7 carry-over: Ruby's MonGainEVs raises EVs without recalculating the party stats, and the
 * pickup's gen3_normalise recalculates them. The normalised Pokemon is published in the same poll
 * as the pickup (the first tick after trades_done advances), so its game form differs from the
 * received bytes the club offers after the trade. */
TEST(test_sync_pickup_stats_lag_evs) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_reset(&st, Gen3ClubPhaseSave);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);

    Gen3Pkm p;
    golden_build(&p);
    gen3_pkm_set(&p, Gen3FieldEvHp, 252);
    gen3_pkm_set(&p, Gen3FieldEvAtk, 252);
    gen3_pkm_fix_checksum(&p);
    uint16_t calc[Gen3StatCount];
    gen3_calc_stats(&p, calc);
    ASSERT_TRUE(calc[Gen3StatHp] != gen3_pkm_get(&p, Gen3FieldMaxHp)); /* the stats lag */
    ASSERT_TRUE(calc[Gen3StatAtk] != gen3_pkm_get(&p, Gen3FieldAtk));
    gen3_pkm_encrypt(&p, rx);

    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickedUp | Gen3SyncOffer);
    ASSERT_EQ(G(EvHp), 252);
    ASSERT_EQ(G(EvAtk), 252);
    ASSERT_EQ(G(MaxHp), calc[Gen3StatHp]);
    ASSERT_EQ(G(Atk), calc[Gen3StatAtk]);
    ASSERT_EQ(G(Hp), 20); /* the current HP is kept */
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_TRUE(memcmp(sync.offer, rx, GEN3_PARTY_SIZE) != 0);
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), 0);
}

/* D6: a received Pokemon replaces an edit the session never took */
TEST(test_sync_pickup_before_offer) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_reset(&st, Gen3ClubPhaseSave);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);

    gen3_edit_set_level(&ed, 30);
    golden_game(rx);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickedUp | Gen3SyncOffer);
    ASSERT_MEM_EQ(ed.pkm.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
    ASSERT_TRUE(sync.offer_valid);
    ASSERT_MEM_EQ(sync.offer, rx, GEN3_PARTY_SIZE);
}

/* O2: the received bytes don't decrypt. The traded-away Pokemon is never offered again, its level
 * edited or not, until the editor holds another one (another PID or OT ID). */
TEST(test_sync_bad_received) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_reset(&st, Gen3ClubPhaseSave);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);
    Gen3Pkm before = ed.pkm;

    golden_game(rx);
    rx[0x30] ^= 1;
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickupBad | Gen3SyncOffer);
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    ASSERT_EQ(sync.trades_seen, 1);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupBadData);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNone); /* the Pokemon itself is fine */
    gen3_trade_sync_offer_taken(&sync);

    /* Not retried */
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), 0);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupBadData);

    /* The same Pokemon edited: still not offered */
    gen3_edit_set_level(&ed, 30);
    ASSERT_EQ(G(Pid), gen3_pkm_get(&before, Gen3FieldPid));
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), 0);
    ASSERT_TRUE(!sync.offer_valid);

    /* Another Pokemon (a new PID): offered */
    ASSERT_TRUE(gen3_edit_set_species(&ed, 4));
    ASSERT_TRUE(G(Pid) != gen3_pkm_get(&before, Gen3FieldPid));
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_EQ(sync.pickup_fail, Gen3PickupOk);
    ASSERT_TRUE(!sync.gone_valid);

    /* A load refused by gen3_edit_load (an egg) is BadData too; another OT ID frees the editor */
    gen3_trade_sync_offer_taken(&sync);
    before = ed.pkm;
    Gen3Pkm egg;
    golden_build(&egg);
    gen3_pkm_set(&egg, Gen3FieldIsEgg, 1);
    gen3_pkm_fix_checksum(&egg);
    gen3_pkm_encrypt(&egg, rx);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 2), Gen3SyncPickupBad | Gen3SyncOffer);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupBadData);
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_EQ(gen3_edit_set_ot_id(&ed, 1, 2), Gen3OtIdOk);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 2), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_EQ(sync.pickup_fail, Gen3PickupOk);
}

/* O2: the bulk file is unreadable at the pickup: retried on every poll, with the offer unpublished
 * meanwhile, until it works */
TEST(test_sync_pickup_retry) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_reset(&st, Gen3ClubPhaseSave);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);
    Gen3Pkm before = ed.pkm;

    moveless_golden_game(rx);
    bulk_file_failing = true;
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickupBad | Gen3SyncOffer);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupNoData);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_EQ(sync.trades_seen, 1);
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    gen3_trade_sync_offer_taken(&sync);

    /* Still unreadable: tried again, nothing else changes */
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickupBad);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupNoData);
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);

    ASSERT_TRUE(bulk_file_open());
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickedUp | Gen3SyncOffer);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupOk);
    ASSERT_TRUE(!sync.gone_valid);
    ASSERT_EQ(gen3_edit_national(&ed), 252);
    ASSERT_EQ(G(Move1), 1); /* Treecko's level-5 moves: POUND, LEER */
    ASSERT_EQ(G(Move2), 43);
    ASSERT_EQ(G(Move3), 0);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), 0);
}

/* D6: while editing, nothing moves either way; afterwards all of it, in order */
TEST(test_sync_holds_while_editing) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_reset(&st, Gen3ClubPhaseWaiting);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);
    uint8_t taken[GEN3_PARTY_SIZE];
    memcpy(taken, sync.offer, sizeof(taken));

    status_ruby(&st);
    golden_game(rx);
    gen3_edit_set_level(&ed, 30);
    Gen3Pkm edited = ed.pkm;
    ASSERT_EQ(gen3_trade_sync_poll(&sync, &ed, &st, rx, 1, true), 0);
    ASSERT_MEM_EQ(ed.pkm.raw, edited.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(!ed.has_trainer);
    ASSERT_EQ(sync.trades_seen, 0);
    ASSERT_TRUE(!sync.trainer_known);
    ASSERT_MEM_EQ(sync.offer, taken, GEN3_PARTY_SIZE);
    ASSERT_MEM_EQ(sync.taken_offer, taken, GEN3_PARTY_SIZE);

    ASSERT_EQ(
        gen3_trade_sync_poll(&sync, &ed, &st, rx, 1, false),
        Gen3SyncPickedUp | Gen3SyncTrainer | Gen3SyncOffer);
    ASSERT_MEM_EQ(
        ed.pkm.raw, GOLDEN_PK3, GEN3_PARTY_SIZE); /* loaded: the player only remembered */
    ASSERT_TRUE(ed.has_trainer);
    ASSERT_EQ(sync.trades_seen, 1);
    ASSERT_TRUE(sync.trainer_known);
    ASSERT_MEM_EQ(sync.offer, rx, GEN3_PARTY_SIZE);
    gen3_trade_sync_offer_taken(&sync);

    /* A NoData retry waits for the editing to end too */
    moveless_golden_game(rx);
    bulk_file_failing = true;
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 2), Gen3SyncPickupBad | Gen3SyncOffer);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupNoData);
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_TRUE(bulk_file_open());
    Gen3Pkm held = ed.pkm;
    ASSERT_EQ(gen3_trade_sync_poll(&sync, &ed, &st, rx, 2, true), 0);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupNoData);
    ASSERT_MEM_EQ(ed.pkm.raw, held.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_EQ(
        gen3_trade_sync_poll(&sync, &ed, &st, rx, 2, false), Gen3SyncPickedUp | Gen3SyncOffer);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupOk);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
}

/* G6, D8, F7/R7: a Pokemon the trade can't take is never published */
TEST(test_sync_bad_offer_unpublished) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_reset(&st, Gen3ClubPhaseRoom);

    Gen3Pkm p;
    golden_build(&p);
    gen3_pkm_set(&p, Gen3FieldHeldItem, GEN3_ITEM_ENIGMA_BERRY);
    gen3_pkm_fix_checksum(&p);
    ASSERT_TRUE(gen3_edit_load(&ed, &p));
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseItem);
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);

    gen3_edit_set_item(&ed, 0);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNone);
    gen3_trade_sync_offer_taken(&sync);

    /* Defences: the editor can't hold Mail, so poke it */
    gen3_pkm_set(&ed.pkm, Gen3FieldHeldItem, GEN3_ITEM_MAIL_FIRST);
    gen3_pkm_fix_checksum(&ed.pkm);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseMail);
    gen3_pkm_set(&ed.pkm, Gen3FieldHeldItem, 0);
    gen3_pkm_set(&ed.pkm, Gen3FieldMail, 0);
    gen3_pkm_fix_checksum(&ed.pkm);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseMail);
    gen3_pkm_set(&ed.pkm, Gen3FieldMail, GEN3_MAIL_NONE);
    gen3_pkm_set(&ed.pkm, Gen3FieldHeldItem, GEN3_RUBY_ITEMS_COUNT);
    gen3_pkm_fix_checksum(&ed.pkm);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseItem);
    ASSERT_TRUE(!sync.offer_valid);
}

/* R7: the ENIGMA BERRY holder for the device check (checklist H.3) */
TEST(test_enigma_fixture) {
    Gen3Pkm p;
    golden_build(&p);
    gen3_pkm_set(&p, Gen3FieldHeldItem, GEN3_ITEM_ENIGMA_BERRY);
    uint8_t want[GEN3_BOX_SIZE];
    gen3_pk3_export(&p, want);

    if(getenv("GEN3_WRITE_FIXTURES")) {
        FILE* w = fopen(ENIGMA_FIXTURE, "wb");
        ASSERT_TRUE(w != NULL);
        size_t n = fwrite(want, 1, sizeof(want), w);
        ASSERT_EQ(fclose(w), 0);
        ASSERT_EQ(n, sizeof(want));
    }

    uint8_t got[GEN3_BOX_SIZE + 1];
    FILE* f = fopen(ENIGMA_FIXTURE, "rb");
    ASSERT_TRUE(f != NULL);
    size_t n = fread(got, 1, sizeof(got), f);
    fclose(f);
    ASSERT_EQ(n, GEN3_BOX_SIZE);
    ASSERT_MEM_EQ(got, want, GEN3_BOX_SIZE);

    Gen3Pkm q;
    ASSERT_EQ(gen3_pk3_import(got, GEN3_BOX_SIZE, &q, NULL), Gen3CheckOk);
    ASSERT_EQ(gen3_pkm_get(&q, Gen3FieldHeldItem), GEN3_ITEM_ENIGMA_BERRY);
    ASSERT_EQ(gen3_trade_offer_check(&q), Gen3RefuseItem);
}

/* The guide as three strings; prints what it got on a mismatch */
static bool guide_is(
    const Gen3ClubStatus* st,
    bool started,
    bool cable_ok,
    Gen3Refuse offer_refuse,
    Gen3PickupFail pickup_fail,
    const char* l1,
    const char* l2,
    const char* l3) {
    char out[GEN3_GUIDE_LINES][GEN3_GUIDE_LINE_LEN];
    memset(out, 'X', sizeof(out));
    gen3_trade_guide(st, started, cable_ok, offer_refuse, pickup_fail, out);
    for(int i = 0; i < GEN3_GUIDE_LINES; i++) {
        if(memchr(out[i], '\0', GEN3_GUIDE_LINE_LEN) == NULL) {
            printf("  guide line %d has no NUL\n", i + 1);
            return false;
        }
    }
    if(strcmp(out[0], l1) != 0 || strcmp(out[1], l2) != 0 || strcmp(out[2], l3) != 0) {
        printf("  guide: \"%s\" / \"%s\" / \"%s\"\n", out[0], out[1], out[2]);
        printf("  want:  \"%s\" / \"%s\" / \"%s\"\n", l1, l2, l3);
        return false;
    }
    return true;
}

#define GUIDE(st, r, pf, l1, l2, l3) ASSERT_TRUE(guide_is((st), true, true, (r), (pf), l1, l2, l3))

/* The reason line of each refusal */
static const char* const REASON[] = {
    [Gen3RefuseEmpty] = "that slot is empty.",
    [Gen3RefuseBadEgg] = "it's a Bad Egg.",
    [Gen3RefuseEgg] = "it's an Egg.",
    [Gen3RefuseMail] = "it holds Mail.",
    [Gen3RefuseItem] = "it holds a bad item.",
    [Gen3RefuseBadData] = "its data is invalid.",
};

TEST(test_guide_texts) {
    Gen3ClubStatus st;
    const Gen3Refuse no = Gen3RefuseNone;
    const Gen3PickupFail ok = Gen3PickupOk;

    /* Cable and start, ahead of everything */
    status_reset(&st, Gen3ClubPhaseRoom);
    st.save_window = true;
    ASSERT_TRUE(
        guide_is(&st, true, false, no, ok, "Plug in the cable", "(or swap its ends).", ""));
    ASSERT_TRUE(guide_is(&st, false, true, no, ok, "Starting...", "", ""));

    /* The save window (D10) */
    status_reset(&st, Gen3ClubPhaseSave);
    st.save_window = true;
    st.trade_pending = true;
    st.leaving = true;
    GUIDE(&st, no, ok, "The GBA is saving.", "Don't unplug or turn", "off. Back is locked.");

    /* left (D9) says nothing about a trade still pending (T8 carry-over: left isn't "trade
     * resolved") */
    status_reset(&st, Gen3ClubPhaseWaiting);
    st.left = true;
    st.trade_pending = true;
    st.last_result = Gen3TradeResultCanceled;
    GUIDE(&st, Gen3RefuseItem, Gen3PickupBadData, "Link ended. You can", "unplug the GBA.", "");

    /* leaving: no "offer changed" hint in the menu (T8 carry-over) */
    status_reset(&st, Gen3ClubPhaseMenu);
    st.leaving = true;
    st.offer_stale = true;
    st.menu = Gen3ClubMenuLeaving;
    GUIDE(&st, no, ok, "Ending: the GBA walks", "out of the Trade", "Center by itself.");
    st.trade_pending = true;
    GUIDE(&st, no, ok, "Ending: the GBA walks", "out of the Trade", "Center by itself.");

    status_reset(&st, Gen3ClubPhaseMenu);
    st.trade_pending = true;
    st.menu = Gen3ClubMenuStarting;
    GUIDE(&st, Gen3RefuseItem, Gen3PickupNoData, "Trading...", "Watch the GBA.", "");
    status_reset(&st, Gen3ClubPhaseTrade);
    st.trade_pending = true;
    GUIDE(&st, no, ok, "Trading...", "Watch the GBA.", "");

    /* Errors */
    status_reset(&st, Gen3ClubPhaseError);
    st.last_error = Gen3ClubErrorOtherRoom;
    GUIDE(
        &st, Gen3RefuseItem, ok, "The GBA chose another", "room. Use the Trade", "Center (right).");
    st.last_error = Gen3ClubErrorBadPlayerBlock;
    GUIDE(&st, no, ok, "Link error. Restart", "the GBA, then go to", "the Cable Club.");
    st.last_error = Gen3ClubErrorUnexpected;
    GUIDE(
        &st,
        no,
        Gen3PickupBadData,
        "Link error. Restart",
        "the GBA, then go to",
        "the Cable Club.");

    /* O2: a failed pickup, above any offer refusal */
    status_reset(&st, Gen3ClubPhaseRoom);
    st.gba_seated = true;
    GUIDE(
        &st,
        Gen3RefuseItem,
        Gen3PickupNoData,
        "Can't take the traded",
        "Pokemon: data file",
        "unreadable. Retrying.");
    status_reset(&st, Gen3ClubPhaseWaiting);
    GUIDE(
        &st,
        no,
        Gen3PickupBadData,
        "Can't take the traded",
        "Pokemon: bad data.",
        "Load or pick another.");

    /* R5: no valid offer while the GBA sits: the Flipper joins once there is one */
    status_reset(&st, Gen3ClubPhaseRoom);
    st.gba_seated = true;
    GUIDE(
        &st,
        Gen3RefuseItem,
        ok,
        "Can't offer it:",
        REASON[Gen3RefuseItem],
        "Edit it; stay seated.");

    /* No valid offer, each reason */
    for(Gen3Refuse r = Gen3RefuseEmpty; r <= Gen3RefuseBadData; r++) {
        status_reset(&st, Gen3ClubPhaseWaiting);
        st.last_result = Gen3TradeResultRolledBack;
        GUIDE(&st, r, ok, "Can't offer it:", REASON[r], "Edit it first.");
        status_reset(&st, Gen3ClubPhaseMenu);
        st.offer_stale = true;
        GUIDE(&st, r, ok, "Can't offer it:", REASON[r], "Edit it first.");
    }

    /* Waiting: how the latest trade ended */
    status_reset(&st, Gen3ClubPhaseWaiting);
    st.last_result = Gen3TradeResultRolledBack;
    GUIDE(&st, no, ok, "Link lost while the", "GBA saved. It keeps", "its old save.");
    st.last_result = Gen3TradeResultAssumed;
    GUIDE(&st, no, ok, "Link lost at the end.", "Check the GBA party:", "it likely traded.");
    st.last_result = Gen3TradeResultCanceled;
    GUIDE(&st, no, ok, "Link lost. Nothing", "was traded.", "");
    st.last_result = Gen3TradeResultDone;
    GUIDE(&st, no, ok, "Go to the Cable Club:", "Pokemon Center 2F,", "right counter.");
    st.last_result = Gen3TradeResultNone;
    GUIDE(&st, no, ok, "Go to the Cable Club:", "Pokemon Center 2F,", "right counter.");
    /* T7 carry-over: after a Disconnected, the link state, not the re-sit hint */
    st.resit_needed = true;
    GUIDE(&st, no, ok, "Go to the Cable Club:", "Pokemon Center 2F,", "right counter.");

    status_reset(&st, Gen3ClubPhaseLinked);
    GUIDE(&st, no, ok, "Linking...", "", "");
    status_reset(&st, Gen3ClubPhasePlayers);
    GUIDE(&st, no, ok, "Linking...", "", "");
    status_reset(&st, Gen3ClubPhaseCards);
    st.resit_needed = true;
    GUIDE(&st, no, ok, "Linking...", "", "");

    /* Room */
    status_reset(&st, Gen3ClubPhaseRoom);
    st.resit_needed = true;
    GUIDE(&st, no, ok, "Sit at the table", "again (step off and", "back on if seated).");
    st.gba_seated = true; /* T7 carry-over: the hint only while the GBA isn't seated */
    GUIDE(&st, no, ok, "Opening the trade", "menu...", "");
    st.resit_needed = false;
    GUIDE(&st, no, ok, "Opening the trade", "menu...", "");
    status_reset(&st, Gen3ClubPhaseExchange);
    GUIDE(&st, no, ok, "Opening the trade", "menu...", "");
    status_reset(&st, Gen3ClubPhaseRoom);
    st.flipper_seated = true;
    GUIDE(&st, no, ok, "In the Trade Center:", "sit at the table on", "the GBA.");

    /* Menu */
    status_reset(&st, Gen3ClubPhaseMenu);
    st.offer_stale = true;
    st.refused = Gen3RefuseEgg;
    GUIDE(&st, no, ok, "Updating the trade", "menu: the GBA goes", "back to the room.");
    status_reset(&st, Gen3ClubPhaseMenu);
    st.menu = Gen3ClubMenuLeaving;
    GUIDE(&st, no, ok, "Updating the trade", "menu: the GBA goes", "back to the room.");
    for(Gen3Refuse r = Gen3RefuseEmpty; r <= Gen3RefuseBadData; r++) {
        status_reset(&st, Gen3ClubPhaseMenu);
        st.refused = r;
        st.trades_done = 1;
        GUIDE(&st, no, ok, "Can't take that one:", REASON[r], "Pick another one.");
    }
    status_reset(&st, Gen3ClubPhaseMenu);
    st.menu = Gen3ClubMenuOffered;
    st.trades_done = 2;
    GUIDE(&st, no, ok, "Confirm the trade", "on the GBA.", "");
    status_reset(&st, Gen3ClubPhaseMenu);
    st.trades_done = 1;
    GUIDE(&st, no, ok, "Edit it (OK), or pick", "the next trade on", "the GBA.");
    status_reset(&st, Gen3ClubPhaseMenu);
    GUIDE(&st, no, ok, "Pick a Pokemon to", "trade on the GBA.", "");
    /* An R8 DDEE leaves refused at None: no reason is claimed (T5 carry-over) */
    st.menu = Gen3ClubMenuIdle;
    st.refused = Gen3RefuseNone;
    GUIDE(&st, no, ok, "Pick a Pokemon to", "trade on the GBA.", "");

    /* Otherwise */
    status_reset(&st, Gen3ClubPhaseClosing);
    GUIDE(&st, no, ok, "Please wait...", "", "");
    status_reset(&st, Gen3ClubPhaseSave);
    st.last_result = Gen3TradeResultDone;
    st.trades_done = 1;
    GUIDE(&st, no, ok, "Please wait...", "", "");
    status_reset(&st, Gen3ClubPhaseTrade);
    GUIDE(&st, no, ok, "Please wait...", "", "");
}

/* Every line fits FontSecondary across 128 px (21 characters) for every combination */
TEST(test_guide_line_lengths) {
    Gen3ClubStatus st;
    char out[GEN3_GUIDE_LINES][GEN3_GUIDE_LINE_LEN];
    unsigned long calls = 0;
    for(int phase = Gen3ClubPhaseWaiting; phase <= Gen3ClubPhaseSave; phase++) {
        for(int err = Gen3ClubErrorNone; err <= Gen3ClubErrorUnexpected; err++) {
            for(unsigned flags = 0; flags < (1u << 7); flags++) {
                for(int refused = Gen3RefuseNone; refused <= Gen3RefuseBadData; refused++) {
                    for(int result = Gen3TradeResultNone; result <= Gen3TradeResultAssumed;
                        result++) {
                        status_reset(&st, (Gen3ClubPhase)phase);
                        st.last_error = (Gen3ClubError)err;
                        st.save_window = flags & 1;
                        st.left = flags & 2;
                        st.leaving = flags & 4;
                        st.trade_pending = flags & 8;
                        st.offer_stale = flags & 16;
                        st.resit_needed = flags & 32;
                        st.gba_seated = flags & 64;
                        st.refused = (Gen3Refuse)refused;
                        st.last_result = (Gen3TradeResult)result;
                        for(int offer = Gen3RefuseNone; offer <= Gen3RefuseBadData; offer++) {
                            for(int pf = Gen3PickupOk; pf <= Gen3PickupBadData; pf++) {
                                memset(out, 'X', sizeof(out));
                                gen3_trade_guide(
                                    &st, true, true, (Gen3Refuse)offer, (Gen3PickupFail)pf, out);
                                calls++;
                                for(int i = 0; i < GEN3_GUIDE_LINES; i++) {
                                    if(memchr(out[i], '\0', GEN3_GUIDE_LINE_LEN) == NULL ||
                                       strlen(out[i]) > GEN3_GUIDE_LINE_LEN - 1) {
                                        printf(
                                            "  line %d too long: phase %d error %d flags 0x%02X "
                                            "refused %d result %d offer %d pickup %d\n",
                                            i + 1,
                                            phase,
                                            err,
                                            flags,
                                            refused,
                                            result,
                                            offer,
                                            pf);
                                        ASSERT_TRUE(false);
                                    }
                                }
                                ASSERT_TRUE(out[0][0] != '\0'); /* line 1 always says something */
                            }
                        }
                    }
                }
            }
        }
    }
    ASSERT_EQ(GEN3_GUIDE_LINE_LEN, 22);
    ASSERT_EQ(calls, 11ul * 5 * 128 * 7 * 5 * 7 * 3);
}

/* Each test starts with the bulk file readable, whatever a test that failed with
 * bulk_file_failing set left behind */
#define RUN_BULK(name) \
    do { \
        if(!bulk_file_open()) return 1; \
        RUN(name); \
    } while(0)

int main(void) {
    RUN_BULK(test_trainer_from_link_player);
    RUN_BULK(test_sync_init);
    RUN_BULK(test_sync_first_offer);
    RUN_BULK(test_sync_trainer);
    RUN_BULK(test_sync_pickup);
    RUN_BULK(test_sync_pickup_stats_lag_evs);
    RUN_BULK(test_sync_pickup_before_offer);
    RUN_BULK(test_sync_bad_received);
    RUN_BULK(test_sync_pickup_retry);
    RUN_BULK(test_sync_holds_while_editing);
    RUN_BULK(test_sync_bad_offer_unpublished);
    RUN_BULK(test_enigma_fixture);
    RUN_BULK(test_guide_texts);
    RUN_BULK(test_guide_line_lengths);
    TEST_EXIT();
}
