#ifndef SUITE_NEMESIS_H
#define SUITE_NEMESIS_H

#include <stdint.h>

// NemProfile feeds nemesis_gen.
// seed picks bytes, nodes 2..5,
// nphases 2..12, max_* cap
// faults, allow_* gate verbs,
// sut is concord or weld.
typedef struct {
    uint64_t seed;
    int nodes;
    int nphases;
    int max_drop;
    int max_kill;
    int max_restart;
    int allow_submit;
    int allow_stop;
    char sut[16];
} NemProfile;

// Writes a valid suite file.
// True on success.
bool nemesis_gen(const NemProfile *p,
                 const char *outpath);

// Oracle on a file: 0 pass,
// 1 fail, 2 skip.
typedef int (*NemOracle)(
    const char *yamlpath, void *ud);

// Shrinks yamlpath to outpath.
// True keeps fail.
bool nemesis_minimize(
    const char *yamlpath,
    NemOracle oracle, void *ud,
    const char *outpath);

#endif
