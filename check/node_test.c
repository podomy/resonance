#include "../node/node.h"
#include <assert.h>
#include <stdint.h>

int main(void) {
    NodeList list;
    assert(nodelist_init(&list, 1));

    Node a = {.id = 0, .x_nm = 0, .y_nm = 0};
    Node b = {.id = 1, .x_nm = 1, .y_nm = 0};
    Node c = {.id = 2, .x_nm = 2, .y_nm = 0};
    assert(nodelist_push(&list, a));
    assert(nodelist_push(&list, b));
    assert(nodelist_push(&list, c));

    assert(nodelist_find(&list, 1)->x_nm == 1);
    assert(nodelist_remove_by_id(&list, 0));
    assert(nodelist_find(&list, 0) == NULL);
    assert(nodelist_find(&list, 2)->x_nm == 2);
    assert(nodelist_find(&list, 1)->x_nm == 1);
    assert(!nodelist_remove_by_id(&list, 0));
    assert(list.len == 2);

    assert(nodelist_free(&list));
    return 0;
}
