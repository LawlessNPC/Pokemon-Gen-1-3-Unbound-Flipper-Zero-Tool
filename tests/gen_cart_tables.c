/* Writes files/gen3_cart_tables.bin for the Cartridge app from the tables compiled into
 * src/gen3/gen3_data_gen.c, which stay the single source of truth. Run: make cart-tables.
 *
 * Layout (version 1, little-endian): a 16-byte header, then the payload, which is the three tables
 * in their in-memory layout.
 *   header:  "G3CT", u16 version 1, u16 species count 387 (index 0 unused), u16 internal ids 412,
 *            u16 abilities 78, u32 CRC-32 (IEEE 802.3) of the payload
 *   payload: gen3_species_info_tab       387 x Gen3SpeciesInfo (16 bytes each, see gen3_data.h)
 *            gen3_internal_to_national_tab 412 x u16
 *            gen3_ability_name_tab        78 x 13 bytes, NUL-padded
 * The reader is gen3_data_tables_open in src/gen3/gen3_data.c. */
#include <stdio.h>
#include <string.h>

#include <src/gen3/gen3_data.h>

_Static_assert(sizeof(Gen3SpeciesInfo) == 16, "Gen3SpeciesInfo layout");

static void put16(unsigned char* p, unsigned v) {
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

int main(int argc, char** argv) {
    if(argc != 2) {
        fprintf(stderr, "usage: gen_cart_tables <out.bin>\n");
        return 2;
    }
    static unsigned char out[16 + sizeof(gen3_species_info_tab) + sizeof(gen3_internal_to_national_tab) +
                             sizeof(gen3_ability_name_tab)];
    unsigned char* p = out + 16;
    for(size_t i = 0; i <= GEN3_NATIONAL_COUNT; i++, p += sizeof(Gen3SpeciesInfo))
        memcpy(p, &gen3_species_info_tab[i], sizeof(Gen3SpeciesInfo));
    for(size_t i = 0; i < GEN3_INTERNAL_COUNT; i++, p += 2) put16(p, gen3_internal_to_national_tab[i]);
    memcpy(p, gen3_ability_name_tab, sizeof(gen3_ability_name_tab));
    p += sizeof(gen3_ability_name_tab);
    size_t payload = (size_t)(p - (out + 16));

    unsigned crc = 0xFFFFFFFFu;
    for(size_t i = 0; i < payload; i++) {
        crc ^= out[16 + i];
        for(int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    crc = ~crc;
    memcpy(out, "G3CT", 4);
    put16(out + 4, 1);
    put16(out + 6, GEN3_NATIONAL_COUNT + 1);
    put16(out + 8, GEN3_INTERNAL_COUNT);
    put16(out + 10, GEN3_ABILITY_COUNT);
    put16(out + 12, crc & 0xFFFF);
    put16(out + 14, crc >> 16);

    FILE* f = fopen(argv[1], "wb");
    if(!f || fwrite(out, 1, sizeof(out), f) != sizeof(out) || fclose(f) != 0) {
        fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    printf("%s: %zu bytes\n", argv[1], sizeof(out));
    return 0;
}
