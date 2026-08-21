#ifndef NODE_H 
#define NODE_H

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

typedef struct {
  uint64_t id;
  int64_t x_nm;
  int64_t y_nm;
} Node;

typedef struct {
  Node* data;
  size_t len;
  size_t cap;
} NodeList;

// Initialize the list with initial capacity.
bool nodelist_init(NodeList* list, size_t initial_cap);

// Add a node to the end, growing the array automatically if full.
bool nodelist_push(NodeList* list, Node node);

// Free the memory when done. False if list is NULL.
bool nodelist_free(NodeList* list);

// Order doesn't matter we delete nodes in O(1) time complexity.
bool nodelist_remove_unordered(NodeList* list, size_t index);

#endif
