#include "rng.h"

/** Xoshiro256** 1.0, splitmix64 seeder. 
 *  This implementation is frozen. Changing values will
 *  break replay. They were carefully picked and should
 *  not be changed. All simulation of randomness must go
 *  through rng_u64 after rng_seed.
 * */

/** Rotates x left by k bits. */
static uint64_t rotl(const uint64_t x, int k) {
  return (x << k) | (x >> (64 - k));
}


/** Advances a splitmix64 state and returns the output. */
static uint64_t splitmix64_next(uint64_t* x) {
    uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/** Seeds xoshiro256** via splitmix64. */
void rng_seed(Rng* rng, uint64_t seed) {
    uint64_t x = seed;
    rng->s[0] = splitmix64_next(&x);
    rng->s[1] = splitmix64_next(&x);
    rng->s[2] = splitmix64_next(&x);
    rng->s[3] = splitmix64_next(&x);
    if (rng->s[0] == 0 && rng->s[1] == 0 &&
        rng->s[2] == 0 && rng->s[3] == 0) {
        rng->s[0] = 1;
    }
}

/** Returns the next 64-bit value. */
uint64_t rng_u64(Rng* rng) {
    const uint64_t result = rotl(rng->s[1] * 5, 7) * 9;
    const uint64_t t = rng->s[1] << 17;

    rng->s[2] ^= rng->s[0];
    rng->s[3] ^= rng->s[1];
    rng->s[1] ^= rng->s[2];
    rng->s[0] ^= rng->s[3];
    rng->s[2] ^= t;
    rng->s[3] = rotl(rng->s[3], 45);

    return result;
}
