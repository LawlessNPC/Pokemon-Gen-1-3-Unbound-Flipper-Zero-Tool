#include <src/gen3/gen3_ub_data.h>

#include <stdio.h>
#include <string.h>

#include <src/gen3/gen3_hlink.h>

#define UB_DATA_VERSION 1u
#define UB_DATA_HEAD 12u
#define UB_DATA_TOC_ENTRY 12u
#define UB_DATA_TABLES_MAX 16u
#define UB_DATA_NAME_SIZE 11u

typedef struct {
    uint32_t off;
    uint16_t entries, size;
} UbTable;

static Gen3UbRead ub_read = NULL;
static void* ub_ctx = NULL;
static UbTable ub_name, ub_grow, ub_ndex;

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
    UbTable name = {0}, grow = {0}, ndex = {0};
    uint32_t end = UB_DATA_HEAD + count * UB_DATA_TOC_ENTRY;
    for(uint16_t k = 0; k < count; k++) {
        if(!read(ctx, UB_DATA_HEAD + k * UB_DATA_TOC_ENTRY, b, UB_DATA_TOC_ENTRY)) return false;
        const UbTable t = {.off = ub_le32(b + 4), .entries = ub_le16(b + 8), .size = ub_le16(b + 10)};
        const uint32_t t_end = t.off + (uint32_t)t.entries * t.size;
        if(t_end > end) end = t_end;
        if(memcmp(b, "NAME", 4) == 0) name = t;
        if(memcmp(b, "GROW", 4) == 0) grow = t;
        if(memcmp(b, "NDEX", 4) == 0) ndex = t;
    }
    if(name.size != UB_DATA_NAME_SIZE || grow.size != 1u || ndex.size != 2u || name.entries == 0 ||
       grow.entries != name.entries || ndex.entries != name.entries) {
        return false;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for(uint32_t at = UB_DATA_HEAD; at < end;) {
        const uint32_t n = end - at < sizeof(b) ? end - at : (uint32_t)sizeof(b);
        if(!read(ctx, at, b, n)) return false;
        crc = gen3_crc32_update(crc, b, n);
        at += n;
    }
    if(~crc != crc_want) return false;
    ub_name = name;
    ub_grow = grow;
    ub_ndex = ndex;
    ub_read = read;
    ub_ctx = ctx;
    return true;
}

/* entry species of t into buf, size bytes */
static bool ub_entry(const UbTable* t, uint16_t species, void* buf) {
    return ub_read && species < t->entries &&
           ub_read(ub_ctx, t->off + (uint32_t)species * t->size, buf, t->size);
}

bool gen3_ub_species_name(uint16_t species, char out[GEN3_UB_NAME_LEN + 1u]) {
    char b[UB_DATA_NAME_SIZE];
    if(species > 0 && ub_entry(&ub_name, species, b) && b[0] != 0) {
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
    return ub_entry(&ub_grow, species, &g) && g <= 5u ? (int)g : -1;
}

uint16_t gen3_ub_national(uint16_t species) {
    uint8_t b[2];
    return ub_entry(&ub_ndex, species, b) ? ub_le16(b) : 0u;
}
