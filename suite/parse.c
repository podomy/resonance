#include "suite/parse.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// First child slot count.
static constexpr int kSlots = 4;
// Slot growth factor.
static constexpr int kGrow = 2;
// Max block nesting depth.
static constexpr int kMaxDepth = 64;
// Max raw line length in bytes.
static constexpr int kMaxLine = 8192;

// One significant source line.
typedef struct {
    const char *body;
    size_t len;
    size_t line;
    size_t indent;
} Line;

// Cursor over significant lines.
typedef struct {
    Line *lines;
    size_t nlines;
    size_t pos;
    SuiteError *err;
    bool failed;
    size_t depth;
} Parser;

// Writes error slot. Ignores nullptr.
static void set_err(SuiteError *err, size_t line,
                    size_t col, const char *msg) {
    if (err == nullptr) {
        return;
    }
    err->line = line;
    err->col = col;
    (void)snprintf(err->message,
                   sizeof(err->message), "%s",
                   msg);
}

// Records first error only.
static void fail(Parser *p, size_t line,
                 size_t col, const char *msg) {
    if (p->failed) {
        return;
    }
    p->failed = true;
    set_err(p->err, line, col, msg);
}

// Makes node. Nullptr on OOM.
static SuiteNode *mknode(SuiteKind kind,
                         size_t line) {
    SuiteNode *nd = malloc(sizeof(*nd));
    if (nd == nullptr) {
        return (nullptr);
    }
    memset(nd, 0, sizeof(*nd));
    nd->kind = kind;
    nd->line = line;
    return (nd);
}

// Frees tree. Safe on nullptr.
static void node_free(SuiteNode *nd) {
    if (nd == nullptr) {
        return;
    }
    if (nd->kind == SUITE_MAP) {
        for (size_t i = 0; i < nd->u.map.len;
             i++) {
            free(nd->u.map.items[i].key);
            node_free(nd->u.map.items[i].value);
        }
        free(nd->u.map.items);
    } else if (nd->kind == SUITE_LIST) {
        for (size_t i = 0; i < nd->u.list.len;
             i++) {
            node_free(nd->u.list.items[i]);
        }
        free(nd->u.list.items);
    } else {
        free(nd->u.scalar.text);
    }
    free(nd);
}

// Grows map slots. False on OOM.
static bool map_grow(SuiteNode *map) {
    size_t want = map->u.map.cap == 0
                      ? (size_t)kSlots
                      : map->u.map.cap *
                            (size_t)kGrow;
    SuiteEntry *items;

    if (want < map->u.map.cap) {
        return (false);
    }
    if (want >
        SIZE_MAX / sizeof(*items)) {
        return (false);
    }
    items = realloc(map->u.map.items,
                    want * sizeof(*items));
    if (items == nullptr) {
        return (false);
    }
    map->u.map.items = items;
    map->u.map.cap = want;
    return (true);
}

// Grows list slots. False on OOM.
static bool list_grow(SuiteNode *list) {
    size_t want = list->u.list.cap == 0
                      ? (size_t)kSlots
                      : list->u.list.cap *
                            (size_t)kGrow;
    SuiteNode **items;

    if (want < list->u.list.cap) {
        return (false);
    }
    if (want >
        SIZE_MAX / sizeof(*items)) {
        return (false);
    }
    items = realloc(list->u.list.items,
                    want * sizeof(*items));
    if (items == nullptr) {
        return (false);
    }
    list->u.list.items = items;
    list->u.list.cap = want;
    return (true);
}

// Appends entry. Takes key on success.
static bool map_put(SuiteNode *map, char *key,
                    size_t kline, SuiteNode *val) {
    if (map->u.map.len == map->u.map.cap &&
        !map_grow(map)) {
        return (false);
    }
    SuiteEntry *e =
        &map->u.map.items[map->u.map.len];
    e->key = key;
    e->key_line = kline;
    e->value = val;
    map->u.map.len++;
    return (true);
}

// Appends item. Takes node on success.
static bool list_push(SuiteNode *list,
                      SuiteNode *item) {
    if (list->u.list.len == list->u.list.cap &&
        !list_grow(list)) {
        return (false);
    }
    list->u.list.items[list->u.list.len] = item;
    list->u.list.len++;
    return (true);
}

// True if key already in map.
static bool map_has(SuiteNode *map,
                    const char *key) {
    for (size_t i = 0; i < map->u.map.len;
         i++) {
        if (strcmp(map->u.map.items[i].key,
                   key) == 0) {
            return (true);
        }
    }
    return (false);
}

// Finds closing quote past i. False if none.
static bool skip_quoted(const char *s, size_t n,
                        size_t i, size_t *endp) {
    char q = s[i];
    size_t j = i + 1;
    if (q == '"') {
        while (j < n) {
            if (s[j] == '\\') {
                j += 2;
                continue;
            }
            if (s[j] == '"') {
                *endp = j + 1;
                return (true);
            }
            j++;
        }
        return (false);
    }
    while (j < n) {
        if (s[j] != '\'') {
            j++;
            continue;
        }
        if (j + 1 < n && s[j + 1] == '\'') {
            j += 2;
            continue;
        }
        *endp = j + 1;
        return (true);
    }
    return (false);
}

// Decodes quoted token at s[0]. No tail check.
static char *quoted_body(Parser *p, const char *s,
                         size_t n, size_t line,
                         size_t col, size_t *endp) {
    char *buf;
    size_t w = 0;
    size_t j = 1;

    if (n == 0) {
        fail(p, line, col,
             "unterminated quote");
        return (nullptr);
    }
    buf = malloc(n + 1);
    if (buf == nullptr) {
        fail(p, line, col, "no memory");
        return (nullptr);
    }
    if (s[0] == '"') {
        while (j < n && s[j] != '"') {
            if (s[j] != '\\') {
                buf[w++] = s[j++];
                continue;
            }
            if (j + 1 >= n) {
                break;
            }
            char v = 0;
            bool known = true;
            switch (s[j + 1]) {
            case 'n':
                v = '\n';
                break;
            case 't':
                v = '\t';
                break;
            case 'r':
                v = '\r';
                break;
            case '\\':
                v = '\\';
                break;
            case '"':
                v = '"';
                break;
            case '\'':
                v = '\'';
                break;
            default:
                known = false;
                break;
            }
            if (!known) {
                fail(p, line, col + j,
                     "unknown escape");
                free(buf);
                return (nullptr);
            }
            buf[w++] = v;
            j += 2;
        }
        if (j >= n) {
            fail(p, line, col,
                 "unterminated quote");
            free(buf);
            return (nullptr);
        }
        *endp = j + 1;
    } else {
        while (j < n) {
            if (s[j] != '\'') {
                buf[w++] = s[j++];
                continue;
            }
            if (j + 1 < n &&
                s[j + 1] == '\'') {
                buf[w++] = '\'';
                j += 2;
                continue;
            }
            break;
        }
        if (j >= n) {
            fail(p, line, col,
                 "unterminated quote");
            free(buf);
            return (nullptr);
        }
        *endp = j + 1;
    }
    buf[w] = '\0';
    return (buf);
}

// Decodes quoted scalar. Checks tail text.
static char *unquote(Parser *p, const char *s,
                     size_t n, size_t line,
                     size_t col, size_t *endp) {
    char *text =
        quoted_body(p, s, n, line, col, endp);
    if (text == nullptr) {
        return (nullptr);
    }
    for (size_t k = *endp; k < n; k++) {
        if (s[k] != ' ') {
            fail(p, line, col + k,
                 "trailing text");
            free(text);
            return (nullptr);
        }
    }
    return (text);
}

// Copies plain scalar after validation.
static char *plain(Parser *p, const char *s,
                   size_t n, size_t line,
                   size_t col) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '{' || c == '}' ||
            c == '[' || c == ']') {
            fail(p, line, col + i,
                 "flow style not allowed");
            return (nullptr);
        }
        if (c == '&') {
            fail(p, line, col + i,
                 "anchor not allowed");
            return (nullptr);
        }
        if (c == '*') {
            fail(p, line, col + i,
                 "alias not allowed");
            return (nullptr);
        }
        if (c == '!') {
            fail(p, line, col + i,
                 "tag not allowed");
            return (nullptr);
        }
        if (c == '@' || c == '`') {
            fail(p, line, col + i,
                 "reserved character");
            return (nullptr);
        }
        if (c == ':' &&
            (i + 1 == n || s[i + 1] == ' ' ||
             s[i + 1] == '\t')) {
            fail(p, line, col + i,
                 "unexpected ':'");
            return (nullptr);
        }
    }
    char *buf = malloc(n + 1);
    if (buf == nullptr) {
        fail(p, line, col, "no memory");
        return (nullptr);
    }
    memcpy(buf, s, n);
    buf[n] = '\0';
    return (buf);
}

// Makes scalar node from inline text.
static SuiteNode *parse_scalar(Parser *p,
                               const char *s,
                               size_t n, size_t line,
                               size_t col) {
    if (n == 0) {
        fail(p, line, col, "missing value");
        return (nullptr);
    }
    char *text = nullptr;
    if (s[0] == '"' || s[0] == '\'') {
        size_t end = 0;
        text = unquote(p, s, n, line, col,
                       &end);
    } else {
        text = plain(p, s, n, line, col);
    }
    if (text == nullptr) {
        return (nullptr);
    }
    SuiteNode *nd = mknode(SUITE_SCALAR, line);
    if (nd == nullptr) {
        fail(p, line, col, "no memory");
        free(text);
        return (nullptr);
    }
    nd->u.scalar.text = text;
    return (nd);
}

// Finds key colon. Spaces only;
// scrub rejects tabs first.
static size_t key_end(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] != ':') {
            continue;
        }
        if (i + 1 == n) {
            return (i);
        }
        if (s[i + 1] == ' ') {
            return (i);
        }
    }
    return (SIZE_MAX);
}

// True if s starts a "- " item.
// Spaces only; scrub kills tabs.
static bool is_dash(const char *s, size_t n) {
    if (n == 0 || s[0] != '-') {
        return (false);
    }
    if (n == 1) {
        return (true);
    }
    return (s[1] == ' ');
}

// True if s is "[]" with spaces only.
static bool is_empty_list(const char *s,
                          size_t n) {
    size_t j = 1;

    if (n == 0 || s[0] != '[') {
        return (false);
    }
    while (j < n && s[j] == ' ') {
        j++;
    }
    if (j >= n || s[j] != ']') {
        return (false);
    }
    j++;
    while (j < n && s[j] == ' ') {
        j++;
    }
    return (j == n);
}

// Parses block at indent.
static SuiteNode *parse_block(Parser *p,
                              size_t indent);

// Parses value after colon at c.
static SuiteNode *parse_value(Parser *p, Line ln,
                              size_t c, size_t indent,
                              bool same) {
    size_t i = c;
    while (i < ln.len && ln.body[i] == ' ') {
        i++;
    }
    if (i == ln.len) {
        if (p->pos < p->nlines) {
            Line nx = p->lines[p->pos];
            if (nx.indent > indent ||
                (same && nx.indent == indent &&
                 is_dash(nx.body, nx.len))) {
                return (parse_block(p, nx.indent));
            }
        }
        fail(p, ln.line,
             ln.indent + ln.len + 1,
             "missing value");
        return (nullptr);
    }
    // Empty list literal feeds the
    // semantic non-empty errors.
    if (is_empty_list(ln.body + i,
                      ln.len - i)) {
        SuiteNode *nd;

        nd = mknode(SUITE_LIST,
                    ln.line);
        if (nd == nullptr) {
            fail(p, ln.line,
                 ln.indent + i + 1,
                 "no memory");
            return (nullptr);
        }
        return (nd);
    }
    size_t col = ln.indent + i + 1;
    char f = ln.body[i];
    if (f == '|' || f == '>') {
        fail(p, ln.line, col,
             "block scalar not allowed");
        return (nullptr);
    }
    if (f == '%') {
        fail(p, ln.line, col,
             "directive not allowed");
        return (nullptr);
    }
    if (f == '?' &&
        (i + 1 == ln.len ||
         ln.body[i + 1] == ' ')) {
        fail(p, ln.line, col, "unexpected '?'");
        return (nullptr);
    }
    if (f == '-' &&
        (i + 1 == ln.len ||
         ln.body[i + 1] == ' ')) {
        fail(p, ln.line, col, "unexpected '-'");
        return (nullptr);
    }
    if (f == '@' || f == '`') {
        fail(p, ln.line, col,
             "reserved character");
        return (nullptr);
    }
    return (parse_scalar(p, ln.body + i,
                         ln.len - i, ln.line,
                         col));
}

// Parses one entry into map.
static bool parse_entry(Parser *p, SuiteNode *map,
                        size_t indent) {
    Line ln = p->lines[p->pos];
    p->pos++;
    // Lone ? matches the value-side
    // message, not expected ':'.
    if (ln.body[0] == '?' &&
        (ln.len == 1 ||
         ln.body[1] == ' ')) {
        fail(p, ln.line, ln.indent + 1,
             "unexpected '?'");
        return (false);
    }
    char *key = nullptr;
    size_t c = 0;
    if (ln.body[0] == '"' ||
        ln.body[0] == '\'') {
        size_t end = 0;
        key = quoted_body(p, ln.body, ln.len,
                          ln.line,
                          ln.indent + 1, &end);
        if (key == nullptr) {
            return (false);
        }
        if (key[0] == '\0') {
            fail(p, ln.line, ln.indent + 1,
                 "empty key");
            free(key);
            return (false);
        }
        size_t k = end;
        while (k < ln.len && ln.body[k] == ' ') {
            k++;
        }
        if (k == ln.len || ln.body[k] != ':') {
            fail(p, ln.line,
                 ln.indent + k + 1,
                 "expected ':'");
            free(key);
            return (false);
        }
        c = k + 1;
    } else {
        size_t ke = key_end(ln.body, ln.len);
        if (ke == SIZE_MAX) {
            fail(p, ln.line,
                 ln.indent + ln.len + 1,
                 "expected ':'");
            return (false);
        }
        size_t kl = ke;
        while (kl > 0 && ln.body[kl - 1] == ' ') {
            kl--;
        }
        if (kl == 0) {
            fail(p, ln.line, ln.indent + 1,
                 "empty key");
            return (false);
        }
        key = plain(p, ln.body, kl, ln.line,
                    ln.indent + 1);
        if (key == nullptr) {
            return (false);
        }
        c = ke + 1;
    }
    if (map_has(map, key)) {
        fail(p, ln.line, ln.indent + 1,
             "duplicate key");
        free(key);
        return (false);
    }
    SuiteNode *val =
        parse_value(p, ln, c, indent, true);
    if (val == nullptr) {
        free(key);
        return (false);
    }
    if (!map_put(map, key, ln.line, val)) {
        fail(p, ln.line, ln.indent + 1,
             "no memory");
        free(key);
        node_free(val);
        return (false);
    }
    return (true);
}

// Parses entries at indent into map.
static bool parse_map_body(Parser *p, SuiteNode *map,
                           size_t indent) {
    while (p->pos < p->nlines &&
           p->lines[p->pos].indent == indent) {
        Line ln = p->lines[p->pos];
        if (is_dash(ln.body, ln.len)) {
            fail(p, ln.line, indent + 1,
                 "unexpected '-'");
            return (false);
        }
        if (!parse_entry(p, map, indent)) {
            return (false);
        }
    }
    if (p->pos < p->nlines &&
        p->lines[p->pos].indent > indent) {
        Line ln = p->lines[p->pos];
        fail(p, ln.line, ln.indent + 1,
             "unexpected indentation");
        return (false);
    }
    return (true);
}

// Parses map block at indent.
static SuiteNode *parse_map(Parser *p,
                            size_t indent) {
    SuiteNode *map =
        mknode(SUITE_MAP,
               p->lines[p->pos].line);
    if (map == nullptr) {
        fail(p, p->lines[p->pos].line,
             indent + 1, "no memory");
        return (nullptr);
    }
    if (!parse_map_body(p, map, indent)) {
        node_free(map);
        return (nullptr);
    }
    return (map);
}

// Parses "- key: val" with continuations.
static SuiteNode *parse_inline(Parser *p, Line ln,
                               size_t i,
                               size_t indent) {
    SuiteNode *map = mknode(SUITE_MAP, ln.line);
    if (map == nullptr) {
        fail(p, ln.line, indent + 1,
             "no memory");
        return (nullptr);
    }
    size_t col = ln.indent + i + 1;
    char *key = nullptr;
    size_t after = 0;
    if (ln.body[i] == '"' ||
        ln.body[i] == '\'') {
        size_t end = 0;
        size_t k;

        key = quoted_body(p, ln.body + i,
                          ln.len - i,
                          ln.line, col,
                          &end);
        if (key == nullptr) {
            node_free(map);
            return (nullptr);
        }
        if (key[0] == '\0') {
            fail(p, ln.line, col,
                 "empty key");
            free(key);
            node_free(map);
            return (nullptr);
        }
        k = i + end;
        while (k < ln.len &&
               ln.body[k] == ' ') {
            k++;
        }
        if (k == ln.len ||
            ln.body[k] != ':') {
            fail(p, ln.line,
                 ln.indent + k + 1,
                 "expected ':'");
            free(key);
            node_free(map);
            return (nullptr);
        }
        after = k + 1;
    } else {
        size_t ke =
            key_end(ln.body + i,
                    ln.len - i);
        size_t kl;

        if (ke == SIZE_MAX) {
            fail(p, ln.line, col,
                 "expected ':'");
            node_free(map);
            return (nullptr);
        }
        kl = ke;
        while (kl > 0 &&
               ln.body[i + kl - 1] ==
                   ' ') {
            kl--;
        }
        if (kl == 0) {
            fail(p, ln.line, col,
                 "empty key");
            node_free(map);
            return (nullptr);
        }
        key = plain(p, ln.body + i, kl,
                    ln.line, col);
        if (key == nullptr) {
            node_free(map);
            return (nullptr);
        }
        after = i + ke + 1;
    }
    SuiteNode *val =
        parse_value(p, ln, after, indent,
                    false);
    if (val == nullptr) {
        free(key);
        node_free(map);
        return (nullptr);
    }
    if (!map_put(map, key, ln.line, val)) {
        fail(p, ln.line, col, "no memory");
        free(key);
        node_free(val);
        node_free(map);
        return (nullptr);
    }
    if (p->pos < p->nlines &&
        p->lines[p->pos].indent > indent) {
        size_t sub = p->lines[p->pos].indent;
        if (!parse_map_body(p, map, sub)) {
            node_free(map);
            return (nullptr);
        }
    }
    return (map);
}

// Parses one "- ..." item.
static SuiteNode *parse_item(Parser *p,
                             size_t indent) {
    Line ln = p->lines[p->pos];
    p->pos++;
    size_t i = 1;
    while (i < ln.len && ln.body[i] == ' ') {
        i++;
    }
    if (i == ln.len) {
        if (p->pos < p->nlines &&
            p->lines[p->pos].indent > indent) {
            size_t sub = p->lines[p->pos].indent;
            return (parse_block(p, sub));
        }
        fail(p, ln.line,
             ln.indent + ln.len + 1,
             "missing value");
        return (nullptr);
    }
    const char *s = ln.body + i;
    size_t n = ln.len - i;
    size_t col = ln.indent + i + 1;
    // Nested "[]" parses as an empty
    // list so lint owns the message.
    if (is_empty_list(s, n)) {
        SuiteNode *nd =
            mknode(SUITE_LIST,
                   ln.line);

        if (nd == nullptr) {
            fail(p, ln.line, col,
                 "no memory");
            return (nullptr);
        }
        return (nd);
    }
    if (s[0] == '"' || s[0] == '\'') {
        size_t close = 0;
        size_t k;

        if (skip_quoted(s, n, 0,
                        &close)) {
            k = close;
            while (k < n && s[k] == ' ') {
                k++;
            }
            if (k < n && s[k] == ':') {
                return (parse_inline(
                    p, ln, i, indent));
            }
        }
    } else if (key_end(s, n) !=
               SIZE_MAX) {
        return (parse_inline(p, ln, i,
                             indent));
    }
    return (parse_scalar(p, s, n, ln.line, col));
}

// Parses list block at indent.
static SuiteNode *parse_list(Parser *p,
                             size_t indent) {
    SuiteNode *list =
        mknode(SUITE_LIST,
               p->lines[p->pos].line);
    if (list == nullptr) {
        fail(p, p->lines[p->pos].line,
             indent + 1, "no memory");
        return (nullptr);
    }
    while (p->pos < p->nlines &&
           p->lines[p->pos].indent == indent &&
           is_dash(p->lines[p->pos].body,
                   p->lines[p->pos].len)) {
        SuiteNode *it = parse_item(p, indent);
        if (it == nullptr) {
            node_free(list);
            return (nullptr);
        }
        if (!list_push(list, it)) {
            fail(p, it->line, indent + 1,
                 "no memory");
            node_free(it);
            node_free(list);
            return (nullptr);
        }
    }
    if (p->pos < p->nlines &&
        p->lines[p->pos].indent > indent) {
        Line ln = p->lines[p->pos];
        fail(p, ln.line, ln.indent + 1,
             "unexpected indentation");
        node_free(list);
        return (nullptr);
    }
    return (list);
}

// Parses block at indent. Caps depth.
static SuiteNode *parse_block(Parser *p,
                              size_t indent) {
    Line ln = p->lines[p->pos];
    SuiteNode *nd;

    if (p->depth >= (size_t)kMaxDepth) {
        fail(p, ln.line, ln.indent + 1,
             "too deep");
        return (nullptr);
    }
    p->depth++;
    if (is_dash(ln.body, ln.len)) {
        nd = parse_list(p, indent);
    } else {
        nd = parse_map(p, indent);
    }
    p->depth--;
    return (nd);
}

// Strips comments, rejects tabs and controls.
static bool scrub_line(Parser *p, char *body,
                       size_t *lenp, size_t lineno,
                       size_t indent) {
    size_t len = *lenp;
    size_t i = 0;
    while (i < len) {
        char c = body[i];
        if (c == '"' || c == '\'') {
            size_t close = 0;
            if (!skip_quoted(body, len, i,
                             &close)) {
                fail(p, lineno, indent + i + 1,
                     "unterminated quote");
                return (false);
            }
            i = close;
            continue;
        }
        if (c == '#' &&
            (i == 0 || body[i - 1] == ' ' ||
             body[i - 1] == '\t')) {
            len = i;
            break;
        }
        if (c == '\t') {
            fail(p, lineno, indent + i + 1,
                 "tab not allowed");
            return (false);
        }
        if ((unsigned char)c < 0x20) {
            fail(p, lineno, indent + i + 1,
                 "bad character");
            return (false);
        }
        i++;
    }
    while (len > 0 &&
           (body[len - 1] == ' ' ||
            body[len - 1] == '\t')) {
        len--;
    }
    body[len] = '\0';
    *lenp = len;
    return (true);
}

// Appends raw line if significant.
static bool take_line(Parser *p, char *raw,
                      size_t lineno) {
    size_t len = strlen(raw);
    if (len > (size_t)kMaxLine) {
        fail(p, lineno, 1,
             "line too long");
        return (false);
    }
    if (len > 0 && raw[len - 1] == '\r') {
        len--;
        raw[len] = '\0';
    }
    char *cr = memchr(raw, '\r', len);
    if (cr != nullptr) {
        fail(p, lineno,
             (size_t)(cr - raw) + 1,
             "bad carriage return");
        return (false);
    }
    size_t indent = 0;
    while (raw[indent] == ' ') {
        indent++;
    }
    if (raw[indent] == '\t') {
        fail(p, lineno, indent + 1,
             "tab in indentation");
        return (false);
    }
    char *body = raw + indent;
    size_t blen = len - indent;
    bool blank = true;
    for (size_t i = 0; i < blen; i++) {
        if (body[i] != ' ' && body[i] != '\t') {
            blank = false;
            break;
        }
    }
    if (blank) {
        return (true);
    }
    if (body[0] == '#') {
        return (true);
    }
    if (!scrub_line(p, body, &blen, lineno,
                    indent)) {
        return (false);
    }
    if (blen == 0) {
        return (true);
    }
    if (body[0] == '%') {
        fail(p, lineno, indent + 1,
             "directive not allowed");
        return (false);
    }
    // Cap fits one slot per raw line.
    Line *ln = &p->lines[p->nlines];
    ln->body = body;
    ln->len = blen;
    ln->line = lineno;
    ln->indent = indent;
    p->nlines++;
    return (true);
}

// Parses text[0..n). True on success.
bool suite_parse(const char *text, size_t n,
                 SuiteDoc **out, SuiteError *err) {
    if (out == nullptr) {
        return (false);
    }
    *out = nullptr;
    set_err(err, 0, 0, "");
    if (n > 0 && text == nullptr) {
        set_err(err, 0, 0, "bad input");
        return (false);
    }
    if (text == nullptr) {
        text = "";
    }
    for (size_t i = 0; i < n; i++) {
        if (text[i] != '\0') {
            continue;
        }
        size_t line = 1;
        for (size_t j = 0; j < i; j++) {
            if (text[j] == '\n') {
                line++;
            }
        }
        size_t k = i;
        while (k > 0 && text[k - 1] != '\n') {
            k--;
        }
        set_err(err, line, i - k + 1,
                "NUL byte");
        return (false);
    }
    char *copy = malloc(n + 1);
    if (copy == nullptr) {
        set_err(err, 0, 0, "no memory");
        return (false);
    }
    if (n > 0) {
        memcpy(copy, text, n);
    }
    copy[n] = '\0';
    size_t max = 1;
    for (size_t i = 0; i < n; i++) {
        if (copy[i] == '\n') {
            max++;
        }
    }
    Line *lines = malloc(max * sizeof(*lines));
    if (lines == nullptr) {
        set_err(err, 0, 0, "no memory");
        free(copy);
        return (false);
    }
    Parser p = {.lines = lines,
                .nlines = 0,
                .pos = 0,
                .err = err,
                .failed = false};
    char *cur = copy;
    size_t lineno = 0;
    while (cur != nullptr) {
        lineno++;
        char *nl = strchr(cur, '\n');
        char *next = nullptr;
        if (nl != nullptr) {
            *nl = '\0';
            next = nl + 1;
        }
        if (!take_line(&p, cur, lineno)) {
            break;
        }
        cur = next;
    }
    if (p.failed) {
        free(lines);
        free(copy);
        return (false);
    }
    SuiteNode *root = nullptr;
    if (p.nlines == 0) {
        root = mknode(SUITE_MAP, 1);
        if (root == nullptr) {
            set_err(err, 0, 0, "no memory");
            free(lines);
            free(copy);
            return (false);
        }
    } else {
        root = parse_block(&p, lines[0].indent);
        if (root == nullptr) {
            free(lines);
            free(copy);
            return (false);
        }
        if (p.pos != p.nlines) {
            set_err(err, lines[p.pos].line,
                    lines[p.pos].indent + 1,
                    "inconsistent indentation");
            node_free(root);
            free(lines);
            free(copy);
            return (false);
        }
    }
    SuiteDoc *doc = malloc(sizeof(*doc));
    if (doc == nullptr) {
        set_err(err, 0, 0, "no memory");
        node_free(root);
        free(lines);
        free(copy);
        return (false);
    }
    doc->root = root;
    *out = doc;
    free(lines);
    free(copy);
    return (true);
}

// Frees doc. Safe on nullptr.
void suite_doc_free(SuiteDoc *doc) {
    if (doc == nullptr) {
        return;
    }
    node_free(doc->root);
    free(doc);
}

// Finds key in map. Nullptr if absent.
SuiteNode *suite_map_find(SuiteNode *node,
                          const char *key) {
    if (node == nullptr || key == nullptr) {
        return (nullptr);
    }
    if (node->kind != SUITE_MAP) {
        return (nullptr);
    }
    for (size_t i = 0; i < node->u.map.len;
         i++) {
        if (strcmp(node->u.map.items[i].key,
                   key) == 0) {
            return (node->u.map.items[i].value);
        }
    }
    return (nullptr);
}

// List length. Zero if not a list.
size_t suite_list_len(const SuiteNode *node) {
    if (node == nullptr) {
        return (0);
    }
    if (node->kind != SUITE_LIST) {
        return (0);
    }
    return (node->u.list.len);
}

// List item. Nullptr if bad index.
SuiteNode *suite_list_at(SuiteNode *node,
                         size_t idx) {
    if (node == nullptr) {
        return (nullptr);
    }
    if (node->kind != SUITE_LIST) {
        return (nullptr);
    }
    if (idx >= node->u.list.len) {
        return (nullptr);
    }
    return (node->u.list.items[idx]);
}

// Scalar text. Nullptr if not scalar.
const char *suite_scalar_str(
    const SuiteNode *node) {
    if (node == nullptr) {
        return (nullptr);
    }
    if (node->kind != SUITE_SCALAR) {
        return (nullptr);
    }
    return (node->u.scalar.text);
}

// Scalar as integer. False if bad.
bool suite_scalar_int(const SuiteNode *node,
                      long long *out) {
    if (node == nullptr || out == nullptr) {
        return (false);
    }
    if (node->kind != SUITE_SCALAR) {
        return (false);
    }
    const char *s = node->u.scalar.text;
    size_t i = 0;
    if (s[0] == '+' || s[0] == '-') {
        i = 1;
    }
    if (s[i] == '\0') {
        return (false);
    }
    for (size_t j = i; s[j] != '\0'; j++) {
        if (s[j] < '0' || s[j] > '9') {
            return (false);
        }
    }
    errno = 0;
    char *end = nullptr;
    long long v = strtoll(s, &end, 10);
    if (errno == ERANGE) {
        return (false);
    }
    if (end == nullptr || *end != '\0') {
        return (false);
    }
    *out = v;
    return (true);
}
