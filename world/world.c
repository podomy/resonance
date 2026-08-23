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

// Floor integer square root.
static uint64_t isqrt_u64(uint64_t n) {
    uint64_t x = n;
    uint64_t y;
    if (n <= 1) {
        return n;
    }
    y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

// Writes unreachable into rp.
static bool blocked(RadioPath* rp) {
    if (rp != NULL) {
        rp->reachable = false;
        rp->delay_ns = 0;
    }
    return false;
}

// Maps nm to a cell. False if outside the grid.
// Here we have an assumption of one corner of the map,
// as being the minimum and the origin.
static bool cell_of(const MediumGrid* grid, int64_t x_nm,
                    int64_t y_nm, int64_t* ix,
                    int64_t* iy) {
    uint64_t ux;
    uint64_t uy;
    
    // If the coordinate is not inside the map and any of the
    // cells then we return false.
    if (x_nm < grid->origin_x_nm ||
        y_nm < grid->origin_y_nm) {
        return false;
    }

    // Distance from the origin that the point has in cell indices.
    ux = (uint64_t)(x_nm - grid->origin_x_nm) /
         grid->cell_nm;
    uy = (uint64_t)(y_nm - grid->origin_y_nm) /
         grid->cell_nm;
    
    // The mapped indices go out of the bounds of the map.
    // We return false, because it is out of range, too far.
    if (ux >= grid->nx || uy >= grid->ny) {
        return false;
    }
    *ix = (int64_t)ux;
    *iy = (int64_t)uy;
    return true;
}

// False if any cell on the segment is not air.
static bool segment_clear(const MediumGrid* grid,
                          int64_t x0, int64_t y0,
                          int64_t x1, int64_t y1) {
    int64_t dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    int64_t dy = y1 >= y0 ? y0 - y1 : y1 - y0;
    int64_t sx = x0 < x1 ? 1 : -1;
    int64_t sy = y0 < y1 ? 1 : -1;
    int64_t err = dx + dy;
    
    while (true) {
        size_t i =
            (size_t)y0 * (size_t)grid->nx + (size_t)x0;
        if (grid->cells[i] != MATERIAL_AIR) {
            return false;
        }
        if (x0 == x1 && y0 == y1) {
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

    if (grid == NULL || params == NULL || rp == NULL ||
        grid->cells == NULL || grid->cell_nm == 0 ||
        params->c_nm_per_ns == 0) {
        return blocked(rp);
    }

    dx = (__int128)bx_nm - ax_nm;
    dy = (__int128)by_nm - ay_nm;

    // Squared distance.
    d2 = dx * dx + dy * dy;
    // Squared range.
    r2 = (__int128)params->range_nm * params->range_nm;
    if (d2 > r2) {
        return blocked(rp);
    }

    if (!cell_of(grid, ax_nm, ay_nm, &ax, &ay) ||
        !cell_of(grid, bx_nm, by_nm, &bx, &by) ||
        !segment_clear(grid, ax, ay, bx, by)) {
        return blocked(rp);
    }

    rp->reachable = true;
    rp->delay_ns =
        isqrt_u64(d2 > UINT64_MAX ? UINT64_MAX
                                  : (uint64_t)d2) /
        params->c_nm_per_ns;
    return true;
}
