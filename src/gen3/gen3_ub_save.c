#include <src/gen3/gen3_ub_save.h>

#include <src/gen3/gen3_rules.h>
#include <src/gen3/gen3_text.h>

#define UB_FOOTER_ID 0xFF4u
#define UB_FOOTER_SUM 0xFF6u
#define UB_FOOTER_SIG 0xFF8u
#define UB_FOOTER_COUNTER 0xFFCu
#define UB_REGIONS 13u
#define UB_PHYS 30u /* a region id from here on is a physical sector */

/* Each section's checksummed length */
static const uint16_t ub_sum_len[GEN3_UB_SLOT_SECTORS] = {
    0xF24, 0xFF0, 0xFF0, 0xFF0, 0xD98, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0x450};

/* The PC stream, in order: (section id, or physical sector 30/31; start; end) */
static const uint16_t ub_region[UB_REGIONS][3] = {
    {5, 4, 4080},
    {6, 0, 4080},
    {7, 0, 4080},
    {8, 0, 4080},
    {9, 0, 4080},
    {10, 0, 4080},
    {11, 0, 4080},
    {12, 0, 4080},
    {13, 0, 424},
    {30, 2828, 4080},
    {31, 0, 3968},
    {2, 3864, 4080},
    {3, 0, 3264},
};

static uint16_t ub_le16(const uint8_t* p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t ub_le32(const uint8_t* p) {
    return (uint32_t)ub_le16(p) | (uint32_t)ub_le16(p + 2) << 16;
}

static void ub_put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void ub_put32(uint8_t* p, uint32_t v) {
    ub_put16(p, (uint16_t)v);
    ub_put16(p + 2, (uint16_t)(v >> 16));
}

uint16_t gen3_ub_section_sum(const uint8_t sector[GEN3_UB_SECTOR_SIZE], uint16_t id) {
    if(id >= GEN3_UB_SLOT_SECTORS) return 0;
    uint32_t sum = 0;
    for(uint32_t i = 0; i < ub_sum_len[id]; i += 4u) sum += ub_le32(sector + i);
    return (uint16_t)((sum & 0xFFFFu) + (sum >> 16));
}

void gen3_ub_sector_info(const uint8_t sector[GEN3_UB_SECTOR_SIZE], Gen3UbSector* out) {
    out->id = ub_le16(sector + UB_FOOTER_ID);
    out->checksum = ub_le16(sector + UB_FOOTER_SUM);
    out->signature = ub_le32(sector + UB_FOOTER_SIG);
    out->counter = ub_le32(sector + UB_FOOTER_COUNTER);
    out->sig_ok = out->signature == GEN3_UB_SIG_A || out->signature == GEN3_UB_SIG_B;
    out->sum_ok = false;
    if(out->id >= GEN3_UB_SLOT_SECTORS) return;
    if(out->id == GEN3_UB_OPAQUE_ID) {
        out->sum_ok = true;
        return;
    }
    out->sum_ok = gen3_ub_section_sum(sector, out->id) == out->checksum;
}

void gen3_ub_set_footer(uint8_t sector[GEN3_UB_SECTOR_SIZE], const Gen3UbSector* f) {
    ub_put16(sector + UB_FOOTER_ID, f->id);
    ub_put16(sector + UB_FOOTER_SUM, f->checksum);
    ub_put32(sector + UB_FOOTER_SIG, f->signature);
    ub_put32(sector + UB_FOOTER_COUNTER, f->counter);
}

/* Slot k (0 or 1) of infos: valid, its counter, and its id -> sector map */
static bool ub_slot(const Gen3UbSector* infos, uint8_t k, uint32_t* counter, uint8_t phys[14]) {
    uint16_t seen = 0;
    for(uint8_t j = 0; j < GEN3_UB_SLOT_SECTORS; j++) {
        const uint8_t sector = (uint8_t)(k * GEN3_UB_SLOT_SECTORS + j);
        const Gen3UbSector* s = &infos[sector];
        if(!s->sig_ok || !s->sum_ok || s->id >= GEN3_UB_SLOT_SECTORS) return false;
        if(seen & (1u << s->id)) return false;
        if(j == 0) *counter = s->counter;
        if(s->counter != *counter) return false;
        seen |= (uint16_t)(1u << s->id);
        phys[s->id] = sector;
    }
    return true;
}

void gen3_ub_slots(const Gen3UbSector infos[2u * GEN3_UB_SLOT_SECTORS], Gen3UbSlots* out) {
    uint8_t phys[2][GEN3_UB_SLOT_SECTORS];
    for(uint8_t k = 0; k < 2u; k++) {
        out->counter[k] = 0;
        out->valid[k] = ub_slot(infos, k, &out->counter[k], phys[k]);
    }
    out->loaded = -1;
    if(out->valid[0] && out->valid[1]) {
        const uint32_t delta = out->counter[1] - out->counter[0];
        out->loaded = delta != 0 && delta < 0x80000000u ? 1 : 0;
    } else if(out->valid[0]) {
        out->loaded = 0;
    } else if(out->valid[1]) {
        out->loaded = 1;
    }
    for(uint8_t id = 0; id < GEN3_UB_SLOT_SECTORS; id++) {
        out->phys[id] = out->loaded < 0 ? 0xFFu : phys[out->loaded][id];
    }
}

void gen3_ub_trainer(const uint8_t section0[GEN3_UB_SECTOR_SIZE], Gen3UbTrainer* out) {
    gen3_text_decode(section0, 7, out->name, sizeof(out->name));
    out->gender = section0[0x08];
    out->tid = ub_le16(section0 + 0x0A);
    out->sid = ub_le16(section0 + 0x0C);
}

uint8_t gen3_ub_party_count(const uint8_t section1[GEN3_UB_SECTOR_SIZE]) {
    const uint8_t n = section1[0x34];
    return n > GEN3_UB_PARTY_MAX ? (uint8_t)GEN3_UB_PARTY_MAX : n;
}

bool gen3_ub_box_pieces(const Gen3UbSlots* s, uint8_t box, uint8_t slot, Gen3UbPieces* out) {
    if(s->loaded < 0 || box >= GEN3_UB_BOXES || slot >= GEN3_UB_BOX_SLOTS) return false;
    uint32_t off = ((uint32_t)box * GEN3_UB_BOX_SLOTS + slot) * GEN3_UB_BOX_MON;
    uint32_t left = GEN3_UB_BOX_MON;
    uint32_t base = 0;
    out->n = 0;
    for(uint32_t r = 0; r < UB_REGIONS && left; r++) {
        const uint32_t len = (uint32_t)ub_region[r][2] - ub_region[r][1];
        if(off < base + len) {
            const uint32_t at = off - base;
            const uint32_t take = len - at < left ? len - at : left;
            const uint16_t id = ub_region[r][0];
            out->sector[out->n] = id >= UB_PHYS ? (uint8_t)id : s->phys[id];
            out->off[out->n] = (uint16_t)(ub_region[r][1] + at);
            out->len[out->n] = (uint16_t)take;
            out->n++;
            off += take;
            left -= take;
        }
        base += len;
    }
    return left == 0;
}

static bool ub_egg(uint32_t ivs) {
    return (ivs >> 30) & 1u;
}

void gen3_ub_box_summary(
    const uint8_t rec[GEN3_UB_BOX_MON],
    Gen3UbGrowthFn g,
    void* ctx,
    Gen3UbSummary* out) {
    out->species = ub_le16(rec + 28);
    const int growth = g ? g(out->species, ctx) : -1;
    out->level = growth >= 0 && growth <= 5 ? gen3_level_for_exp((uint8_t)growth, ub_le32(rec + 32)) : 0;
    out->egg = ub_egg(ub_le32(rec + 54));
    gen3_text_decode(rec + 8, 10, out->nickname, sizeof(out->nickname));
}

void gen3_ub_party_summary(const uint8_t rec[GEN3_UB_PARTY_MON], Gen3UbSummary* out) {
    out->species = ub_le16(rec + 32);
    out->level = rec[84];
    out->egg = ub_egg(ub_le32(rec + 72));
    gen3_text_decode(rec + 8, 10, out->nickname, sizeof(out->nickname));
}
