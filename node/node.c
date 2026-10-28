#include "node.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// Grow by_id until id fits. New slots are SIZE_MAX.
static bool ensure_index_cap(IdIndex* by_id, uint64_t id) {
    if (id >= SIZE_MAX) {
        return false;
    }
    if (id < by_id->cap) {
        return true;
    }

    size_t new_cap = by_id->cap;
    if (new_cap == 0) {
        new_cap = 1;
    }
    while (new_cap <= (size_t)id) {
        if (new_cap > SIZE_MAX / 2) {
            return false;
        }
        new_cap *= 2;
    }

    size_t* grown =
        realloc(by_id->data, new_cap * sizeof(*grown));
    if (grown == nullptr) {
        return false;
    }
    for (size_t i = by_id->cap; i < new_cap; i++) {
        grown[i] = SIZE_MAX;
    }
    by_id->data = grown;
    by_id->cap = new_cap;
    return true;
}

// Initialize the list with initial capacity.
bool nodelist_init(NodeList* list, size_t initial_cap) {
    if (list == nullptr) {
        return false;
    }
    if (initial_cap == 0) {
        initial_cap = 1;
    }

    list->data = malloc(initial_cap * sizeof(*list->data));
    if (list->data == nullptr) {
        return false;
    }

    list->len = 0;
    list->cap = initial_cap;
    list->by_id.data = nullptr;
    list->by_id.cap = 0;
    return true;
}

// Add a node. Records by_id.data[node.id].
bool nodelist_push(NodeList* list, Node node) {
    if (list == nullptr) {
        return false;
    }
    if (!ensure_index_cap(&list->by_id, node.id)) {
        return false;
    }
    if (list->by_id.data[node.id] != SIZE_MAX) {
        return false;
    }

    if (list->len >= list->cap) {
        size_t new_cap;
        if (list->cap == 0) {
            new_cap = 1;
        } else if (list->cap > SIZE_MAX / 2) {
            return false;
        } else {
            new_cap = list->cap * 2;
        }
        if (new_cap > SIZE_MAX / sizeof(*list->data)) {
            return false;
        }
        Node* new_data = realloc(
            list->data, new_cap * sizeof(*list->data));
        if (new_data == nullptr) {
            return false;
        }
        list->data = new_data;
        list->cap = new_cap;
    }

    size_t i = list->len;
    list->data[i] = node;
    list->by_id.data[node.id] = i;
    list->len++;
    return true;
}

// Free the memory when done. False if list is nullptr.
bool nodelist_free(NodeList* list) {
    if (list == nullptr) {
        return false;
    }
    free(list->data);
    free(list->by_id.data);
    list->data = nullptr;
    list->by_id.data = nullptr;
    list->len = 0;
    list->cap = 0;
    list->by_id.cap = 0;
    return true;
}

// Live node with this id, or nullptr.
Node* nodelist_find(NodeList* list, uint64_t id) {
    if (list == nullptr || list->by_id.data == nullptr) {
        return nullptr;
    }
    if (id >= list->by_id.cap) {
        return nullptr;
    }
    size_t i = list->by_id.data[id];
    if (i == SIZE_MAX) {
        return nullptr;
    }
    return &list->data[i];
}

// Swap-remove the node at index. Updates the directory.
bool nodelist_remove_unordered(NodeList* list,
                               size_t index) {
    if (list == nullptr || index >= list->len) {
        return false;
    }

    uint64_t gone = list->data[index].id;
    list->by_id.data[gone] = SIZE_MAX;

    size_t last = list->len - 1;
    if (index != last) {
        list->data[index] = list->data[last];
        list->by_id.data[list->data[index].id] = index;
    }
    list->len--;
    return true;
}

// Find by id, then swap-remove that slot.
bool nodelist_remove_by_id(NodeList* list, uint64_t id) {
    if (list == nullptr || list->by_id.data == nullptr) {
        return false;
    }
    if (id >= list->by_id.cap) {
        return false;
    }
    size_t i = list->by_id.data[id];
    if (i == SIZE_MAX) {
        return false;
    }
    return nodelist_remove_unordered(list, i);
}
