/*
 * Per-snake movement logic.
 *
 * Two functions: snake_set_direction (buffer the next requested direction
 * unless it would cause an instant 180-degree reversal) and snake_advance
 * (apply the buffered direction and move one cell, handling wall/self/
 * other-snake collisions and apple consumption).
 */

#include <stddef.h>

#include "snake.h"
#include "game_board.h"
#include "global.h"
#include "debug.h"

/* ---------------------------------------------------------------------------
 * is_opposite_direction
 * ---------------------------------------------------------------------------
 * UP <-> DOWN and LEFT <-> RIGHT are opposites.  Anything else is allowed.
 */
static int is_opposite_direction(direction_t a, direction_t b) {
    return (a == DIR_UP    && b == DIR_DOWN)  ||
           (a == DIR_DOWN  && b == DIR_UP)    ||
           (a == DIR_LEFT  && b == DIR_RIGHT) ||
           (a == DIR_RIGHT && b == DIR_LEFT);
}

/* ---------------------------------------------------------------------------
 * snake_set_direction
 * ---------------------------------------------------------------------------
 * Buffer a direction request from the client.  An opposite-direction
 * request is silently ignored (per the spec, this is not an error).
 */
int snake_set_direction(snake_t *snake, direction_t dir) {
    if (snake == NULL) {
        return -1;
    }
    if (dir != DIR_UP && dir != DIR_DOWN && dir != DIR_LEFT && dir != DIR_RIGHT) {
        return -1;
    }

    /* The opposite-direction filter is only meaningful for a snake that
     * actually has a current direction; for a freshly-spawned snake of
     * length 1 we still apply it so the buffered direction stays
     * consistent with the head's facing. */
    if (is_opposite_direction(snake->direction, dir)) {
        return 0;
    }

    snake->next_direction = dir;
    return 0;
}

/* ---------------------------------------------------------------------------
 * snake_advance
 * ---------------------------------------------------------------------------
 * Move the snake by one cell, applying its buffered direction first.
 * Returns:
 *    0 = normal movement
 *    1 = death (collision with wall or any snake including itself)
 *    2 = apple eaten (snake grew)
 *   -1 = error (bad arguments / snake not alive)
 */
int snake_advance(struct game_board *board, int snake_id) {
    if (board == NULL || board->cells == NULL) {
        return -1;
    }
    if (snake_id < 0 || snake_id >= MAX_PLAYERS) {
        return -1;
    }

    snake_t *s = &board->snakes[snake_id];
    if (!s->alive || s->length <= 0) {
        return -1;
    }

    /* Apply the buffered input. */
    s->direction = s->next_direction;

    /* Compute the new head position. */
    int hx = s->body[0].x;
    int hy = s->body[0].y;
    int nx = hx, ny = hy;
    switch (s->direction) {
    case DIR_UP:    ny = hy - 1; break;
    case DIR_DOWN:  ny = hy + 1; break;
    case DIR_LEFT:  nx = hx - 1; break;
    case DIR_RIGHT: nx = hx + 1; break;
    default:        return -1;
    }

    int size = board->size;

    /* Anything outside the array bounds is treated as a wall collision.
     * In practice the wall ring at the border catches this first. */
    if (nx < 0 || nx >= size || ny < 0 || ny >= size) {
        board_remove_snake(board, snake_id);
        return 1;
    }

    cell_t target = board->cells[ny * size + nx];
    cell_t my_cell = (cell_t)(CELL_SNAKE_0 + snake_id);

    /* --- Wall or any-snake collision: snake dies. ----------------------- */
    if (target == CELL_WALL ||
        (target >= CELL_SNAKE_0 && target <= CELL_SNAKE_7)) {
        board_remove_snake(board, snake_id);
        return 1;
    }

    /* --- Apple: grow.  Do NOT clear the tail. -------------------------- */
    if (target == CELL_APPLE) {
        /* Shift body tail-to-head, write new head at body[0].  Cap length
         * at MAX_SNAKE_LENGTH; if already at the cap, the tail simply does
         * not extend further (this preserves the body in place). */
        int new_length = s->length + 1;
        if (new_length > MAX_SNAKE_LENGTH) {
            new_length = MAX_SNAKE_LENGTH;
            /* No room to grow: behave like a normal move - clear tail. */
            int tx = s->body[s->length - 1].x;
            int ty = s->body[s->length - 1].y;
            board->cells[ty * size + tx] = CELL_EMPTY;
            for (int i = s->length - 1; i > 0; i--) {
                s->body[i] = s->body[i - 1];
            }
        } else {
            for (int i = new_length - 1; i > 0; i--) {
                s->body[i] = s->body[i - 1];
            }
            s->length = new_length;
        }
        s->body[0].x = nx;
        s->body[0].y = ny;
        board->cells[ny * size + nx] = my_cell;

        /* Spawn the next apple.  An empty board (no free cells) is rare
         * but possible if every cell is occupied by snakes and walls; we
         * tolerate the failure and leave board->apple as-is. */
        (void)board_place_apple(board);
        return 2;
    }

    /* --- Empty: normal movement (clear tail, shift, write new head). --- */
    if (target == CELL_EMPTY) {
        int tx = s->body[s->length - 1].x;
        int ty = s->body[s->length - 1].y;
        board->cells[ty * size + tx] = CELL_EMPTY;

        for (int i = s->length - 1; i > 0; i--) {
            s->body[i] = s->body[i - 1];
        }
        s->body[0].x = nx;
        s->body[0].y = ny;
        board->cells[ny * size + nx] = my_cell;
        return 0;
    }

    /* Unknown cell type - treat as collision/death. */
    board_remove_snake(board, snake_id);
    return 1;
}
