// TweetNaCl, built without warnings and the undefined-behaviour sanitizer (it shifts negative values),
// for gs_ed25519's signature check. Only verification is used: the random source its key and box
// functions ask for is never called.
#include <stdlib.h>
#define randombytes gs_tweetnacl_randombytes
static void gs_tweetnacl_randombytes(unsigned char *p, unsigned long long n) { (void)p, (void)n, abort(); }
#include "../vendor/tweetnacl/tweetnacl.c"
