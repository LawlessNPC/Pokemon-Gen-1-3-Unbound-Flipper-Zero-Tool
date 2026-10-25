#include "test.h"
#include <src/gen3/gen3_text.h>
#include <src/gen3/gen3_link_player.h>

static const uint8_t magic[16] =
    {0x47, 0x61, 0x6D, 0x65, 0x46, 0x72, 0x65, 0x61, 0x6B, 0x20, 0x69, 0x6E, 0x63, 0x2E, 0x00, 0x00};

static void make_lp(Gen3LinkPlayer* lp) {
    memset(lp, 0, sizeof(*lp));
    lp->version = GEN3_VERSION_RUBY;
    lp->lp_field_2 = 0;
    lp->trainer_id = 0x12345678;
    gen3_text_encode("FLIPPER", lp->name, 11, 7);
    lp->gender = 1;
    lp->link_type = GEN3_LINKTYPE_TRADE_SETUP;
    lp->id = 0;
    lp->language = GEN3_LANGUAGE_ENGLISH;
}

TEST(test_lp_build_layout) {
    Gen3LinkPlayer lp;
    make_lp(&lp);
    uint8_t out[GEN3_LINK_PLAYER_BLOCK_SIZE];
    gen3_link_player_block_build(&lp, out);
    ASSERT_MEM_EQ(out, magic, 16);
    ASSERT_EQ(out[0x10], 0x02);
    ASSERT_EQ(out[0x11], 0x40);
    const uint8_t tid[4] = {0x78, 0x56, 0x34, 0x12};
    ASSERT_MEM_EQ(out + 0x14, tid, 4);
    ASSERT_MEM_EQ(out + 0x18, lp.name, 11);
    ASSERT_EQ(out[0x23], 1);
    const uint8_t lt[4] = {0x33, 0x11, 0, 0};
    ASSERT_MEM_EQ(out + 0x24, lt, 4);
    ASSERT_EQ(out[0x2A], 0x02);
    ASSERT_EQ(out[0x2B], 0);
    ASSERT_MEM_EQ(out + 0x2C, magic, 16);
}

TEST(test_lp_roundtrip) {
    Gen3LinkPlayer lp, back;
    make_lp(&lp);
    lp.lp_field_2 = 0x8000;
    lp.id = 1;
    uint8_t out[GEN3_LINK_PLAYER_BLOCK_SIZE];
    gen3_link_player_block_build(&lp, out);
    memset(&back, 0xAA, sizeof(back));
    ASSERT_TRUE(gen3_link_player_block_parse(out, &back));
    ASSERT_EQ(back.version, lp.version);
    ASSERT_EQ(back.lp_field_2, lp.lp_field_2);
    ASSERT_EQ(back.trainer_id, lp.trainer_id);
    ASSERT_MEM_EQ(back.name, lp.name, 11);
    ASSERT_EQ(back.gender, lp.gender);
    ASSERT_EQ(back.link_type, lp.link_type);
    ASSERT_EQ(back.id, lp.id);
    ASSERT_EQ(back.language, lp.language);
}

TEST(test_lp_bad_magic1) {
    Gen3LinkPlayer lp, back;
    make_lp(&lp);
    uint8_t out[GEN3_LINK_PLAYER_BLOCK_SIZE];
    gen3_link_player_block_build(&lp, out);
    out[3] ^= 1;
    ASSERT_TRUE(!gen3_link_player_block_parse(out, &back));
}

TEST(test_lp_bad_magic2) {
    Gen3LinkPlayer lp, back;
    make_lp(&lp);
    uint8_t out[GEN3_LINK_PLAYER_BLOCK_SIZE];
    gen3_link_player_block_build(&lp, out);
    out[0x2C + 3] ^= 1;
    ASSERT_TRUE(!gen3_link_player_block_parse(out, &back));
}

TEST(test_lp_magic_byte15_ignored) {
    Gen3LinkPlayer lp, back;
    make_lp(&lp);
    uint8_t out[GEN3_LINK_PLAYER_BLOCK_SIZE];
    gen3_link_player_block_build(&lp, out);
    out[15] = 0x55;
    out[0x3B] = 0x55;
    ASSERT_TRUE(gen3_link_player_block_parse(out, &back));
}

TEST(test_card_layout) {
    Gen3LinkPlayer lp;
    make_lp(&lp);
    uint8_t out[GEN3_TRAINER_CARD_BLOCK_SIZE];
    memset(out, 0x77, sizeof(out));
    gen3_trainer_card_build(&lp, out);
    ASSERT_EQ(out[0], 1);
    ASSERT_EQ(out[1], 0);
    ASSERT_EQ(out[2], 1);
    ASSERT_EQ(out[0x0E], 0x78);
    ASSERT_EQ(out[0x0F], 0x56);
    for(int i = 0x28; i < 0x30; i++) ASSERT_EQ(out[i], 0xFF);
    ASSERT_MEM_EQ(out + 0x30, lp.name, 8);
    for(int i = 0x24; i < 0x28; i++) ASSERT_EQ(out[i], 0);
    for(int i = 0x38; i < 0x64; i++) ASSERT_EQ(out[i], 0);
    for(int i = 3; i < 0x0E; i++) ASSERT_EQ(out[i], 0);
    for(int i = 0x10; i < 0x24; i++) ASSERT_EQ(out[i], 0);
}

int main(void) {
    RUN(test_lp_build_layout);
    RUN(test_lp_roundtrip);
    RUN(test_lp_bad_magic1);
    RUN(test_lp_bad_magic2);
    RUN(test_lp_magic_byte15_ignored);
    RUN(test_card_layout);
    TEST_EXIT();
}
