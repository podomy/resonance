#ifndef IMG_H
#define IMG_H

#include <stdbool.h>
#include <stddef.h>

// Test image supply for scenarios that need running
// containers. Everything here is local bytes and local
// HTTP: no docker, no base pulls, no network.
//
// Each Concord node runs its own registry on :8444. The
// helpers talk to node i's registry from inside its own
// network namespace (host netns for node 0), so no byte
// crosses the sim mesh and the pump never stalls. The
// filesystem is shared across namespaces, so image files
// stay in one place.
//
// A push tars rootfs_dir as one layer, writes a minimal
// config and manifest, and PUTs all three into the
// node's registry under repo:tag. Queries read the
// registry catalog, tag lists, and manifests back.
// Scenarios push once per node, poll the queries while
// pumping, then submit localhost:8444/<repo>:<tag> and
// let each node pull from itself.

#define IMG_PORT 8444

// img_push tars rootfs_dir and pushes it as repo:tag
// into node i's registry. Returns 1 on success.
bool img_push(int node, const char* repo, const char* tag,
              const char* rootfs);

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
