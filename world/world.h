#ifndef WORLD_H
#define WORLD_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Spatial medium. The grid is the map. RadioPath is derived
 * from two positions and is not stored in the world.
 */

typedef enum {
    MATERIAL_AIR = 0,
    MATERIAL_ROCK,
    MATERIAL_DIRT,
    MATERIAL_WATER,
    MATERIAL_WOOD,
} Material;

typedef struct {
    int64_t origin_x_nm;
    int64_t origin_y_nm;
    uint64_t cell_nm;
    uint64_t nx;
    uint64_t ny;
    Material* cells;
} MediumGrid;

typedef struct {
    uint64_t range_nm;
    uint64_t c_nm_per_ns;
} RadioParams;

typedef struct {
    bool reachable;
    uint64_t delay_ns;
} RadioPath;

// Allocates nx*ny cells filled with fill. cell_nm must be >
// 0.
bool mediumgrid_init(MediumGrid* grid, int64_t origin_x_nm,
                     int64_t origin_y_nm, uint64_t cell_nm,
                     uint64_t nx, uint64_t ny,
                     Material fill);

// Releases cell storage. False if grid is NULL.
bool mediumgrid_free(MediumGrid* grid);

// Fills rp. False if args are invalid or the path is
// blocked.
bool radio_path(const MediumGrid* grid,
                const RadioParams* params, int64_t ax_nm,
                int64_t ay_nm, int64_t bx_nm, int64_t by_nm,
                RadioPath* rp);

#endif
