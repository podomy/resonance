#ifndef RNG_H
#define RNG_H

#include <stdint.h>

typedef struct {
    uint64_t s[4];
} Rng;

/** Seeds xoshiro256** via splitmix64. */
void rng_seed(Rng* rng, uint64_t seed);

/** Returns the next 64-bit value. */
uint64_t rng_u64(Rng* rng);

#endif
