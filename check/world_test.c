#include "../world/world.h"
#include <assert.h>

int main(void) {
    MediumGrid grid;
    RadioParams params;
    RadioPath rp;

    assert(mediumgrid_init(&grid, 0, 0, 1000, 8, 8,
                           MATERIAL_AIR));
    params.range_nm = 100000;

    assert(radio_path(&grid, &params, 500, 500, 2500, 500,
                      &rp));
    assert(rp.reachable);
    assert(rp.delay_ns == 3 * (1000 / 300));

    assert(!radio_path(&grid, &params, 500, 500, 200000,
                       500, &rp));
    assert(!rp.reachable);

    grid.cells[1 * 8 + 2] = MATERIAL_ROCK;
    assert(!radio_path(&grid, &params, 500, 1500, 3500,
                       1500, &rp));
    assert(!rp.reachable);

    assert(mediumgrid_free(&grid));
    return 0;
}
