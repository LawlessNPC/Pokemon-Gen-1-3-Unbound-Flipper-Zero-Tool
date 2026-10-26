#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GEN3_LINK_PLAYER_BLOCK_SIZE  60
#define GEN3_TRAINER_CARD_BLOCK_SIZE 100
#define GEN3_LINKTYPE_TRADE_SETUP    0x1133
#define GEN3_VERSION_RUBY            0x4002
#define GEN3_LANGUAGE_ENGLISH        2

typedef struct {
    uint16_t version;
    uint16_t lp_field_2;
    uint32_t trainer_id; /* low 16 = TID, high 16 = SID */
    uint8_t name[11];
    uint8_t gender;
    uint32_t link_type;
    uint16_t id;
    uint16_t language;
} Gen3LinkPlayer;

void gen3_link_player_block_build(const Gen3LinkPlayer* lp, uint8_t out[GEN3_LINK_PLAYER_BLOCK_SIZE]);

/* false if either magic's first 15 bytes differ from "GameFreak inc.\0" (byte 15 is not compared) */
bool gen3_link_player_block_parse(const uint8_t in[GEN3_LINK_PLAYER_BLOCK_SIZE], Gen3LinkPlayer* lp);

/* 100-byte trainer card for block request type 2 (§A5 table), from lp's gender, TID and name[0..7] */
void gen3_trainer_card_build(const Gen3LinkPlayer* lp, uint8_t out[GEN3_TRAINER_CARD_BLOCK_SIZE]);

/* Part B link types (docs/gen3/link-protocol-trade.md §B0) */
#define GEN3_LINKTYPE_TRADE              0x1111 /* room again after the trade menu (§B0 row 1') */
#define GEN3_LINKTYPE_TRADE_CONNECTING   0x1122 /* trade menu (§B1.6) */
#define GEN3_LINKTYPE_TRADE_DISCONNECTED 0x1144 /* trade animation and link save (§B4.1) */
/* lp_field_2 that makes Ruby unlock its National Dex (§B4.5) */
#define GEN3_LP_NATIONAL_DEX 0x8000
