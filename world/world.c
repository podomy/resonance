#include "world.h"
#include <stdint.h>
#include <stdlib.h>

bool mediumgrid_init(MediumGrid* grid, int64_t origin_x_nm,
                     int64_t origin_y_nm, uint64_t cell_nm,
                     uint64_t nx, uint64_t ny,
                     Material fill) {
    uint64_t n;
    if (grid == NULL || cell_nm == 0 || nx == 0 ||
        ny == 0) {
        return false;
    }
    if (nx > UINT64_MAX / ny) {
        return false;
    }
    n = nx * ny;
    if (n > SIZE_MAX / sizeof(*grid->cells)) {
        return false;
    }
    grid->cells = malloc(n * sizeof(*grid->cells));
    if (grid->cells == NULL) {
        return false;
    }
    grid->cell_nm = cell_nm;
    grid->origin_x_nm = origin_x_nm;
    grid->origin_y_nm = origin_y_nm;
    grid->nx = nx;
    grid->ny = ny;
    for (uint64_t i = 0; i < n; i++) {
        grid->cells[i] = fill;
    }
    return true;
}

bool mediumgrid_free(MediumGrid* grid) {
    if (grid == NULL) {
        return false;
    }
    free(grid->cells);
    grid->cells = NULL;
    grid->nx = 0;
    grid->ny = 0;
    grid->cell_nm = 0;
    grid->origin_x_nm = 0;
    grid->origin_y_nm = 0;
    return true;
}

/*
 * Speed of the wave in each material (nm / ns).
 * Zero means the cell blocks the ray.
 */
static const uint64_t k_c_nm_per_ns[MATERIAL_COUNT] = {
    [MATERIAL_AIR] = 300, [MATERIAL_ROCK] = 0,
    [MATERIAL_DIRT] = 0,  [MATERIAL_WATER] = 0,
    [MATERIAL_WOOD] = 0,
};

// Writes unreachable into rp.
static bool blocked(RadioPath* rp) {
    if (rp != NULL) {
        rp->reachable = false;
        rp->delay_ns = 0;
    }
    return false;
}

// nm -> cell index. False if outside the map.
static bool cell_of(const MediumGrid* grid, int64_t x_nm,
                    int64_t y_nm, int64_t* ix,
                    int64_t* iy) {
    uint64_t ux, uy;

    if (x_nm < grid->origin_x_nm ||
        y_nm < grid->origin_y_nm)
        return (false);
    ux = (uint64_t)(x_nm - grid->origin_x_nm) /
         grid->cell_nm;
    uy = (uint64_t)(y_nm - grid->origin_y_nm) /
         grid->cell_nm;
    if (ux >= grid->nx || uy >= grid->ny)
        return (false);
    *ix = (int64_t)ux;
    *iy = (int64_t)uy;
    return (true);
}

// Walks cells A to B. Adds cell_nm/c per cell. False if
// blocked.
static bool segment_delay(const MediumGrid* grid,
                          int64_t x0, int64_t y0,
                          int64_t x1, int64_t y1,
                          uint64_t* delay_ns) {
    int64_t dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    int64_t dy = y1 >= y0 ? y0 - y1 : y1 - y0;
    int64_t sx = x0 < x1 ? 1 : -1;
    int64_t sy = y0 < y1 ? 1 : -1;
    int64_t err = dx + dy;
    uint64_t delay = 0;

    while (true) {
        size_t i =
            (size_t)y0 * (size_t)grid->nx + (size_t)x0;
        Material mat = grid->cells[i];
        uint64_t c;
        if (mat >= MATERIAL_COUNT) {
            return false;
        }
        c = k_c_nm_per_ns[mat];
        if (c == 0) {
            return false;
        }
        delay += grid->cell_nm / c;
        if (x0 == x1 && y0 == y1) {
            *delay_ns = delay;
            return true;
        }
        int64_t e2 = err * 2;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

// Fills rp. False if args are invalid or the path is
// blocked.
bool radio_path(const MediumGrid* grid,
                const RadioParams* params, int64_t ax_nm,
                int64_t ay_nm, int64_t bx_nm, int64_t by_nm,
                RadioPath* rp) {
    int64_t ax, ay, bx, by;
    __int128 dx, dy, d2, r2;

    uint64_t delay_ns;

    if (grid == NULL || params == NULL || rp == NULL ||
        grid->cells == NULL || grid->cell_nm == 0) {
        return blocked(rp);
    }

    dx = (__int128)bx_nm - ax_nm;
    dy = (__int128)by_nm - ay_nm;
    d2 = dx * dx + dy * dy;
    r2 = (__int128)params->range_nm * params->range_nm;
    if (d2 > r2) {
        return blocked(rp);
    }

    if (!cell_of(grid, ax_nm, ay_nm, &ax, &ay) ||
        !cell_of(grid, bx_nm, by_nm, &bx, &by) ||
        !segment_delay(grid, ax, ay, bx, by, &delay_ns)) {
        return blocked(rp);
    }

    rp->reachable = true;
    rp->delay_ns = (d2 == 0) ? 0 : delay_ns;
    return true;
}
