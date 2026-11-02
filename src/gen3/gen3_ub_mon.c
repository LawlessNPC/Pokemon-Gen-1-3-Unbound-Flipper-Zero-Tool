#include "gen3_ub_mon.h"
#include "gen3_ub_data.h"
#include "gen3_rules.h"
#include "gen3_text.h"
#include <string.h>

/* The record layouts, the identity solver, the stat formulas and the new-record defaults are ported
 * from PUSE (github.com/Zannael/PUSE, MIT, commit 2c624440fb24940480a3430fa17214942d4ba04a):
 * backend/modules/party.py and pc.py, and backend/data/profiles/unbound-2.1.1.1.json. */

#define OFF_PID 0u
#define OFF_OTID 4u
#define OFF_NICK 8u
#define OFF_LANG 18u
#define OFF_FLAGS 19u
#define OFF_OT 20u
#define OFF_CHECKSUM 28u
#define OFF_P_PP 52u
#define OFF_P_LEVEL 84u
#define OFF_P_HP 86u
#define OFF_P_STATS 88u
#define BOX_MOVES 39u
#define UB_SHEDINJA 303u

typedef struct {
    uint8_t species, item, exp, pp_ups, friendship, ball, evs, ivs;
} Layout;

static const Layout box_layout = {28, 30, 32, 36, 37, 38, 44, 54};
/* Growth at +32, Attacks at +44, EVs at +56, Misc at +68 (IV word at +4 of it) */
static const Layout party_layout = {32, 34, 36, 40, 41, 42, 56, 72};

static const Layout* lay(const Gen3UbMon* m) {
    return m->party ? &party_layout : &box_layout;
}

static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t* p, uint32_t v) {
    for(uint8_t i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

uint16_t gen3_ub_mon_species(const Gen3UbMon* m) {
    return rd16(&m->raw[lay(m)->species]);
}
void gen3_ub_mon_set_species(Gen3UbMon* m, uint16_t v) {
    wr16(&m->raw[lay(m)->species], v);
}
uint16_t gen3_ub_mon_item(const Gen3UbMon* m) {
    return rd16(&m->raw[lay(m)->item]);
}
void gen3_ub_mon_set_item(Gen3UbMon* m, uint16_t v) {
    wr16(&m->raw[lay(m)->item], v);
}
uint32_t gen3_ub_mon_exp(const Gen3UbMon* m) {
    return rd32(&m->raw[lay(m)->exp]);
}
void gen3_ub_mon_set_exp(Gen3UbMon* m, uint32_t v) {
    wr32(&m->raw[lay(m)->exp], v);
}
uint8_t gen3_ub_mon_pp_ups(const Gen3UbMon* m) {
    return m->raw[lay(m)->pp_ups];
}
void gen3_ub_mon_set_pp_ups(Gen3UbMon* m, uint8_t v) {
    m->raw[lay(m)->pp_ups] = v;
}
uint8_t gen3_ub_mon_friendship(const Gen3UbMon* m) {
    return m->raw[lay(m)->friendship];
}
void gen3_ub_mon_set_friendship(Gen3UbMon* m, uint8_t v) {
    m->raw[lay(m)->friendship] = v;
}
uint8_t gen3_ub_mon_ball(const Gen3UbMon* m) {
    return m->raw[lay(m)->ball];
}
void gen3_ub_mon_set_ball(Gen3UbMon* m, uint8_t v) {
    m->raw[lay(m)->ball] = v;
}

void gen3_ub_mon_moves(const Gen3UbMon* m, uint16_t out[4]) {
    if(m->party) {
        for(uint8_t i = 0; i < 4; i++) out[i] = rd16(&m->raw[44u + 2u * i]);
        return;
    }
    uint64_t packed = 0;
    for(uint8_t i = 0; i < 5; i++) packed |= (uint64_t)m->raw[BOX_MOVES + i] << (8 * i);
    for(uint8_t i = 0; i < 4; i++) out[i] = (uint16_t)((packed >> (10 * i)) & 0x3FFu);
}

void gen3_ub_mon_set_moves(Gen3UbMon* m, const uint16_t in[4]) {
    if(m->party) {
        for(uint8_t i = 0; i < 4; i++) wr16(&m->raw[44u + 2u * i], in[i]);
        return;
    }
    uint64_t packed = 0;
    for(uint8_t i = 0; i < 4; i++) packed |= (uint64_t)(in[i] & 0x3FFu) << (10 * i);
    for(uint8_t i = 0; i < 5; i++) m->raw[BOX_MOVES + i] = (uint8_t)(packed >> (8 * i));
}

void gen3_ub_mon_pp(const Gen3UbMon* m, uint8_t out[4]) {
    for(uint8_t i = 0; i < 4; i++) out[i] = m->party ? m->raw[OFF_P_PP + i] : 0;
}
void gen3_ub_mon_set_pp(Gen3UbMon* m, const uint8_t in[4]) {
    if(!m->party) return;
    memcpy(&m->raw[OFF_P_PP], in, 4);
}

void gen3_ub_mon_evs(const Gen3UbMon* m, uint8_t out[6]) {
    memcpy(out, &m->raw[lay(m)->evs], 6);
}
void gen3_ub_mon_set_evs(Gen3UbMon* m, const uint8_t in[6]) {
    memcpy(&m->raw[lay(m)->evs], in, 6);
}

void gen3_ub_mon_ivs(const Gen3UbMon* m, uint8_t out[6]) {
    uint32_t w = rd32(&m->raw[lay(m)->ivs]);
    for(uint8_t i = 0; i < 6; i++) out[i] = (uint8_t)((w >> (5 * i)) & 0x1Fu);
}
void gen3_ub_mon_set_ivs(Gen3UbMon* m, const uint8_t in[6]) {
    uint8_t* p = &m->raw[lay(m)->ivs];
    uint32_t w = rd32(p) & 0xC0000000u;
    for(uint8_t i = 0; i < 6; i++) w |= (uint32_t)(in[i] & 0x1Fu) << (5 * i);
    wr32(p, w);
}
bool gen3_ub_mon_hidden(const Gen3UbMon* m) {
    return (rd32(&m->raw[lay(m)->ivs]) >> 31) & 1u;
}
void gen3_ub_mon_set_hidden(Gen3UbMon* m, bool v) {
    uint8_t* p = &m->raw[lay(m)->ivs];
    uint32_t w = rd32(p) & 0x7FFFFFFFu;
    wr32(p, w | (v ? 0x80000000u : 0u));
}

uint32_t gen3_ub_mon_pid(const Gen3UbMon* m) {
    return rd32(&m->raw[OFF_PID]);
}
void gen3_ub_mon_set_pid(Gen3UbMon* m, uint32_t v) {
    wr32(&m->raw[OFF_PID], v);
}
uint32_t gen3_ub_mon_otid(const Gen3UbMon* m) {
    return rd32(&m->raw[OFF_OTID]);
}
void gen3_ub_mon_set_otid(Gen3UbMon* m, uint32_t v) {
    wr32(&m->raw[OFF_OTID], v);
}
void gen3_ub_mon_nickname(const Gen3UbMon* m, uint8_t out[GEN3_UB_MON_NICK]) {
    memcpy(out, &m->raw[OFF_NICK], GEN3_UB_MON_NICK);
}
void gen3_ub_mon_set_nickname(Gen3UbMon* m, const uint8_t in[GEN3_UB_MON_NICK]) {
    memcpy(&m->raw[OFF_NICK], in, GEN3_UB_MON_NICK);
}
void gen3_ub_mon_ot_name(const Gen3UbMon* m, uint8_t out[GEN3_UB_MON_OT]) {
    memcpy(out, &m->raw[OFF_OT], GEN3_UB_MON_OT);
}
void gen3_ub_mon_set_ot_name(Gen3UbMon* m, const uint8_t in[GEN3_UB_MON_OT]) {
    memcpy(&m->raw[OFF_OT], in, GEN3_UB_MON_OT);
}

uint8_t gen3_ub_mon_level(const Gen3UbMon* m) {
    if(m->party) return m->raw[OFF_P_LEVEL];
    int g = gen3_ub_growth(gen3_ub_mon_species(m), NULL);
    return g < 0 ? 0 : gen3_level_for_exp((uint8_t)g, gen3_ub_mon_exp(m));
}
void gen3_ub_mon_set_level(Gen3UbMon* m, uint8_t v) {
    if(m->party) m->raw[OFF_P_LEVEL] = v;
}

uint8_t gen3_ub_nature(const Gen3UbMon* m) {
    return (uint8_t)(gen3_ub_mon_pid(m) % 25u);
}

static bool shiny_pid(uint32_t otid, uint32_t pid) {
    return ((otid >> 16) ^ (otid & 0xFFFFu) ^ (pid >> 16) ^ (pid & 0xFFFFu)) < 16u;
}

bool gen3_ub_shiny(const Gen3UbMon* m) {
    return shiny_pid(gen3_ub_mon_otid(m), gen3_ub_mon_pid(m));
}

uint8_t gen3_ub_ability_slot(const Gen3UbMon* m) {
    return gen3_ub_mon_hidden(m) ? 2u : (uint8_t)(gen3_ub_mon_pid(m) & 1u);
}

/* 0 male, 1 female, 2 genderless by the threshold byte; -1 for an unknown species */
static int gender_of(int thr, uint32_t pid) {
    if(thr < 0) return -1;
    if(thr == 255) return 2;
    if(thr == 0) return 0;
    if(thr == 254) return 1;
    return (int)(pid & 0xFFu) < thr ? 1 : 0;
}

int gen3_ub_gender(const Gen3UbMon* m) {
    int g = gender_of(gen3_ub_gender_threshold(gen3_ub_mon_species(m)), gen3_ub_mon_pid(m));
    return g < 0 ? 2 : g;
}

/* The high half nearest to preferred with the residue req mod 25, within 0..0xFFFF */
static uint32_t nearest_high(uint32_t req, uint32_t preferred) {
    int32_t k = (((int32_t)preferred - (int32_t)req + 12) + 25 * 4000) / 25 - 4000;
    int32_t max_k = (int32_t)((0xFFFFu - req) / 25u);
    if(k < 0) k = 0;
    if(k > max_k) k = max_k;
    return req + 25u * (uint32_t)k;
}

/* The first non-shiny PID with this low half and a high half of the residue req, trying the nearest
 * to cur_high and then 25 steps either side; false if none. */
static bool pick_plain(uint32_t otid, uint32_t low, uint32_t req, uint32_t cur_high, uint32_t* out) {
    uint32_t base = nearest_high(req, cur_high);
    for(int32_t n = 0; n <= 5; n++) {
        for(int32_t sign = -1; sign <= 1; sign += 2) {
            if(n == 0 && sign == 1) continue;
            int32_t h = (int32_t)base + sign * 25 * n;
            if(h < 0 || h > 0xFFFF) continue;
            uint32_t pid = ((uint32_t)h << 16) | low;
            if(!shiny_pid(otid, pid)) {
                *out = pid;
                return true;
            }
        }
    }
    return false;
}

bool gen3_ub_solve_identity(Gen3UbMon* m, uint8_t nature, int shiny, int gender, int ability_slot) {
    uint32_t pid = gen3_ub_mon_pid(m);
    uint32_t otid = gen3_ub_mon_otid(m);
    uint32_t nat = nature % 25u;
    int thr = gen3_ub_gender_threshold(gen3_ub_mon_species(m));
    bool dynamic = thr > 0 && thr < 254;

    if(shiny < 0) shiny = shiny_pid(otid, pid);
    if(gender < 0) gender = (thr >= 0 && thr != 255) ? gender_of(thr, pid) : -1;
    if(gender >= 0 && !dynamic && gender_of(thr, 0) != gender) return false;
    if(gender > 1 && thr != 255) return false;

    uint32_t need_bit = (ability_slot == 0 || ability_slot == 1) ? (uint32_t)ability_slot :
                                                                   (pid & 1u);
    uint32_t tid_sid = (otid & 0xFFFFu) ^ (otid >> 16);
    uint32_t cur_high = pid >> 16;
    uint32_t found = 0;
    bool ok = false;

    for(uint32_t delta = 0; delta < 0x10000u && !ok; delta++) {
        uint32_t low = ((pid & 0xFFFFu) + delta) & 0xFFFFu;
        if((low & 1u) != need_bit) continue;
        if(dynamic && gender >= 0 && gender_of(thr, low) != gender) continue;
        uint32_t req = ((nat + 25u - low % 25u) * 16u) % 25u;
        if(shiny) {
            for(uint32_t sv = 0; sv < 16u; sv++) {
                uint32_t high = (tid_sid ^ low ^ sv) & 0xFFFFu;
                if(high % 25u != req) continue;
                found = (high << 16) | low;
                ok = true;
                break;
            }
        } else {
            ok = pick_plain(otid, low, req, cur_high, &found);
        }
    }
    if(!ok) return false;

    gen3_ub_mon_set_pid(m, found);
    if(ability_slot == 2) gen3_ub_mon_set_hidden(m, true);
    else if(ability_slot == 0 || ability_slot == 1) gen3_ub_mon_set_hidden(m, false);
    return true;
}

/* The nature's raised stat is 1 + nature / 5 and its lowered one 1 + nature % 5, in the order
 * Atk, Def, Spe, SpA, SpD; the same index means neutral. stat is HP 0, Atk 1, Def 2, Spe 3, ... */
static uint32_t nature_adjust(uint32_t value, uint8_t nature, uint8_t stat) {
    uint8_t up = nature / 5u, down = nature % 5u;
    if(up == down) return value;
    if(stat == up + 1u) return value * 110u / 100u;
    if(stat == down + 1u) return value * 90u / 100u;
    return value;
}

void gen3_ub_party_restat(Gen3UbMon* m) {
    if(!m->party) return;
    uint16_t species = gen3_ub_mon_species(m);
    int g = gen3_ub_growth(species, NULL);
    if(g >= 0) m->raw[OFF_P_LEVEL] = gen3_level_for_exp((uint8_t)g, gen3_ub_mon_exp(m));
    wr16(&m->raw[OFF_CHECKSUM], 0);

    uint8_t base[6];
    if(!gen3_ub_base_stats(species, base)) return;
    uint8_t ivs[6], evs[6];
    gen3_ub_mon_ivs(m, ivs);
    gen3_ub_mon_evs(m, evs);
    uint32_t level = m->raw[OFF_P_LEVEL];
    if(level < 1u) level = 1u;
    uint8_t nature = gen3_ub_nature(m);

    uint16_t stats[6];
    for(uint8_t i = 0; i < 6; i++) {
        uint32_t core = ((2u * base[i] + ivs[i] + evs[i] / 4u) * level) / 100u;
        uint32_t v = i == 0 ? core + level + 10u : nature_adjust(core + 5u, nature, i);
        if(v < 1u) v = 1u;
        stats[i] = (uint16_t)v;
    }
    if(species == UB_SHEDINJA) stats[0] = 1;

    uint16_t hp = rd16(&m->raw[OFF_P_HP]);
    if(hp > stats[0]) hp = stats[0];
    wr16(&m->raw[OFF_P_HP], hp);
    for(uint8_t i = 0; i < 6; i++) wr16(&m->raw[OFF_P_STATS + 2u * i], stats[i]);
}

bool gen3_ub_new_box_mon(Gen3UbMon* m, uint16_t species, uint8_t level, const Gen3UbMon* owner) {
    char name[GEN3_UB_NAME_LEN + 1u];
    if(!gen3_ub_species_real(species)) return false;
    int g = gen3_ub_growth(species, NULL);
    if(g < 0 || !gen3_ub_species_name(species, name)) return false;
    if(level < 1u) level = 1u;
    if(level > 100u) level = 100u;
    uint32_t exp = gen3_exp_for_level((uint8_t)g, level);
    if(exp == 0u) exp = 1u;

    memset(m, 0, sizeof(*m));
    m->party = false;
    gen3_ub_mon_set_species(m, species);
    gen3_ub_mon_set_exp(m, exp);
    gen3_ub_mon_set_ball(m, 3);
    gen3_ub_mon_set_friendship(m, 70);
    gen3_ub_mon_set_otid(m, gen3_ub_mon_otid(owner));
    memcpy(&m->raw[OFF_OT], &owner->raw[OFF_OT], GEN3_UB_MON_OT);
    m->raw[OFF_LANG] = owner->raw[OFF_LANG];
    m->raw[OFF_FLAGS] = owner->raw[OFF_FLAGS] & (uint8_t)~0x05u;
    gen3_text_encode(name, &m->raw[OFF_NICK], GEN3_UB_MON_NICK, GEN3_UB_MON_NICK);

    uint16_t moves[4] = {0, 0, 0, 0};
    uint16_t list[4];
    uint8_t n = gen3_ub_levelup(species, list, 4);
    for(uint8_t i = 0; i < n && i < 4u; i++) moves[i] = list[i];
    gen3_ub_mon_set_moves(m, moves);

    const uint8_t ivs[6] = {31, 31, 31, 31, 31, 31};
    gen3_ub_mon_set_ivs(m, ivs);

    /* Ability slot 0: an odd PID moves up 25, an even step that keeps the nature */
    uint32_t pid = species * 2654435761u;
    if((pid & 1u) && pid < 0xFFFFFFE6u) pid += 25u;
    gen3_ub_mon_set_pid(m, pid);
    return true;
}
