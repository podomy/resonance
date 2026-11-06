#ifndef IMG_H
#define IMG_H

#include <stddef.h>

#include "sim/sim.h"

static constexpr int IMG_PORT = 8444;

// Test image supply for scenarios that need running
// containers. Everything here is local bytes and local
// HTTP: no docker, no base pulls, no network.
//
// Each SUT node that serves images runs its own OCI
// registry on :8444. The helpers talk to node i's
// registry from inside its own network namespace
// (host netns for node 0), so no byte crosses the
// sim mesh and the pump never stalls. Localhost
// registries are the test pattern: the filesystem
// is shared across namespaces, so image files stay
// in one place.
//
// A push tars rootfs_dir as one layer, writes a minimal
// config and manifest, and PUTs all three into the
// node's registry under repo:tag. Queries read the
// registry catalog, tag lists, and manifests back.
// Scenarios push once per node, poll the queries while
// pumping, then submit localhost:8444/<repo>:<tag> and
// let each node pull from itself.

// img_push tars rootfs_dir and pushes it as repo:tag
// into node i's registry. Returns 1 on success.
// Entrypoint is always /probe: check/ only.
// New code uses img_push_net instead.
bool img_push(int node, const char* repo, const char* tag,
              const char* rootfs);

// img_push_net is img_push for an explicit
// network: node 0 runs in the host netns
// only on the shared path, else in its own
// namespace like every other node. The
// entrypoint runs when the image starts;
// /hog serves the memory-pressure tests.
// Rootfs is read from the shared
// filesystem, so no byte crosses the
// sim mesh. Returns 1 on success.
bool img_push_net(const SimNet* net,
                  int node,
                  const char* repo,
                  const char* tag,
                  const char* rootfs,
                  const char* entry);

// img_present reports 1 if node i's registry serves the
// manifest for repo:tag. Single shot; no retries.
bool img_present(int node, const char* repo,
                 const char* tag);

// img_catalog stores node i's repository list into out.
// Returns 1 on success.
bool img_catalog(int node, char* out, size_t n);

// img_tags stores node i's tag list for repo into out.
// Returns 1 on success.
bool img_tags(int node, const char* repo, char* out,
              size_t n);

#endif
