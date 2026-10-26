#include <src/gen3/gen3_data.h>

#include <string.h>

_Static_assert(sizeof(Gen3SpeciesInfo) == 16, "Gen3SpeciesInfo must stay 16 bytes");

static const char gen3_unknown_name[] = "???";

const Gen3SpeciesInfo* gen3_species(uint16_t national) {
    if(national < 1 || national > GEN3_NATIONAL_COUNT) return NULL;
    return &gen3_species_info_tab[national];
}

bool gen3_species_two_abilities(uint16_t national) {
    const Gen3SpeciesInfo* info = gen3_species(national);
    return info && info->ability[1] != 0;
}

bool gen3_species_gender_fixed(uint16_t national) {
    const Gen3SpeciesInfo* info = gen3_species(national);
    if(!info) return true;
    return info->gender_ratio == GEN3_GENDER_MALE_ONLY ||
           info->gender_ratio == GEN3_GENDER_FEMALE_ONLY || info->gender_ratio == GEN3_GENDERLESS;
}

uint16_t gen3_national_from_internal(uint16_t internal) {
    if(internal >= GEN3_INTERNAL_COUNT) return 0;
    return gen3_internal_to_national_tab[internal];
}

uint16_t gen3_internal_from_national(uint16_t national) {
    const Gen3SpeciesInfo* info = gen3_species(national);
    return info ? info->internal : 0;
}

const char* gen3_move_name(uint16_t move) {
    if(move >= GEN3_MOVE_COUNT) return gen3_unknown_name;
    return gen3_move_name_tab[move];
}

uint8_t gen3_move_pp(uint16_t move) {
    if(move >= GEN3_MOVE_COUNT) return 0;
    return gen3_move_pp_tab[move];
}

uint16_t gen3_move_by_name(size_t i) {
    if(i >= GEN3_MOVE_COUNT - 1) return 0;
    return gen3_move_by_name_tab[i];
}

const char* gen3_item_name(uint16_t item) {
    if(item >= GEN3_ITEM_COUNT || gen3_item_name_tab[item][0] == '\0') return gen3_unknown_name;
    return gen3_item_name_tab[item];
}

bool gen3_item_holdable(uint16_t item) {
    for(size_t i = 0; i < GEN3_HOLDABLE_COUNT; i++) {
        if(gen3_holdable_by_name_tab[i] == item) return true;
    }
    return false;
}

uint16_t gen3_holdable_by_name(size_t i) {
    if(i >= GEN3_HOLDABLE_COUNT) return 0;
    return gen3_holdable_by_name_tab[i];
}

const char* gen3_ability_name(uint8_t ability) {
    if(ability >= GEN3_ABILITY_COUNT) return gen3_unknown_name;
    return gen3_ability_name_tab[ability];
}

const char* gen3_nature_name(uint8_t nature) {
    if(nature >= GEN3_NATURE_COUNT) return gen3_unknown_name;
    return gen3_nature_name_tab[nature];
}

const char* gen3_type_name(uint8_t type) {
    if(type >= GEN3_TYPE_COUNT) return gen3_unknown_name;
    return gen3_type_name_tab[type];
}

/* files/gen3_bulk.bin, written by tools/gen3_tables.py. Little-endian: a 16-byte header, then the
 * payload: species names, learnset starts (in entries), learnset entries. */
#define BULK_NAME_SIZE   (GEN3_SPECIES_NAME_LEN + 1)
#define BULK_NAMES_OFF   16
#define BULK_STARTS_OFF  (BULK_NAMES_OFF + BULK_NAME_SIZE * (GEN3_NATIONAL_COUNT + 1))
#define BULK_ENTRIES_OFF (BULK_STARTS_OFF + 2 * (GEN3_NATIONAL_COUNT + 2))
#define BULK_SIZE        (BULK_ENTRIES_OFF + 2 * GEN3_LEARNSET_COUNT)
#define BULK_CHUNK       64

/* The header up to its CRC: magic "G3BK", u16 version 1, u16 species count 387 (index 0 unused),
 * u32 learnset entries 3947. The u32 CRC-32 of the payload follows. */
static const uint8_t bulk_header[12] = {
    'G',
    '3',
    'B',
    'K',
    1,
    0,
    (GEN3_NATIONAL_COUNT + 1) & 0xFF,
    (GEN3_NATIONAL_COUNT + 1) >> 8,
    GEN3_LEARNSET_COUNT & 0xFF,
    GEN3_LEARNSET_COUNT >> 8,
    0,
    0};

static struct {
    Gen3BulkRead read; /* NULL: not ready */
    void* ctx;
} bulk;

static uint32_t bulk_le16(const uint8_t* p) {
    return p[0] | (uint32_t)p[1] << 8;
}

/* CRC-32 (IEEE 802.3, reflected), bit by bit */
static uint32_t bulk_crc32(uint32_t crc, const uint8_t* p, uint32_t len) {
    for(uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for(int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

bool gen3_bulk_open(Gen3BulkRead read, void* ctx) {
    uint8_t buf[BULK_CHUNK];
    gen3_bulk_close();
    if(!read || !read(ctx, 0, buf, BULK_NAMES_OFF) ||
       memcmp(buf, bulk_header, sizeof(bulk_header)) != 0)
        return false;
    uint32_t want = bulk_le16(buf + 12) | bulk_le16(buf + 14) << 16;
    uint32_t crc = 0xFFFFFFFFu;
    for(uint32_t off = BULK_NAMES_OFF; off < BULK_SIZE; off += BULK_CHUNK) {
        uint32_t len = BULK_SIZE - off < BULK_CHUNK ? BULK_SIZE - off : BULK_CHUNK;
        if(!read(ctx, off, buf, len)) return false;
        crc = bulk_crc32(crc, buf, len);
    }
    /* The CRC covers to the end of the file, so nothing may follow */
    if(~crc != want || read(ctx, BULK_SIZE, buf, 1)) return false;
    bulk.read = read;
    bulk.ctx = ctx;
    return true;
}

void gen3_bulk_close(void) {
    bulk.read = NULL;
    bulk.ctx = NULL;
}

bool gen3_bulk_ready(void) {
    return bulk.read != NULL;
}

/* Reads from the installed source for a species 1..386 */
static bool bulk_get(uint16_t national, uint32_t offset, void* buf, uint32_t len) {
    return bulk.read && national >= 1 && national <= GEN3_NATIONAL_COUNT &&
           bulk.read(bulk.ctx, offset, buf, len);
}

bool gen3_species_name_get(uint16_t national, char out[GEN3_SPECIES_NAME_LEN + 1]) {
    bool ok = bulk_get(national, BULK_NAMES_OFF + BULK_NAME_SIZE * national, out, BULK_NAME_SIZE);
    out[ok ? GEN3_SPECIES_NAME_LEN : 0] = '\0';
    return ok;
}

size_t gen3_learnset_get(uint16_t national, uint16_t* out, size_t max) {
    uint8_t se[4];
    if(!bulk_get(national, BULK_STARTS_OFF + 2 * national, se, sizeof(se))) return 0;
    uint32_t start = bulk_le16(se);
    uint32_t end = bulk_le16(se + 2);
    if(end < start || end > GEN3_LEARNSET_COUNT) return 0;
    size_t count = end - start < max ? end - start : max;
    /* Read the bytes into out, then turn each pair into its entry in place */
    uint8_t* raw = (uint8_t*)out;
    if(count == 0 || !bulk.read(bulk.ctx, BULK_ENTRIES_OFF + 2 * start, raw, 2 * count)) return 0;
    for(size_t i = 0; i < count; i++) out[i] = (uint16_t)bulk_le16(raw + 2 * i);
    return count;
}
