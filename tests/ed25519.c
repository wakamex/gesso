// gs_ed25519_verify on RFC 8032's test vectors (section 7.1), and on tests/ed25519/cases.txt, where it
// must give ed25519-dalek's verify_strict verdict on every case.
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "gs_ed25519.h"
#include "test.h"

static size_t unhex(const char *h, uint8_t *out, size_t cap) {
    size_t n = 0;
    for (; h[0] && h[1] && h[0] != ' ' && h[0] != '\n' && n < cap; h += 2) {
        char b[3] = { h[0], h[1], 0 };
        out[n++] = (uint8_t)strtoul(b, NULL, 16);
    }
    return n;
}

void test_ed25519(void) {
    static const char *rfc[][3] = {  // public key, message, signature
        { "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
          "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
        { "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
          "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
        { "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
          "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
        { "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
          "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704" },
    };
    for (int i = 0; i < 4; i++) {
        uint8_t pk[32], sig[64], m[64];
        unhex(rfc[i][0], pk, 32), unhex(rfc[i][2], sig, 64);
        size_t n = unhex(rfc[i][1], m, sizeof m);
        CHECK(gs_ed25519_verify(sig, pk, m, n));
        sig[i * 9] ^= 4;  // (a bit changed anywhere: refused)
        CHECK(!gs_ed25519_verify(sig, pk, m, n));
    }

    char *text = SDL_LoadFile("tests/ed25519/cases.txt", NULL);
    CHECK(text);
    int cases = 0, wrong = 0;
    for (char *line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        if (*line == '#' || *line == '\n') continue;
        uint8_t pk[32], sig[64], m[256];
        const char *f = line + 2;
        unhex(f, pk, 32), f = strchr(f, ' ') + 1;
        unhex(f, sig, 64), f = strchr(f, ' ') + 1;
        size_t n = *f == '-' ? 0 : unhex(f, m, sizeof m);
        bool want = line[0] == '1', got = gs_ed25519_verify(sig, pk, m, n);
        cases++;
        if (got != want && wrong++ < 5) fprintf(stderr, "  case %d: %s, ed25519-dalek %s\n", cases, got ? "accepted" : "refused", want ? "accepts" : "refuses");
    }
    CHECK(cases > 1000 && wrong == 0);
    SDL_free(text);
}
