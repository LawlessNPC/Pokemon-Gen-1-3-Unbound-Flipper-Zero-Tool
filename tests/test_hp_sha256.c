#include "test.h"
#include <stdlib.h>
#include <hp_sha256.h>

/* The helper's SHA-256 (hp_sha256.h) against FIPS 180-2's examples */

static void hex(const uint8_t d[32], char out[65]) {
    for(uint32_t i = 0; i < 32u; i++) snprintf(out + 2u * i, 3, "%02x", d[i]);
}

static void sha_of(const uint8_t* p, uint32_t n, char out[65]) {
    HpSha256 s;
    uint8_t d[32];
    hp_sha256_init(&s);
    hp_sha256_update(&s, p, n);
    hp_sha256_final(&s, d);
    hex(d, out);
}

TEST(test_sha256_empty) {
    char h[65];
    sha_of((const uint8_t*)"", 0, h);
    ASSERT_STR_EQ(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(test_sha256_abc) {
    char h[65];
    sha_of((const uint8_t*)"abc", 3, h);
    ASSERT_STR_EQ(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(test_sha256_two_blocks) {
    char h[65];
    const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha_of((const uint8_t*)m, (uint32_t)strlen(m), h);
    ASSERT_STR_EQ(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(test_sha256_million_a_pieces) {
    /* One million 'a', fed in pieces of 1, 63, 64, 65, 1000 ... bytes */
    static const uint32_t sizes[] = {1u, 63u, 64u, 65u, 1000u, 4096u, 7u};
    uint8_t* a = malloc(4096u);
    memset(a, 'a', 4096u);
    HpSha256 s;
    uint8_t d[32];
    char h[65];
    hp_sha256_init(&s);
    uint32_t done = 0;
    for(uint32_t k = 0; done < 1000000u; k++) {
        uint32_t n = sizes[k % 7u];
        if(n > 1000000u - done) n = 1000000u - done;
        hp_sha256_update(&s, a, n);
        done += n;
    }
    hp_sha256_final(&s, d);
    hex(d, h);
    free(a);
    ASSERT_STR_EQ(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

int main(void) {
    RUN(test_sha256_empty);
    RUN(test_sha256_abc);
    RUN(test_sha256_two_blocks);
    RUN(test_sha256_million_a_pieces);
    TEST_EXIT();
}
