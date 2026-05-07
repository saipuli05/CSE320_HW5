/*
 * Binary network protocol serialisation/deserialisation.
 *
 * All multi-byte integer fields are big-endian (network byte order).  The
 * functions are entirely self-contained and have no I/O - they read from
 * or write to caller-provided buffers, so they can be tested with
 * Criterion in isolation.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>

#include "protocol.h"
#include "game_board.h"
#include "snake.h"
#include "global.h"
#include "debug.h"

/* ---------------------------------------------------------------------------
 * Helper: write a uint16 in big-endian order at buf[offset].
 * ---------------------------------------------------------------------------
 */
static void put_be16(uint8_t *buf, size_t offset, uint16_t value) {
    uint16_t be = htons(value);
    memcpy(buf + offset, &be, sizeof(be));
}

/* ---------------------------------------------------------------------------
 * protocol_serialize_welcome
 * ---------------------------------------------------------------------------
 * Layout:  [0]=0x10, [1]=player_id, [2]=board_size, [3]=max_players.
 */
int protocol_serialize_welcome(uint8_t *buf, size_t buf_len, int player_id,
                               int board_size, int max_players) {
    if (buf == NULL || buf_len < 4) {
        return -1;
    }
    if (player_id   < 0 || player_id   > 0xFF ||
        board_size  < 0 || board_size  > 0xFF ||
        max_players < 0 || max_players > 0xFF) {
        return -1;
    }
    buf[0] = MSG_WELCOME;
    buf[1] = (uint8_t)player_id;
    buf[2] = (uint8_t)board_size;
    buf[3] = (uint8_t)max_players;
    return 4;
}

/* ---------------------------------------------------------------------------
 * protocol_serialize_game_state
 * ---------------------------------------------------------------------------
 * Variable-length message:
 *   [0]   = 0x20
 *   [1]   = number of alive snakes
 *   [2:4] = apple_x   (uint16 BE)
 *   [4:6] = apple_y   (uint16 BE)
 *   then for each alive snake (in id order):
 *     [+0]  snake id
 *     [+1:3] length (uint16 BE)
 *     [+3]  direction
 *     [+4 ..] length pairs of (x_be16, y_be16), head first
 */
int protocol_serialize_game_state(uint8_t *buf, size_t buf_len,
                                  const game_board_t *board) {
    if (buf == NULL || board == NULL) {
        return -1;
    }
    if (buf_len < 6) {
        return -1;
    }

    /* First pass: count alive snakes and check the buffer can fit
     * everything before we start writing. */
    int alive_count = 0;
    size_t needed = 6;  /* fixed header */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (board->snakes[i].alive) {
            alive_count++;
            needed += 4 + (size_t)board->snakes[i].length * 4;
        }
    }
    if (needed > buf_len) {
        return -1;
    }
    if (alive_count > 0xFF) {
        return -1;
    }

    /* Header */
    buf[0] = MSG_GAME_STATE;
    buf[1] = (uint8_t)alive_count;
    /* Apple position; clamp to non-negative for the wire (an unset apple
     * would have x=y=-1 from board_init - safest is to send 0,0 in that
     * unlikely case). */
    uint16_t ax = (board->apple.x >= 0) ? (uint16_t)board->apple.x : 0;
    uint16_t ay = (board->apple.y >= 0) ? (uint16_t)board->apple.y : 0;
    put_be16(buf, 2, ax);
    put_be16(buf, 4, ay);

    /* Per-snake records, in id order (so the bytes are deterministic). */
    size_t off = 6;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const snake_t *s = &board->snakes[i];
        if (!s->alive) {
            continue;
        }
        buf[off + 0] = (uint8_t)s->id;
        put_be16(buf, off + 1, (uint16_t)s->length);
        buf[off + 3] = (uint8_t)s->direction;
        off += 4;
        for (int j = 0; j < s->length; j++) {
            put_be16(buf, off + 0, (uint16_t)s->body[j].x);
            put_be16(buf, off + 2, (uint16_t)s->body[j].y);
            off += 4;
        }
    }

    return (int)off;
}

/* ---------------------------------------------------------------------------
 * protocol_serialize_dead
 * ---------------------------------------------------------------------------
 * Layout: [0]=0x30, [1]=player_id.
 */
int protocol_serialize_dead(uint8_t *buf, size_t buf_len, int player_id) {
    if (buf == NULL || buf_len < 2) {
        return -1;
    }
    if (player_id < 0 || player_id > 0xFF) {
        return -1;
    }
    buf[0] = MSG_PLAYER_DEAD;
    buf[1] = (uint8_t)player_id;
    return 2;
}

/* ---------------------------------------------------------------------------
 * protocol_serialize_game_over
 * ---------------------------------------------------------------------------
 * Layout: [0]=0x40, [1]=winner_id (or 0xFF).  We accept winner_id in the
 * range -1..255; -1 is interpreted as "no winner" and serialised as 0xFF.
 */
int protocol_serialize_game_over(uint8_t *buf, size_t buf_len, int winner_id) {
    if (buf == NULL || buf_len < 2) {
        return -1;
    }
    uint8_t encoded;
    if (winner_id < 0) {
        encoded = 0xFF;
    } else if (winner_id <= 0xFF) {
        encoded = (uint8_t)winner_id;
    } else {
        return -1;
    }
    buf[0] = MSG_GAME_OVER;
    buf[1] = encoded;
    return 2;
}

/* ---------------------------------------------------------------------------
 * protocol_serialize_error
 * ---------------------------------------------------------------------------
 * Layout: [0]=0xF0, [1]=error_code.
 */
int protocol_serialize_error(uint8_t *buf, size_t buf_len, uint8_t error_code) {
    if (buf == NULL || buf_len < 2) {
        return -1;
    }
    buf[0] = MSG_ERROR;
    buf[1] = error_code;
    return 2;
}

/* ---------------------------------------------------------------------------
 * protocol_deserialize_client_msg
 * ---------------------------------------------------------------------------
 * Validate and split a 2-byte client message into (type, payload).  An
 * unknown type is rejected with -1 so the caller can reply ERR_INVALID_MSG.
 */
int protocol_deserialize_client_msg(const uint8_t *buf, size_t buf_len,
                                    uint8_t *out_type, uint8_t *out_payload) {
    if (buf == NULL || out_type == NULL || out_payload == NULL) {
        return -1;
    }
    if (buf_len < CLIENT_MSG_SIZE) {
        return -1;
    }

    uint8_t type    = buf[0];
    uint8_t payload = buf[1];

    switch (type) {
    case MSG_JOIN:
    case MSG_LEAVE:
        /* Payload byte is reserved; spec shows it as 0x00 but we don't
         * enforce that - we still report the byte we received. */
        break;
    case MSG_DIRECTION:
        if (payload != DIR_UP   && payload != DIR_DOWN &&
            payload != DIR_LEFT && payload != DIR_RIGHT) {
            return -1;
        }
        break;
    default:
        return -1;
    }

    *out_type    = type;
    *out_payload = payload;
    return 0;
}
