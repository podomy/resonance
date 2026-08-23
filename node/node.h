#ifndef NODE_H
#define NODE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/*
 * Two stores inside NodeList.
 *
 * data[0 .. len)   live nodes, packed, no holes.
 * by_id.data[id]   slot in data, or SIZE_MAX if unused
 *                  or removed.
 *
 * Find:  i = by_id.data[id];  node = data[i]
 * Remove: swap data[i] with the last live node, then
 *         fix by_id.data for both ids.
 */

typedef struct {
    uint64_t id;
    int64_t x_nm;
    int64_t y_nm;
    int64_t vx_nm_per_ns;
    int64_t vy_nm_per_ns;
} Node;

typedef struct {
    size_t* data;
    size_t cap;
} IdIndex;

typedef struct {
    Node* data;
    size_t len;
    size_t cap;
    IdIndex by_id;
} NodeList;

// Initialize the list with initial capacity.
bool nodelist_init(NodeList* list, size_t initial_cap);

// Add a node. Records by_id.data[node.id].
bool nodelist_push(NodeList* list, Node node);

// Free the memory when done. False if list is NULL.
bool nodelist_free(NodeList* list);

// Live node with this id, or NULL.
Node* nodelist_find(NodeList* list, uint64_t id);

// Swap-remove the node at index. Updates the directory.
bool nodelist_remove_unordered(NodeList* list,
                               size_t index);

// Find by id, then swap-remove that slot.
bool nodelist_remove_by_id(NodeList* list, uint64_t id);

#endif
