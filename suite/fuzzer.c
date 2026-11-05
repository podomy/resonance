#include "suite/parse.h"

#include <stdio.h>
#include <stdlib.h>

// Reads one file fully then parses
// it. Parse errors are success;
// only crashes or hangs fail.
static bool run_one(const char *path) {
    FILE *fp = fopen(path, "rb");
    long end = 0;
    size_t n = 0;
    char *buf = nullptr;
    SuiteDoc *doc = nullptr;
    SuiteError err = {0, 0, ""};

    if (fp == nullptr) {
        return (false);
    }
    if (fseek(fp, 0, SEEK_END) !=
        0) {
        (void)fclose(fp);
        return (false);
    }
    end = ftell(fp);
    if (end < 0) {
        (void)fclose(fp);
        return (false);
    }
    if (fseek(fp, 0, SEEK_SET) !=
        0) {
        (void)fclose(fp);
        return (false);
    }
    n = (size_t)end;
    buf = malloc(n + 1);
    if (buf == nullptr) {
        (void)fclose(fp);
        return (false);
    }
    if (n > 0 &&
        fread(buf, 1, n, fp) !=
            n) {
        free(buf);
        (void)fclose(fp);
        return (false);
    }
    (void)fclose(fp);
    buf[n] = '\0';
    (void)suite_parse(buf, n, &doc,
                      &err);
    suite_doc_free(doc);
    free(buf);
    return (true);
}

// Fuzz entry: parses each argv file.
// Exit 0 means no crash or hang.
int main(int argc, char **argv) {
    int i = 0;

    if (argc < 2) {
        return (0);
    }
    for (i = 1; i < argc; i++) {
        if (argv[i] == nullptr) {
            return (1);
        }
        if (!run_one(argv[i])) {
            return (1);
        }
    }
    return (0);
}
