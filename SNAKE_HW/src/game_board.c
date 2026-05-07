/*
 * Game board management for the snake server.
 *
 * Implements the core game-board data-structure operations: allocation,
 * deterministic random apple placement, snake placement and removal, and
 * the per-tick advance.  All functions here operate purely on in-memory
 * data structures - no networking, no threading.  Synchronisation around
 * concurrent access is the responsibility of the server layer.
 */

#include <stdlib.h>

#include "game_board.h"
#include "snake.h"
#include "global.h"
#include "debug.h"

/* ---------------------------------------------------------------------------
 * board_init
 * ---------------------------------------------------------------------------
 * Allocate the cells array, place walls around the border, initialise all
 * snake slots to a default (dead) state, store the seed, and place the
 * first apple.
 */
int board_init(game_board_t *board, int size, int max_snakes,
               unsigned int seed) {
    if (board == NULL) {
        return -1;
    }
    if (size < BOARD_SIZE_MIN || size > BOARD_SIZE_MAX) {
        return -1;
    }
    if (max_snakes < MAX_PLAYERS_MIN || max_snakes > MAX_PLAYERS_MAX) {
        return -1;
    }

    board->size       = size;
    board->max_snakes = max_snakes;
    board->num_snakes = 0;
    board->rng_state  = seed;
    board->apple.x    = -1;
    board->apple.y    = -1;

    /* Allocate the flattened cells array, zero-initialised so every cell
     * starts as CELL_EMPTY (which is 0). */
    board->cells = calloc((size_t)size * (size_t)size, sizeof(cell_t));
    if (board->cells == NULL) {
        return -1;
    }

    /* Place walls around the border. */
    for (int x = 0; x < size; x++) {
        board->cells[0 * size + x]          = CELL_WALL;
        board->cells[(size - 1) * size + x] = CELL_WALL;
    }
    for (int y = 0; y < size; y++) {
        board->cells[y * size + 0]          = CELL_WALL;
        board->cells[y * size + (size - 1)] = CELL_WALL;
    }

    /* Initialise every snake slot to a dead/empty state.  body[] does not
     * need to be cleared because length==0 means no segments are read. */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        board->snakes[i].id             = i;
        board->snakes[i].length         = 0;
        board->snakes[i].direction      = DIR_RIGHT;
        board->snakes[i].next_direction = DIR_RIGHT;
        board->snakes[i].alive          = 0;
    }

    /* Place the first apple.  If placement fails (board full of walls only
     * if size is 1 or 2), it is not a fatal error - the apple just stays
     * unplaced until a cell becomes free. */
    (void)board_place_apple(board);

    return 0;
}

/* ---------------------------------------------------------------------------
 * board_free
 * ---------------------------------------------------------------------------
 * Release the dynamically allocated cells array.  Safe to call with a NULL
 * pointer or a board whose cells pointer is already NULL.
 */
void board_free(game_board_t *board) {
    if (board == NULL) {
        return;
    }
    if (board->cells != NULL) {
        free(board->cells);
        board->cells = NULL;
    }
}

/* ---------------------------------------------------------------------------
 * board_random
 * ---------------------------------------------------------------------------
 * Linear-congruential pseudo-random generator.  This exact formula is
 * mandated by the spec so apple placement is reproducible across
 * implementations given the same seed.
 */
unsigned int board_random(game_board_t *board) {
    board->rng_state = board->rng_state * 1103515245 + 12345;
    return (board->rng_state / 65536) % 32768;
}

/* ---------------------------------------------------------------------------
 * board_place_apple
 * ---------------------------------------------------------------------------
 * Place an apple on a uniformly random empty cell.  Algorithm:
 *   1. Count CELL_EMPTY cells.
 *   2. If none, return -1.
 *   3. Pick target = random() % empty_count.
 *   4. Walk the cells array in row-major order, decrementing target until
 *      it reaches zero on an empty cell, and place the apple there.
 */
int board_place_apple(game_board_t *board) {
    if (board == NULL || board->cells == NULL) {
        return -1;
    }

    int total = board->size * board->size;
    int empty_count = 0;
    for (int i = 0; i < total; i++) {
        if (board->cells[i] == CELL_EMPTY) {
            empty_count++;
        }
    }

    if (empty_count == 0) {
        return -1;
    }

    unsigned int r = board_random(board);
    int target = (int)(r % (unsigned int)empty_count);

    for (int i = 0; i < total; i++) {
        if (board->cells[i] == CELL_EMPTY) {
            if (target == 0) {
                board->cells[i] = CELL_APPLE;
                board->apple.x  = i % board->size;
                board->apple.y  = i / board->size;
                return 0;
            }
            target--;
        }
    }

    /* Should be unreachable since empty_count > 0. */
    return -1;
}

/* ---------------------------------------------------------------------------
 * find_empty_in_interior
 * ---------------------------------------------------------------------------
 * Helper for board_add_snake: starting from (start_x, start_y), search for
 * the first CELL_EMPTY cell scanning rightward then wrapping downward,
 * staying strictly inside the wall border.  Returns 0 and writes the
 * position into *out on success, or -1 if no empty interior cell exists.
 */
static int find_empty_in_interior(game_board_t *board,
                                  int start_x, int start_y,
                                  position_t *out) {
    int size = board->size;

    /* Clamp the starting position into the interior [1, size-2]. */
    if (start_x < 1)        start_x = 1;
    if (start_x > size - 2) start_x = size - 2;
    if (start_y < 1)        start_y = 1;
    if (start_y > size - 2) start_y = size - 2;

    int interior = size - 2;       /* width of the interior strip       */
    if (interior <= 0) {
        return -1;
    }
    int total = interior * interior;

    int x = start_x;
    int y = start_y;
    for (int step = 0; step < total; step++) {
        if (board->cells[y * size + x] == CELL_EMPTY) {
            out->x = x;
            out->y = y;
            return 0;
        }
        /* Advance: rightward, wrap to next row at the right wall. */
        x++;
        if (x > size - 2) {
            x = 1;
            y++;
            if (y > size - 2) {
                y = 1;
            }
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * board_add_snake
 * ---------------------------------------------------------------------------
 * Place a new snake in the first available slot, picking a starting cell
 * based on the slot id quadrant.  See README for the exact algorithm.
 */
int board_add_snake(game_board_t *board, int *out_id) {
    if (board == NULL || out_id == NULL || board->cells == NULL) {
        return -1;
    }

    /* Find first available slot.  The README says "first slot where
     * alive == 0", up to max_snakes (we cannot exceed the configured
     * concurrent player limit). */
    int slot = -1;
    for (int i = 0; i < board->max_snakes && i < MAX_PLAYERS; i++) {
        if (board->snakes[i].alive == 0) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        return -1;
    }

    int size = board->size;
    int q   = slot % 4;
    int sx, sy;
    switch (q) {
    case 0: sx = size / 4;       sy = size / 4;       break;
    case 1: sx = 3 * size / 4;   sy = size / 4;       break;
    case 2: sx = size / 4;       sy = 3 * size / 4;   break;
    case 3:
    default: sx = 3 * size / 4;  sy = 3 * size / 4;   break;
    }

    /* If the natural starting cell is not empty, scan for the next empty
     * interior cell. */
    position_t start;
    if (board->cells[sy * size + sx] == CELL_EMPTY) {
        start.x = sx;
        start.y = sy;
    } else if (find_empty_in_interior(board, sx, sy, &start) != 0) {
        return -1;
    }

    snake_t *s         = &board->snakes[slot];
    s->id              = slot;
    s->body[0]         = start;
    s->length          = 1;
    s->direction       = DIR_RIGHT;
    s->next_direction  = DIR_RIGHT;
    s->alive           = 1;

    board->cells[start.y * size + start.x] = (cell_t)(CELL_SNAKE_0 + slot);
    board->num_snakes++;
    *out_id = slot;
    return 0;
}

/* ---------------------------------------------------------------------------
 * board_remove_snake
 * ---------------------------------------------------------------------------
 * Clear a snake's cells back to CELL_EMPTY, mark it dead, drop num_snakes.
 */
int board_remove_snake(game_board_t *board, int snake_id) {
    if (board == NULL || board->cells == NULL) {
        return -1;
    }
    if (snake_id < 0 || snake_id >= MAX_PLAYERS) {
        return -1;
    }

    snake_t *s = &board->snakes[snake_id];
    if (s->alive == 0) {
        return -1;
    }

    int size = board->size;
    cell_t my_cell = (cell_t)(CELL_SNAKE_0 + snake_id);
    for (int i = 0; i < s->length; i++) {
        int x = s->body[i].x;
        int y = s->body[i].y;
        if (x >= 0 && x < size && y >= 0 && y < size) {
            /* Only clear cells that actually contain this snake.  The head
             * may have been overwritten by a wall/other-snake collision
             * marker by the caller in some edge cases; guard against that. */
            if (board->cells[y * size + x] == my_cell) {
                board->cells[y * size + x] = CELL_EMPTY;
            }
        }
    }

    s->alive  = 0;
    s->length = 0;
    if (board->num_snakes > 0) {
        board->num_snakes--;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * board_tick
 * ---------------------------------------------------------------------------
 * Advance every alive snake by one cell, in stable id order.  A snake that
 * dies during its own tick is removed inside snake_advance().
 */
int board_tick(game_board_t *board) {
    if (board == NULL || board->cells == NULL) {
        return -1;
    }

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (board->snakes[i].alive) {
            (void)snake_advance(board, i);
        }
    }
    return 0;
}
