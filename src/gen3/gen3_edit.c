#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_data.h>

#include <string.h>

/* OT name bytes in the PKHeX .pk3 layout (gen3_pkm.c). A species change copies them raw so an
 * OT name with characters the app can't decode stays the game's own. */
#define GEN3_EDIT_OT_NAME_OFF 0x14

_Static_assert(Gen3PresetMaxIvMaxEv + 1 == GEN3_PRESET_COUNT, "GEN3_PRESET_COUNT");

static const Gen3SpeciesInfo* ed_info(const Gen3Pkm* p) {
    return gen3_species(gen3_national_from_internal(gen3_pkm_get(p, Gen3FieldSpecies)));
}

static uint8_t ed_level(const Gen3Pkm* p) {
    const Gen3SpeciesInfo* info = ed_info(p);
    if(!info) return gen3_pkm_get(p, Gen3FieldLevel);
    return gen3_level_for_exp(info->growth, gen3_pkm_get(p, Gen3FieldExp));
}

static bool ed_two_abilities(const Gen3Pkm* p) {
    return gen3_species_two_abilities(
        gen3_national_from_internal(gen3_pkm_get(p, Gen3FieldSpecies)));
}

/* After a new PID: the ability slot it was chosen for, which is pid & 1 (CreateBoxMon; and
 * gen3_pid_find matches pid & 1 to the wanted slot on a two-ability species). */
static void ed_ability_from_pid(Gen3Pkm* p) {
    gen3_pkm_set(
        p, Gen3FieldAbilityBit, ed_two_abilities(p) ? (gen3_pkm_get(p, Gen3FieldPid) & 1) : 0);
}

/* Every mutator ends here: ability bit 0 on a one-ability species (a two-ability species keeps
 * its stored slot, so an unrelated edit never changes its ability), party stats, checksum. */
static void ed_commit(Gen3Editor* ed) {
    Gen3Pkm* p = &ed->pkm;
    if(!ed_two_abilities(p)) gen3_pkm_set(p, Gen3FieldAbilityBit, 0);
    gen3_recalc_party(p);
    gen3_pkm_fix_checksum(p);
}

static bool ed_level_ev(Gen3Preset preset) {
    return preset == Gen3PresetRandIvLevelEv || preset == Gen3PresetMaxIvLevelEv;
}

/* EVs from the preset: 0, GEN3_EDIT_MAX_EV * level / 100 or GEN3_EDIT_MAX_EV. */
static void ed_apply_evs(Gen3Editor* ed) {
    uint8_t ev = 0;
    if(ed_level_ev(ed->preset))
        ev = GEN3_EDIT_MAX_EV * ed_level(&ed->pkm) / 100;
    else if(ed->preset == Gen3PresetRandIvMaxEv || ed->preset == Gen3PresetMaxIvMaxEv)
        ev = GEN3_EDIT_MAX_EV;
    for(int s = 0; s < Gen3StatCount; s++) gen3_pkm_set(&ed->pkm, Gen3FieldEvHp + s, ev);
}

/* IVs from the preset (random ones re-rolled from one RNG word, 5 bits each), then EVs. */
static void ed_apply_preset(Gen3Editor* ed) {
    bool max_iv = ed->preset >= Gen3PresetMaxIvZeroEv;
    uint32_t r = max_iv ? 0 : gen3_rng_next(&ed->rng);
    for(int s = 0; s < Gen3StatCount; s++) {
        gen3_pkm_set(&ed->pkm, Gen3FieldIvHp + s, max_iv ? 31 : (r >> (5 * s)) & 31);
    }
    ed_apply_evs(ed);
    ed->preset_applied = true;
}

static void ed_set_default_moves(Gen3Pkm* p, uint16_t national, uint8_t level) {
    uint16_t moves[4];
    gen3_default_moves(national, level, moves);
    for(int s = 0; s < 4; s++) {
        gen3_pkm_set(p, Gen3FieldMove1 + s, moves[s]);
        gen3_pkm_set(p, Gen3FieldPp1 + s, moves[s] ? gen3_move_pp(moves[s]) : 0);
    }
    gen3_pkm_set(p, Gen3FieldPpBonuses, 0);
}

/* Stable partition: occupied slots first, each slot carrying its move, PP and 2 PP Up bits. */
static void ed_pack_moves(Gen3Pkm* p) {
    uint16_t mv[4];
    uint8_t pp[4];
    uint8_t ups = gen3_pkm_get(p, Gen3FieldPpBonuses);
    uint8_t order[4];
    int n = 0;
    for(int s = 0; s < 4; s++) {
        mv[s] = gen3_pkm_get(p, Gen3FieldMove1 + s);
        pp[s] = gen3_pkm_get(p, Gen3FieldPp1 + s);
        if(mv[s]) order[n++] = s;
    }
    for(int s = 0; s < 4; s++)
        if(!mv[s]) order[n++] = s;
    uint8_t new_ups = 0;
    for(int s = 0; s < 4; s++) {
        gen3_pkm_set(p, Gen3FieldMove1 + s, mv[order[s]]);
        gen3_pkm_set(p, Gen3FieldPp1 + s, pp[order[s]]);
        new_ups |= ((ups >> (2 * order[s])) & 3) << (2 * s);
    }
    gen3_pkm_set(p, Gen3FieldPpBonuses, new_ups);
}

static void ed_clear_move(Gen3Pkm* p, int slot) {
    gen3_pkm_set(p, Gen3FieldMove1 + slot, 0);
    gen3_pkm_set(p, Gen3FieldPp1 + slot, 0);
    uint8_t ups = gen3_pkm_get(p, Gen3FieldPpBonuses);
    gen3_pkm_set(p, Gen3FieldPpBonuses, ups & ~(3u << (2 * slot)));
}

/* D5: a fresh Pokemon (as CreateBoxMon) keeping OT name, OT ID, OT gender; the preset is
 * applied. */
static void ed_new_mon(Gen3Editor* ed, uint16_t national, uint8_t level) {
    const Gen3SpeciesInfo* info = gen3_species(national);
    if(!info) return;
    if(level < 1) level = 1;
    if(level > 100) level = 100;
    const Gen3Pkm* old = &ed->pkm;
    uint32_t ot_id = gen3_pkm_get(old, Gen3FieldOtId);
    Gen3Pkm p;
    memset(&p, 0, sizeof(p));
    memcpy(p.raw + GEN3_EDIT_OT_NAME_OFF, old->raw + GEN3_EDIT_OT_NAME_OFF, GEN3_OT_NAME_LEN);
    gen3_pkm_set(&p, Gen3FieldOtId, ot_id);
    gen3_pkm_set(&p, Gen3FieldOtGender, gen3_pkm_get(old, Gen3FieldOtGender));

    uint32_t pid;
    do {
        pid = gen3_rng_next(&ed->rng);
    } while(gen3_is_shiny(ot_id, pid));
    gen3_pkm_set(&p, Gen3FieldPid, pid);
    char name[GEN3_SPECIES_NAME_LEN + 1];
    gen3_species_name_get(national, name);
    gen3_pkm_set_nickname(&p, name);
    gen3_pkm_set(&p, Gen3FieldLanguage, GEN3_MON_LANGUAGE_ENGLISH);
    gen3_pkm_set(&p, Gen3FieldHasSpecies, 1);
    gen3_pkm_set(&p, Gen3FieldSpecies, info->internal);
    gen3_pkm_set(&p, Gen3FieldExp, gen3_exp_for_level(info->growth, level));
    gen3_pkm_set(&p, Gen3FieldFriendship, info->friendship);
    ed_set_default_moves(&p, national, level);
    gen3_pkm_set(&p, Gen3FieldMetLocation, GEN3_METLOC_FATEFUL);
    gen3_pkm_set(&p, Gen3FieldMetLevel, level);
    gen3_pkm_set(&p, Gen3FieldMetGame, GEN3_EDIT_MET_GAME);
    gen3_pkm_set(&p, Gen3FieldBall, GEN3_EDIT_BALL);
    /* FireRed/LeafGreen/Emerald need it for Mew and Deoxys to obey. */
    if(national == GEN3_NATIONAL_MEW || national == GEN3_NATIONAL_DEOXYS)
        gen3_pkm_set(&p, Gen3FieldFateful, 1);
    gen3_pkm_set(&p, Gen3FieldMail, GEN3_MAIL_NONE);
    ed_ability_from_pid(&p);
    ed->pkm = p;
    ed_apply_preset(ed);
    ed_commit(ed);
}

void gen3_edit_init(Gen3Editor* ed, uint32_t seed) {
    memset(ed, 0, sizeof(*ed));
    gen3_rng_seed(&ed->rng, seed);
    ed->preset = Gen3PresetRandIvZeroEv;
    gen3_pkm_set_ot_name(&ed->pkm, GEN3_EDIT_DEFAULT_OT_NAME);
    gen3_pkm_set(
        &ed->pkm,
        Gen3FieldOtId,
        ((uint32_t)GEN3_EDIT_DEFAULT_SID << 16) | GEN3_EDIT_DEFAULT_TID);
    ed_new_mon(ed, GEN3_EDIT_DEFAULT_NATIONAL, GEN3_EDIT_DEFAULT_LEVEL);
}

bool gen3_normalise(Gen3Pkm* pkm, Gen3NormaliseInfo* info) {
    Gen3NormaliseInfo unused;
    if(!info) info = &unused;
    memset(info, 0, sizeof(*info));
    uint16_t national = gen3_national_from_internal(gen3_pkm_get(pkm, Gen3FieldSpecies));
    const Gen3SpeciesInfo* sp = gen3_species(national);
    if(!sp) return false;

    /* Moves: note gaps in the input, drop duplicates with their PP and PP Ups, then pack. */
    bool empty_seen = false;
    for(int s = 0; s < 4; s++) {
        uint16_t m = gen3_pkm_get(pkm, Gen3FieldMove1 + s);
        if(!m)
            empty_seen = true;
        else if(empty_seen)
            info->gaps_packed = true;
    }
    for(int s = 1; s < 4; s++) {
        uint16_t m = gen3_pkm_get(pkm, Gen3FieldMove1 + s);
        for(int t = 0; m && t < s; t++) {
            if(gen3_pkm_get(pkm, Gen3FieldMove1 + t) == m) {
                ed_clear_move(pkm, s);
                info->duplicates_removed = true;
                break;
            }
        }
    }
    ed_pack_moves(pkm);

    uint32_t exp = gen3_pkm_get(pkm, Gen3FieldExp);
    uint32_t lo = gen3_exp_for_level(sp->growth, 1);
    uint32_t hi = gen3_exp_for_level(sp->growth, 100);
    uint32_t clamped = exp < lo ? lo : (exp > hi ? hi : exp);
    if(clamped != exp) {
        gen3_pkm_set(pkm, Gen3FieldExp, clamped);
        info->exp_clamped = true;
    }
    if(gen3_pkm_get(pkm, Gen3FieldMove1) == 0) {
        ed_set_default_moves(pkm, national, ed_level(pkm));
        info->moves_defaulted = true;
    }
    if(!gen3_species_two_abilities(national) && gen3_pkm_get(pkm, Gen3FieldAbilityBit)) {
        gen3_pkm_set(pkm, Gen3FieldAbilityBit, 0);
        info->ability_fixed = true;
    }
    /* Recalculate, keeping the current HP clamped to the new MaxHp. */
    uint32_t hp = gen3_pkm_get(pkm, Gen3FieldHp);
    gen3_recalc_party(pkm);
    if(hp < gen3_pkm_get(pkm, Gen3FieldMaxHp)) gen3_pkm_set(pkm, Gen3FieldHp, hp);
    gen3_pkm_fix_checksum(pkm);
    return true;
}

void gen3_edit_load(Gen3Editor* ed, const Gen3Pkm* pkm) {
    ed->pkm = *pkm;
    ed->preset_applied = false;
    gen3_normalise(&ed->pkm, NULL);
}

uint16_t gen3_edit_national(const Gen3Editor* ed) {
    return gen3_national_from_internal(gen3_pkm_get(&ed->pkm, Gen3FieldSpecies));
}

void gen3_edit_set_species(Gen3Editor* ed, uint16_t national) {
    if(!gen3_species(national) || national == gen3_edit_national(ed)) return;
    ed_new_mon(ed, national, ed_level(&ed->pkm));
}

void gen3_edit_set_level(Gen3Editor* ed, uint8_t level) {
    const Gen3SpeciesInfo* info = ed_info(&ed->pkm);
    if(!info) return;
    if(level < 1) level = 1;
    if(level > 100) level = 100;
    gen3_pkm_set(&ed->pkm, Gen3FieldExp, gen3_exp_for_level(info->growth, level));
    if(ed->preset_applied && ed_level_ev(ed->preset)) ed_apply_evs(ed);
    ed_commit(ed);
}

void gen3_edit_set_preset(Gen3Editor* ed, Gen3Preset preset) {
    if((unsigned)preset >= GEN3_PRESET_COUNT) return;
    ed->preset = preset;
    ed_apply_preset(ed);
    ed_commit(ed);
}

void gen3_edit_set_nickname(Gen3Editor* ed, const char* ascii) {
    char name[GEN3_SPECIES_NAME_LEN + 1];
    if(!ascii || !ascii[0]) {
        gen3_species_name_get(gen3_edit_national(ed), name);
        ascii = name;
    }
    gen3_pkm_set_nickname(&ed->pkm, ascii);
    ed_commit(ed);
}

void gen3_edit_set_ot_name(Gen3Editor* ed, const char* ascii) {
    gen3_pkm_set_ot_name(&ed->pkm, ascii ? ascii : "");
    ed_commit(ed);
}

void gen3_edit_set_ot_id(Gen3Editor* ed, uint16_t tid, uint16_t sid) {
    Gen3Traits old;
    gen3_traits_of(&ed->pkm, &old);
    uint32_t ot_id = ((uint32_t)sid << 16) | tid;
    gen3_pkm_set(&ed->pkm, Gen3FieldOtId, ot_id);
    /* Both paths keep the stored ability slot: set_traits asks for old.ability_slot (the stored
     * bit), and ed_commit leaves a two-ability species' bit alone. */
    if(gen3_is_shiny(ot_id, gen3_pkm_get(&ed->pkm, Gen3FieldPid)) != old.shiny &&
       gen3_edit_set_traits(ed, &old))
        return;
    /* Reached with flipped shininess only for an unknown species: for a real one, every want
     * gen3_traits_of returns is in range (nature < 25, a 1-bit slot, letter < 28, the gender the
     * ratio allows), and the rarest one (an Unown nature and letter, ~1 in 700 PIDs; shiny PIDs
     * are built shiny) is found far inside GEN3_PID_MAX_TRIES, so set_traits doesn't fail. */
    ed_commit(ed);
}

void gen3_edit_set_item(Gen3Editor* ed, uint16_t item) {
    if(item != 0 && !gen3_item_holdable(item)) return;
    gen3_pkm_set(&ed->pkm, Gen3FieldHeldItem, item);
    ed_commit(ed);
}

bool gen3_edit_set_move(Gen3Editor* ed, uint8_t slot, uint16_t move) {
    if(slot >= 4 || move >= GEN3_MOVE_COUNT) return false;
    Gen3Pkm* p = &ed->pkm;
    int others = 0;
    for(int s = 0; s < 4; s++) {
        if(s == slot) continue;
        uint16_t m = gen3_pkm_get(p, Gen3FieldMove1 + s);
        if(m) others++;
        if(move && m == move) return false;
    }
    if(!move && !others) return false;
    ed_clear_move(p, slot);
    if(move) {
        gen3_pkm_set(p, Gen3FieldMove1 + slot, move);
        gen3_pkm_set(p, Gen3FieldPp1 + slot, gen3_move_pp(move));
    }
    ed_pack_moves(p);
    ed_commit(ed);
    return true;
}

void gen3_edit_default_moves(Gen3Editor* ed) {
    ed_set_default_moves(&ed->pkm, gen3_edit_national(ed), ed_level(&ed->pkm));
    ed_commit(ed);
}

void gen3_edit_set_ball(Gen3Editor* ed, uint8_t ball) {
    if(ball < 1 || ball > GEN3_EDIT_BALL_MAX) return;
    gen3_pkm_set(&ed->pkm, Gen3FieldBall, ball);
    ed_commit(ed);
}

void gen3_edit_set_pokerus(Gen3Editor* ed, uint8_t value) {
    gen3_pkm_set(&ed->pkm, Gen3FieldPokerus, value);
    ed_commit(ed);
}

/* want equals the current traits once normalised as gen3_pid_find does: gender ignored for a
 * fixed ratio, the slot for one ability, the letter unless Unown. Never for an unknown species,
 * which gen3_pid_find refuses. */
static bool ed_traits_same(uint16_t national, const Gen3Traits* want, const Gen3Traits* cur) {
    if(!gen3_species(national)) return false;
    if(want->nature != cur->nature || want->shiny != cur->shiny) return false;
    if(!gen3_species_gender_fixed(national) && want->gender != cur->gender) return false;
    if(gen3_species_two_abilities(national) && want->ability_slot != cur->ability_slot)
        return false;
    if(national == GEN3_NATIONAL_UNOWN && want->unown_letter != cur->unown_letter) return false;
    return true;
}

bool gen3_edit_set_traits(Gen3Editor* ed, const Gen3Traits* want) {
    uint16_t national = gen3_edit_national(ed);
    Gen3Traits cur;
    gen3_traits_of(&ed->pkm, &cur);
    /* Nothing to change: keep the PID (and every other byte) */
    if(ed_traits_same(national, want, &cur)) return true;
    uint32_t pid;
    if(!gen3_pid_find(&ed->rng, national, gen3_pkm_get(&ed->pkm, Gen3FieldOtId), want, &pid))
        return false;
    gen3_pkm_set(&ed->pkm, Gen3FieldPid, pid);
    ed_ability_from_pid(&ed->pkm); /* the slot gen3_pid_find satisfied */
    ed_commit(ed);
    return true;
}

const char* gen3_pokerus_status(uint8_t value) {
    if(value == 0) return "Clean";
    if(value & 0x0F) return "Infected";
    return "Cured";
}
