#include <src/gen3/gen3_ub_data.h>

#include <stdio.h>
#include <string.h>

#include <src/gen3/gen3_hlink.h>

#define UB_DATA_VERSION 3u
#define UB_DATA_HEAD 12u
#define UB_DATA_TOC_ENTRY 12u
#define UB_DATA_TABLES_MAX 16u
#define UB_DATA_NAME_SIZE 11u
#define UB_MOVE_SIZE 13u
#define UB_ITEM_SIZE 18u
#define UB_ABIL_SIZE 16u

typedef struct {
    uint32_t off;
    uint16_t entries, size;
} UbTable;

/* The tables the reader needs, in the order of ub_tags and ub_sizes */
typedef enum {
    UbName,
    UbGrow,
    UbNdex,
    UbMove,
    UbMvpp,
    UbItem,
    UbItpk,
    UbAbil,
    UbSabl,
    UbBase,
    UbGend,
    UbLvup,
    UbSrts,
    UbSrtm,
    UbSrti,
    UbShip,
    UbTableCount
} UbTableId;

static const char ub_tags[UbTableCount][5] = {
    "NAME", "GROW", "NDEX", "MOVE", "MVPP", "ITEM", "ITPK", "ABIL",
    "SABL", "BASE", "GEND", "LVUP", "SRTS", "SRTM", "SRTI", "SHIP"};
static const uint8_t ub_sizes[UbTableCount] = {
    UB_DATA_NAME_SIZE, 1, 2, UB_MOVE_SIZE, 1, UB_ITEM_SIZE, 1, UB_ABIL_SIZE, 3, 6, 1, 2, 2, 2, 2, 2};

static Gen3UbRead ub_read = NULL;
static void* ub_ctx = NULL;
static UbTable ub_t[UbTableCount];

static uint16_t ub_le16(const uint8_t* p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t ub_le32(const uint8_t* p) {
    return (uint32_t)ub_le16(p) | (uint32_t)ub_le16(p + 2) << 16;
}

void gen3_ub_data_close(void) {
    ub_read = NULL;
    ub_ctx = NULL;
}

bool gen3_ub_data_ok(void) {
    return ub_read != NULL;
}

/* The tables that must have the species count of NAME, and the pairs that must match in count */
static bool ub_shape_ok(const UbTable* t) {
    const uint16_t species = t[UbName].entries;
    return species != 0 && t[UbGrow].entries == species && t[UbNdex].entries == species &&
           t[UbSabl].entries == species && t[UbBase].entries == species &&
           t[UbGend].entries == species && t[UbLvup].entries >= species + 1u &&
           t[UbMove].entries != 0 && t[UbMvpp].entries == t[UbMove].entries &&
           t[UbItem].entries != 0 && t[UbItpk].entries == t[UbItem].entries &&
           t[UbAbil].entries != 0 && t[UbShip].entries == species;
}

bool gen3_ub_data_open(Gen3UbRead read, void* ctx) {
    gen3_ub_data_close();
    uint8_t b[256];
    if(!read(ctx, 0, b, UB_DATA_HEAD) || memcmp(b, "UBND", 4) != 0 ||
       ub_le16(b + 4) != UB_DATA_VERSION) {
        return false;
    }
    const uint16_t count = ub_le16(b + 6);
    const uint32_t crc_want = ub_le32(b + 8);
    if(count == 0 || count > UB_DATA_TABLES_MAX) return false;
    UbTable* const t = ub_t; /* only read once ub_read is set */
    memset(ub_t, 0, sizeof(ub_t));
    bool seen[UbTableCount] = {false};
    uint32_t end = UB_DATA_HEAD + count * UB_DATA_TOC_ENTRY;
    for(uint16_t k = 0; k < count; k++) {
        if(!read(ctx, UB_DATA_HEAD + k * UB_DATA_TOC_ENTRY, b, UB_DATA_TOC_ENTRY)) return false;
        const UbTable e = {.off = ub_le32(b + 4), .entries = ub_le16(b + 8), .size = ub_le16(b + 10)};
        const uint32_t e_end = e.off + (uint32_t)e.entries * e.size;
        if(e_end > end) end = e_end;
        for(uint8_t id = 0; id < UbTableCount; id++) {
            if(memcmp(b, ub_tags[id], 4) == 0) {
                t[id] = e;
                seen[id] = true;
            }
        }
    }
    for(uint8_t id = 0; id < UbTableCount; id++) {
        if(!seen[id] || t[id].size != ub_sizes[id]) return false;
    }
    if(!ub_shape_ok(t)) return false;
    uint32_t crc = 0xFFFFFFFFu;
    for(uint32_t at = UB_DATA_HEAD; at < end;) {
        const uint32_t n = end - at < sizeof(b) ? end - at : (uint32_t)sizeof(b);
        if(!read(ctx, at, b, n)) return false;
        crc = gen3_crc32_update(crc, b, n);
        at += n;
    }
    if(~crc != crc_want) return false;
    ub_read = read;
    ub_ctx = ctx;
    return true;
}

/* entry index of table id into buf, its size bytes */
static bool ub_entry(UbTableId id, uint16_t index, void* buf) {
    const UbTable* t = &ub_t[id];
    return ub_read && index < t->entries &&
           ub_read(ub_ctx, t->off + (uint32_t)index * t->size, buf, t->size);
}

/* The name at index of table id (size bytes, NUL-padded) into out; out is "#<index>" and the
 * result false for an empty name, an unknown index or a read failure. */
static bool ub_name_of(UbTableId id, uint16_t index, char* out) {
    if(index > 0 && ub_entry(id, index, out) && out[0] != 0) {
        out[ub_sizes[id] - 1u] = 0;
        return true;
    }
    snprintf(out, ub_sizes[id], "#%u", (unsigned)index);
    return false;
}

bool gen3_ub_species_name(uint16_t species, char out[GEN3_UB_NAME_LEN + 1u]) {
    char b[UB_DATA_NAME_SIZE];
    if(species > 0 && ub_entry(UbName, species, b) && b[0] != 0) {
        memcpy(out, b, GEN3_UB_NAME_LEN);
        out[GEN3_UB_NAME_LEN] = 0;
        return true;
    }
    snprintf(out, GEN3_UB_NAME_LEN + 1u, "#%u", (unsigned)species);
    return false;
}

int gen3_ub_growth(uint16_t species, void* ctx) {
    (void)ctx;
    uint8_t g = 0xFF;
    return ub_entry(UbGrow, species, &g) && g <= 5u ? (int)g : -1;
}

uint16_t gen3_ub_national(uint16_t species) {
    uint8_t b[2];
    return ub_entry(UbNdex, species, b) ? ub_le16(b) : 0u;
}

uint16_t gen3_ub_shipped_picture(uint16_t species) {
    uint8_t b[2];
    return ub_entry(UbShip, species, b) ? ub_le16(b) : 0u;
}

bool gen3_ub_move_name(uint16_t move, char out[13]) {
    return ub_name_of(UbMove, move, out);
}

uint8_t gen3_ub_move_pp(uint16_t move) {
    uint8_t pp = 0;
    return ub_entry(UbMvpp, move, &pp) ? pp : 0u;
}

bool gen3_ub_item_name(uint16_t item, char out[18]) {
    return ub_name_of(UbItem, item, out);
}

bool gen3_ub_item_holdable(uint16_t item) {
    uint8_t pocket = 0;
    return ub_entry(UbItpk, item, &pocket) && pocket >= 1u && pocket <= 3u;
}

bool gen3_ub_ability_name(uint8_t ability, char out[16]) {
    return ub_name_of(UbAbil, ability, out);
}

bool gen3_ub_species_abilities(uint16_t species, uint8_t out[3]) {
    return ub_entry(UbSabl, species, out);
}

bool gen3_ub_base_stats(uint16_t species, uint8_t out[6]) {
    return ub_entry(UbBase, species, out);
}

int gen3_ub_gender_threshold(uint16_t species) {
    uint8_t g = 0;
    return ub_entry(UbGend, species, &g) ? (int)g : -1;
}

uint8_t gen3_ub_levelup(uint16_t species, uint16_t* out, uint8_t max) {
    const UbTable* t = &ub_t[UbLvup];
    uint8_t b[4];
    if(!ub_read || species >= ub_t[UbName].entries || !ub_read(ub_ctx, t->off + (uint32_t)species * 2u, b, 4u)) {
        return 0;
    }
    const uint32_t start = ub_le16(b), end = ub_le16(b + 2);
    const uint32_t moves = t->entries - (uint32_t)ub_t[UbName].entries - 1u;
    if(start > end || end > moves) return 0;
    const uint32_t first = t->off + ((uint32_t)ub_t[UbName].entries + 1u) * 2u + start * 2u;
    uint8_t n = 0;
    while(n < max && start + n < end) {
        if(!ub_read(ub_ctx, first + n * 2u, b, 2u)) return 0;
        out[n++] = ub_le16(b);
    }
    return n;
}

static UbTableId ub_sort_table(Gen3UbSort s, bool* ok) {
    *ok = true;
    switch(s) {
    case Gen3UbSortSpecies:
        return UbSrts;
    case Gen3UbSortMoves:
        return UbSrtm;
    case Gen3UbSortItems:
        return UbSrti;
    }
    *ok = false;
    return UbSrts;
}

uint16_t gen3_ub_sorted_count(Gen3UbSort s) {
    bool ok;
    const UbTableId id = ub_sort_table(s, &ok);
    return ok && ub_read ? ub_t[id].entries : 0u;
}

uint16_t gen3_ub_sorted(Gen3UbSort s, uint16_t index) {
    bool ok;
    const UbTableId id = ub_sort_table(s, &ok);
    uint8_t b[2];
    return ok && ub_entry(id, index, b) ? ub_le16(b) : 0u;
}
