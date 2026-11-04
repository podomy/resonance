#ifndef SUITE_PARSE_H
#define SUITE_PARSE_H

#include <stddef.h>

// Node kinds: mapping, sequence, scalar.
typedef enum {
    SUITE_MAP,
    SUITE_LIST,
    SUITE_SCALAR,
} SuiteKind;

typedef struct SuiteNode SuiteNode;

// One mapping entry. Key text owned.
typedef struct {
    char *key;
    size_t key_line;
    SuiteNode *value;
} SuiteEntry;

// Parse tree node. Line is 1-based source.
struct SuiteNode {
    SuiteKind kind;
    size_t line;
    union {
        struct {
            SuiteEntry *items;
            size_t len;
            size_t cap;
        } map;
        struct {
            SuiteNode **items;
            size_t len;
            size_t cap;
        } list;
        struct {
            char *text;
        } scalar;
    } u;
};

// Parsed document. Root is map or list.
typedef struct {
    SuiteNode *root;
} SuiteDoc;

// Parse error. Line and col are 1-based.
typedef struct {
    size_t line;
    size_t col;
    char message[128];
} SuiteError;

// Parses text[0..n). True on success.
bool suite_parse(const char *text, size_t n,
                 SuiteDoc **out, SuiteError *err);

// Frees doc. Safe on nullptr.
void suite_doc_free(SuiteDoc *doc);

// Finds key in map. Nullptr if absent.
SuiteNode *suite_map_find(SuiteNode *node,
                          const char *key);

// List length. Zero if not a list.
size_t suite_list_len(const SuiteNode *node);

// List item. Nullptr if bad index.
SuiteNode *suite_list_at(SuiteNode *node,
                         size_t idx);

// Scalar text. Nullptr if not scalar.
const char *suite_scalar_str(
    const SuiteNode *node);

// Scalar as integer. False if bad.
bool suite_scalar_int(const SuiteNode *node,
                      long long *out);

#endif
