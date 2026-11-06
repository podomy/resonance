#define _GNU_SOURCE
#include "img.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// run executes cmd via system.
static int run(const char* cmd) {
    return (system(cmd) == 0);
}

// fail reports the step when IMG_DEBUG is set and
// returns 0. Silent otherwise: scenarios stay quiet,
// probes set IMG_DEBUG for post-mortems.
static int fail(const char* step) {
    if (getenv("IMG_DEBUG") != nullptr)
        printf("[img] %s failed\n", step);
    return (0);
}

// ns_prefix writes "" for node 0 or "ip netns exec
// resonance_<i> " for the rest into out.
static void ns_prefix(int node, char* out, size_t n) {
    if (node <= 0) {
        out[0] = '\0';
        return;
    }
    snprintf(out, n, "ip netns exec resonance_%d ", node);
}

// ns_prefix_net writes "" for node 0 on the shared
// path, else "ip netns exec <prefix><i> " into out.
// The executor path always isolates, so node 0 gets
// its own namespace there too.
static void ns_prefix_net(const SimNet* net, int node,
                          char* out, size_t n) {
    int m;

    if (net == nullptr) {
        out[0] = '\0';
        return;
    }
    if (node <= 0 && net->isolate_all == 0) {
        out[0] = '\0';
        return;
    }
    m = snprintf(out, n, "ip netns exec %s%d ",
                 net->ns_prefix, node);
    if (m < 0 || m >= (int)n)
        out[0] = '\0';
}

// read_line runs cmd and stores its first line into out.
// Returns 1 on success.
static int read_line(const char* cmd, char* out,
                     size_t n) {
    FILE* fp;
    size_t len;

    fp = popen(cmd, "r");
    if (fp == nullptr)
        return (0);
    if (fgets(out, n, fp) == nullptr) {
        pclose(fp);
        return (0);
    }
    pclose(fp);
    len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' ||
                       out[len - 1] == '\r'))
        out[--len] = '\0';
    return (len > 0);
}

// file_bytes stores the size of path into out.
// Returns 1 on success.
static int file_bytes(const char* path, size_t* out) {
    struct stat st;

    if (stat(path, &st) != 0)
        return (0);
    *out = (size_t)st.st_size;
    return (1);
}

// sha256_file stores the hex digest of path into hex,
// which must hold 65 bytes. The filesystem is shared
// across namespaces, so this always runs on the host.
// Returns 1 on success.
static int sha256_file(const char* path, char* hex) {
    char cmd[512];
    char buf[128];

    snprintf(cmd, sizeof(cmd),
             "sha256sum %s 2>/dev/null", path);
    if (!read_line(cmd, buf, sizeof(buf)))
        return (0);
    if (strlen(buf) < 64)
        return (0);
    memcpy(hex, buf, 64);
    hex[64] = '\0';
    return (1);
}

// tmp_path writes /tmp/resonance-img-<pid>-<name> into
// out. Per-process names keep parallel and repeated
// runs, as root or user, off each other's files.
static void tmp_path(const char* name, char* out,
                     size_t n) {
    snprintf(out, n, "/tmp/resonance-img-%d-%s",
             (int)getpid(), name);
}

// put_blob uploads one blob into a node's registry with
// a monolithic POST carrying ?digest=: the registry answers
// 201 directly, no session Location dance. ns runs curl in
// the node's namespace, empty means the host netns. Returns
// 1 on HTTP 201.
static int put_blob(const char* ns, const char* repo,
                    const char* path,
                    const char* digest) {
    char cmd[2048];
    char code[64];

    tmp_path("code", code, sizeof(code));
    snprintf(cmd, sizeof(cmd),
             "%scurl -s -o /dev/null -w "
             "\"%%{http_code}\" "
             "--max-time 60 -X POST --data-binary @%s "
             "\"http://127.0.0.1:%d/v2/%s/blobs/"
             "uploads/?digest=sha256:%s\" "
             "2>/dev/null | tee %s | "
             "grep -qx 201",
             ns, path, IMG_PORT, repo,
             digest, code);
    if (run(cmd))
        return (1);
    if (getenv("IMG_DEBUG") != nullptr) {
        char out[16];

        if (read_line(code, out,
                      sizeof(out)))
            printf("[img] blob code %s\n",
                   out);
    }
    return (fail("put blob"));
}

// put_manifest uploads manifest_path as repo:tag into
// node i's registry. Retries a few times: the registry
// can answer 405 while it is still indexing a just
// pushed blob. Returns 1 on HTTP 201.
static int put_manifest(const char* ns, const char* repo,
                        const char* tag,
                        const char* manifest_path) {
    char cmd[2048];
    char body[64], hdrs[64], code[64];
    int tries;

    tmp_path("body", body, sizeof(body));
    tmp_path("hdump", hdrs, sizeof(hdrs));
    tmp_path("code", code, sizeof(code));
    snprintf(cmd, sizeof(cmd),
             "%scurl -s -o %s -D "
             "%s -w "
             "\"%%{http_code}\" "
             "--max-time 30 -X PUT -H \"Content-Type: "
             "application/vnd.oci.image.manifest."
             "v1+json\" --data-binary @%s "
             "\"http://127.0.0.1:%d/v2/%s/manifests/%s\" "
             "2>/dev/null | tee %s | "
             "grep -qx 201",
             ns, body, hdrs, manifest_path, IMG_PORT,
             repo, tag, code);
    for (tries = 0; tries < 4; tries++) {
        if (run(cmd))
            return (1);
        if (getenv("IMG_DEBUG") != nullptr) {
            char out[16];
            char txt[256];
            char dump[512];
            char cat[128];

            snprintf(cat, sizeof(cat), "cat %s "
                     "2>/dev/null",
                     code);
            if (read_line(cat, out, sizeof(out)))
                printf("[img] manifest code %s try %d\n",
                       out, tries);
            snprintf(cat, sizeof(cat), "head -c 200 %s "
                     "2>/dev/null",
                     body);
            if (read_line(cat, txt, sizeof(txt)))
                printf("[img] manifest body %s\n", txt);
            snprintf(cat, sizeof(cat), "head -c 400 %s "
                     "2>/dev/null | tr '\n' '|'",
                     hdrs);
            if (read_line(cat, dump, sizeof(dump)))
                printf("[img] manifest hdrs %s\n",
                       dump);
        }
        sleep(2);
    }
    return (fail("put manifest"));
}

// write_text stores s into path. Returns 1 on success.
static int write_text(const char* path, const char* s) {
    FILE* fp;

    fp = fopen(path, "w");
    if (fp == nullptr)
        return (0);
    if (fputs(s, fp) == EOF) {
        fclose(fp);
        return (0);
    }
    return (fclose(fp) == 0);
}

// push_at tars rootfs_dir and pushes it as repo:tag
// into the registry reached through ns (empty runs on
// the host). entry becomes the image Entrypoint.
// Returns 1 on success.
static bool push_at(const char* ns, const char* repo,
                    const char* tag,
                    const char* rootfs,
                    const char* entry) {
    char layer[256], config[256], manifest[256];
    char layerhex[65], confighex[65];
    char configjson[1024], manifestjson[2048];
    size_t layersz, configsz;

    if (repo == nullptr || tag == nullptr ||
        rootfs == nullptr || entry == nullptr)
        return (false);
    if (strchr(repo, '/') != nullptr ||
        strchr(tag, '/') != nullptr)
        return (false);
    snprintf(layer, sizeof(layer),
             "/tmp/resonance-img-%d-%s-%s-layer.tar",
             (int)getpid(), repo, tag);
    snprintf(config, sizeof(config),
             "/tmp/resonance-img-%d-%s-%s-config.json",
             (int)getpid(), repo, tag);
    snprintf(manifest, sizeof(manifest),
             "/tmp/resonance-img-%d-%s-%s-manifest.json",
             (int)getpid(), repo, tag);
    if (system("mkdir -p /tmp 2>/dev/null") != 0)
        return (false);
    {
        char cmd[1024];

        // Explicit file list: archiving "." emits a "./"
        // entry that extractors reject as escaping root.
        snprintf(cmd, sizeof(cmd),
                 "tar -C %s -cf %s $(cd %s && ls -A) "
                 "2>/dev/null",
                 rootfs, layer, rootfs);
        if (!run(cmd))
            return (fail("tar"));
    }
    if (!sha256_file(layer, layerhex))
        return (fail("layer sha"));
    if (!file_bytes(layer, &layersz))
        return (fail("layer size"));
    snprintf(configjson, sizeof(configjson),
             "{\"architecture\":\"amd64\",\"os\":\"linux\","
             "\"config\":{\"Entrypoint\":[\"%s\"],"
             "\"ExposedPorts\":{\"8080/tcp\":{}}},"
             "\"rootfs\":{\"type\":\"layers\",\"diff_ids\":"
             "[\"sha256:%s\"]},\"history\":[{\"created\":"
             "\"2026-01-01T00:00:00Z\",\"comment\":"
             "\"resonance test image\"}]}",
             entry, layerhex);
    if (!write_text(config, configjson))
        return (fail("write config"));
    if (!sha256_file(config, confighex))
        return (fail("config sha"));
    if (!file_bytes(config, &configsz))
        return (fail("config size"));
    snprintf(manifestjson, sizeof(manifestjson),
             "{\"schemaVersion\":2,\"mediaType\":"
             "\"application/vnd.oci.image.manifest."
             "v1+json\",\"config\":{\"mediaType\":"
             "\"application/vnd.oci.image.config."
             "v1+json\",\"digest\":\"sha256:%s\","
             "\"size\":%zu},\"layers\":[{\"mediaType\":"
             "\"application/vnd.oci.image.layer.v1.tar\","
             "\"digest\":\"sha256:%s\",\"size\":%zu}]}",
             confighex, configsz, layerhex, layersz);
    if (!write_text(manifest, manifestjson))
        return (fail("write manifest"));
    if (!put_blob(ns, repo, layer, layerhex))
        return (fail("put layer"));
    if (!put_blob(ns, repo, config, confighex))
        return (fail("put config"));
    if (!put_manifest(ns, repo, tag, manifest))
        return (fail("put manifest"));
    return (1);
}

// img_push tars rootfs_dir and pushes it as repo:tag
// into node i's registry. Returns 1 on success.
// Entrypoint is always /probe: check/ only.
// New code uses img_push_net instead.
bool img_push(int node, const char* repo, const char* tag,
              const char* rootfs) {
    char ns[64];

    if (node < 0 || node > 253)
        return (false);
    ns_prefix(node, ns, sizeof(ns));
    return (push_at(ns, repo, tag, rootfs, "/probe"));
}

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
                  const char* entry) {
    char ns[64];
    size_t i;

    if (net == nullptr || !sim_net_valid(net))
        return (false);
    if (node < 0 || node >= TUN_MAP_MAX)
        return (false);
    if (repo == nullptr || repo[0] == '\0' ||
        tag == nullptr || tag[0] == '\0' ||
        rootfs == nullptr || rootfs[0] == '\0' ||
        entry == nullptr || entry[0] != '/')
        return (false);
    for (i = 0; entry[i] != '\0'; i++) {
        char ch = entry[i];

        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '.' ||
            ch == '/' || ch == '-') {
            continue;
        }
        return (false);
    }
    ns_prefix_net(net, node, ns, sizeof(ns));
    if (ns[0] != '\0' && strncmp(ns, "ip netns exec ",
                                  13) != 0)
        return (false);
    return (push_at(ns, repo, tag, rootfs, entry));
}

// get_body stores the body of path on node i's registry
// into out. extra holds added curl flags. Returns 1 on
// HTTP 200.
static int get_body(int node, const char* path,
                    const char* extra, char* out,
                    size_t n) {
    char ns[64];
    char cmd[1024];
    FILE* fp;
    size_t len;
    size_t r;

    ns_prefix(node, ns, sizeof(ns));
    snprintf(cmd, sizeof(cmd),
             "%scurl -s --max-time 10 %s "
             "\"http://127.0.0.1:%d%s\" 2>/dev/null",
             ns, extra, IMG_PORT, path);
    fp = popen(cmd, "r");
    if (fp == nullptr)
        return (0);
    len = 0;
    while ((r = fread(out + len, 1, n - len - 1,
                      fp)) > 0) {
        len += r;
        if (len >= n - 1)
            break;
    }
    pclose(fp);
    out[len] = '\0';
    return (len > 0);
}

// img_present reports 1 if node i's registry serves the
// manifest for repo:tag. Single shot; no retries.
bool img_present(int node, const char* repo,
                 const char* tag) {
    char ns[64];
    char cmd[1024];

    if (node < 0 || node > 253 || repo == nullptr ||
        tag == nullptr)
        return (false);
    ns_prefix(node, ns, sizeof(ns));
    snprintf(cmd, sizeof(cmd),
             "%scurl -s -o /dev/null -w \"%%{http_code}\" "
             "--max-time 10 -H \"Accept: "
             "application/vnd.oci.image.manifest."
             "v1+json\" \"http://127.0.0.1:%d/v2/%s/"
             "manifests/%s\" 2>/dev/null | grep -qx 200",
             ns, IMG_PORT, repo, tag);
    return (run(cmd));
}

// img_catalog stores node i's repository list into out.
// Returns 1 on success.
bool img_catalog(int node, char* out, size_t n) {
    if (out == nullptr || n == 0)
        return (false);
    return (get_body(node, "/v2/_catalog", "", out, n));
}

// img_tags stores node i's tag list for repo into out.
// Returns 1 on success.
bool img_tags(int node, const char* repo, char* out,
              size_t n) {
    char path[256];

    if (repo == nullptr || out == nullptr || n == 0)
        return (false);
    snprintf(path, sizeof(path), "/v2/%s/tags/list",
             repo);
    return (get_body(node, path, "", out, n));
}
