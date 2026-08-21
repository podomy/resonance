#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include "node.h"

// Initialize the list with initial capacity.
bool nodelist_init(NodeList* list, size_t initial_cap) {
  if (list == NULL) {
    return false;
  }
  if (initial_cap == 0) {
    // This is needed so the multiplication below,
    // keeps on working.
    initial_cap = 1;
  }

  list->data = malloc(initial_cap * sizeof(*list->data));
  if(!list->data){
    return false;
  }
  
  list->len = 0;
  list->cap = initial_cap;

  return true;
}

// Add a node to the end, growing the array automatically if full.
bool nodelist_push(NodeList* list, Node node) {
  if (list->len >= list->cap) {
    // Double the capacity on growth.
    size_t new_cap = list->cap * 2;
    Node* new_data = realloc(list->data, new_cap * sizeof(*list->data));
    if(!new_data) {
      return false;
    }

    list->data = new_data;
    list->cap = new_cap;
  }

  list->data[list->len] = node;
  list->len++;

  return true;
}

// Free the memory when done. False if list is NULL.
bool nodelist_free(NodeList* list) {
  if (list == NULL) {
    return false;
  }
  free(list->data);
  list->data = NULL;
  list->len = 0;
  list->cap = 0;
  return true;
}

// Order doesn't matter we delete nodes in O(1) time complexity.
bool nodelist_remove_unordered(NodeList* list, size_t index) {
  if (index >= list->len) return false;

  list->data[index] = list->data[list->len - 1];
  // The last element is still there but we do not care about.
  list->len--;

  return true;
}
