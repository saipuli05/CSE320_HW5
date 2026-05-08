/*
 * Multi-threaded TCP server for the snake game.
 *
 * Threading model:
 *   - 1 main thread: accepts connections and spawns one detached
 *     client-handler thread per connection.
 *   - 1 game-loop thread: ticks the board on a fixed interval and
 *     broadcasts the serialised game state to every connected client.
 *   - N client-handler threads: one per connected player.  Each handler
 *     blocks in recv() waiting for 2-byte client messages.
 *
 * Synchronisation: a single pthread mutex (server->board_mutex) protects
 * all shared state (the game board, the client_fds and client_snake_ids
 * arrays).  The mutex is held only across short bursts of in-memory
 * work; it is NEVER held across blocking syscalls (recv/send/accept/
 * usleep) to keep the design deadlock-free and allow ThreadSanitizer to
 * pass cleanly.
 *
 * Note: send() must be performed without holding the mutex, but the
 * data we send (e.g. the serialised game state) is built while the
 * mutex is held into a local stack buffer, so the broadcast does not
 * race with mutating threads.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "server.h"
#include "game_board.h"
#include "snake.h"
#include "protocol.h"
#include "global.h"
#include "debug.h"

/* ---------------------------------------------------------------------------
 * Active-client counter
 * ---------------------------------------------------------------------------
 * Client handler threads are detached, so we cannot pthread_join them.  To
 * make sure server_cleanup does not free the board / destroy the mutex
 * while a handler thread still holds a reference to either, we keep a
 * counter of in-flight handlers and a condvar to signal when it drops to
 * zero.  These are file-static so we do not need to modify server.h.
 */
static pthread_mutex_t active_clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  active_clients_cond  = PTHREAD_COND_INITIALIZER;
static int             active_clients_count = 0;

/* Serialize send()/close() pairs on client sockets.  The game loop sends
 * from fd snapshots while client handlers may be exiting; this mutex keeps
 * a handler from closing a socket while another thread is in send() on it. */
static pthread_mutex_t socket_io_mutex = PTHREAD_MUTEX_INITIALIZER;

static void active_clients_inc(void) {
    pthread_mutex_lock(&active_clients_mutex);
    active_clients_count++;
    pthread_mutex_unlock(&active_clients_mutex);
}

static void active_clients_dec(void) {
    pthread_mutex_lock(&active_clients_mutex);
    if (active_clients_count > 0) {
        active_clients_count--;
    }
    if (active_clients_count == 0) {
        pthread_cond_broadcast(&active_clients_cond);
    }
    pthread_mutex_unlock(&active_clients_mutex);
}

static void active_clients_wait_zero(void) {
    pthread_mutex_lock(&active_clients_mutex);
    while (active_clients_count > 0) {
        pthread_cond_wait(&active_clients_cond, &active_clients_mutex);
    }
    pthread_mutex_unlock(&active_clients_mutex);
}

/* ---------------------------------------------------------------------------
 * Signal handling
 * ---------------------------------------------------------------------------
 * The handler writes to a single volatile int (the listening fd) and calls
 * shutdown(2) - both async-signal-safe operations.  No shared C variable
 * is read or written without proper synchronisation, so ThreadSanitizer
 * has nothing to flag.  shutdown() makes accept() in server_start return
 * an error, which triggers a clean shutdown.
 */
static volatile int g_listen_fd = -1;

static void server_signal_handler(int signum) {
    (void)signum;
    int fd = g_listen_fd;
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
    }
}

static void install_signal_handlers(int listen_fd) {
    g_listen_fd = listen_fd;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = server_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  /* no SA_RESTART: we want accept() to EINTR */
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Ignore SIGPIPE so writes to closed sockets only fail with EPIPE
     * instead of killing the process. */
    struct sigaction sa_pipe;
    memset(&sa_pipe, 0, sizeof(sa_pipe));
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sigaction(SIGPIPE, &sa_pipe, NULL);
}

/* ---------------------------------------------------------------------------
 * recv_exact / send_all
 * ---------------------------------------------------------------------------
 * Loop over recv()/send() until the requested number of bytes have been
 * transferred or the peer closes / an error occurs.  Both functions
 * gracefully retry on EINTR.
 */
int recv_exact(int fd, uint8_t *buf, size_t len) {
    if (buf == NULL) {
        return -1;
    }
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(fd, buf + total, len - total, 0);
        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        if (n == 0) {
            /* Peer performed an orderly shutdown. */
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

static int send_all_unlocked(int fd, const uint8_t *buf, size_t len) {
    if (buf == NULL) {
        return -1;
    }
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(fd, buf + total, len - total, MSG_NOSIGNAL);
        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        if (n == -1 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

int send_all(int fd, const uint8_t *buf, size_t len) {
    int ret;

    pthread_mutex_lock(&socket_io_mutex);
    ret = send_all_unlocked(fd, buf, len);
    pthread_mutex_unlock(&socket_io_mutex);

    return ret;
}

static int send_all_to_client_slot(server_t *server, int slot, int fd,
                                   int snake_id, const uint8_t *buf,
                                   size_t len) {
    int valid = 0;
    int ret = -1;

    if (server == NULL || buf == NULL || slot < 0 || slot >= MAX_PLAYERS ||
        fd < 0 || snake_id < 0) {
        return -1;
    }

    pthread_mutex_lock(&socket_io_mutex);
    pthread_mutex_lock(&server->board_mutex);
    valid = (server->client_fds[slot] == fd &&
             server->client_snake_ids[slot] == snake_id);
    pthread_mutex_unlock(&server->board_mutex);

    if (valid) {
        ret = send_all_unlocked(fd, buf, len);
    }

    pthread_mutex_unlock(&socket_io_mutex);
    return ret;
}

static void close_client_fd(int fd) {
    if (fd < 0) {
        return;
    }

    pthread_mutex_lock(&socket_io_mutex);
    close(fd);
    pthread_mutex_unlock(&socket_io_mutex);
}

/* ---------------------------------------------------------------------------
 * server_init
 * ---------------------------------------------------------------------------
 * Build the listening socket, initialise the board, init the mutex.  All
 * client slot tables are pre-set to -1 so a "free slot" is identifiable.
 */
int server_init(server_t *server, int port, int board_size,
                int max_players, unsigned int seed) {
    if (server == NULL) {
        return -1;
    }

    server->listen_fd = -1;
    server->running   = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        server->client_fds[i]       = -1;
        server->client_snake_ids[i] = -1;
    }

    /* Game board first - if it fails we don't leak a socket. */
    if (board_init(&server->board, board_size, max_players, seed) != 0) {
        return -1;
    }

    if (pthread_mutex_init(&server->board_mutex, NULL) != 0) {
        board_free(&server->board);
        return -1;
    }

    /* Listening socket. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        pthread_mutex_destroy(&server->board_mutex);
        board_free(&server->board);
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(fd);
        pthread_mutex_destroy(&server->board_mutex);
        board_free(&server->board);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        pthread_mutex_destroy(&server->board_mutex);
        board_free(&server->board);
        return -1;
    }

    if (listen(fd, MAX_PLAYERS) < 0) {
        close(fd);
        pthread_mutex_destroy(&server->board_mutex);
        board_free(&server->board);
        return -1;
    }

    server->listen_fd = fd;
    server->running   = 1;

    /* Install the SIGINT/SIGTERM/SIGPIPE handlers now that we have a
     * listening socket to point them at. */
    install_signal_handlers(fd);

    return 0;
}

/* ---------------------------------------------------------------------------
 * usleep_ms
 * ---------------------------------------------------------------------------
 * Tiny helper around usleep() that takes milliseconds, for readability.
 */
static void usleep_ms(unsigned int ms) {
    usleep(ms * 1000U);
}

/* ---------------------------------------------------------------------------
 * server_game_loop
 * ---------------------------------------------------------------------------
 * Sleeps for TICK_INTERVAL_MS, ticks the board under the mutex, sends a
 * PLAYER_DEAD message to any snake that just died, then broadcasts the
 * serialised game state to every connected client.
 *
 * The serialisation is done WHILE the mutex is held (so the snapshot is
 * coherent), but the actual send() calls happen AFTER releasing the
 * mutex, against snapshot copies of the per-client fds.  This keeps the
 * mutex window small and avoids the classic "send while holding lock"
 * deadlock.
 */
void *server_game_loop(void *arg) {
    server_t *server = (server_t *)arg;
    if (server == NULL) {
        return NULL;
    }

    while (1) {
        /* Read running flag under the mutex - it is shared state. */
        pthread_mutex_lock(&server->board_mutex);
        int still_running = server->running;
        pthread_mutex_unlock(&server->board_mutex);
        if (!still_running) {
            break;
        }

        usleep_ms(TICK_INTERVAL_MS);

        /* --- Critical section: tick + snapshot --- */
        uint8_t state_buf[GAME_STATE_BUF_SIZE];
        int     state_len = 0;
        int     fds_snapshot[MAX_PLAYERS];
        int     snake_ids_snapshot[MAX_PLAYERS];
        int     was_alive[MAX_PLAYERS];
        int     is_alive[MAX_PLAYERS];

        pthread_mutex_lock(&server->board_mutex);

        if (!server->running) {
            pthread_mutex_unlock(&server->board_mutex);
            break;
        }

        /* Record alive flags BEFORE the tick so we can detect deaths. */
        for (int i = 0; i < MAX_PLAYERS; i++) {
            was_alive[i] = server->board.snakes[i].alive;
        }

        board_tick(&server->board);

        for (int i = 0; i < MAX_PLAYERS; i++) {
            is_alive[i] = server->board.snakes[i].alive;
        }

        /* Snapshot the client tables. */
        for (int i = 0; i < MAX_PLAYERS; i++) {
            fds_snapshot[i]       = server->client_fds[i];
            snake_ids_snapshot[i] = server->client_snake_ids[i];
        }

        state_len = protocol_serialize_game_state(state_buf, sizeof(state_buf),
                                                  &server->board);

        pthread_mutex_unlock(&server->board_mutex);
        /* --- End critical section --- */

        /* Send PLAYER_DEAD to anyone who just died.  Do this on snapshot
         * data so we don't touch shared state while doing I/O. */
        for (int slot = 0; slot < MAX_PLAYERS; slot++) {
            int fd = fds_snapshot[slot];
            int sid = snake_ids_snapshot[slot];
            if (fd < 0 || sid < 0) {
                continue;
            }
            if (sid >= 0 && sid < MAX_PLAYERS &&
                was_alive[sid] && !is_alive[sid]) {
                uint8_t dead[2];
                int n = protocol_serialize_dead(dead, sizeof(dead), sid);
                if (n > 0) {
                    (void)send_all_to_client_slot(server, slot, fd, sid,
                                                  dead, (size_t)n);
                }
            }
        }

        /* Broadcast the serialised game state only to clients whose snake
         * is still alive after this tick.  Per the instructor: once a
         * snake dies, stop sending it GAME_STATE.  PLAYER_DEAD has already
         * been delivered above for any snake that died this tick. */
        if (state_len > 0) {
            for (int slot = 0; slot < MAX_PLAYERS; slot++) {
                int fd = fds_snapshot[slot];
                int sid = snake_ids_snapshot[slot];
                if (fd >= 0 && sid >= 0 &&
                    sid < MAX_PLAYERS && is_alive[sid]) {
                    (void)send_all_to_client_slot(server, slot, fd, sid,
                                                  state_buf,
                                                  (size_t)state_len);
                }
            }
        }
    }

    return NULL;
}

/* ---------------------------------------------------------------------------
 * find_my_slot
 * ---------------------------------------------------------------------------
 * Helper for client handler: find the client_fds slot matching this fd.
 * Caller must hold the mutex.  Returns -1 if not found.
 */
static int find_my_slot(server_t *server, int fd) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (server->client_fds[i] == fd) {
            return i;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * server_client_handler
 * ---------------------------------------------------------------------------
 * One thread per client.  Lifecycle:
 *
 *   1. Read the first 2-byte message.  Must be JOIN.
 *   2. Add a snake to the board.  Send WELCOME.  If the board is full,
 *      send ERR_GAME_FULL and close.
 *   3. Loop reading 2-byte messages, handling DIRECTION / LEAVE.
 *   4. On exit (LEAVE, disconnect, or error): remove the snake, clear
 *      the slot in client_fds/client_snake_ids, close the fd, free the
 *      argument struct, return.
 */
void *server_client_handler(void *arg) {
    client_handler_arg_t *carg = (client_handler_arg_t *)arg;
    if (carg == NULL) {
        return NULL;
    }

    server_t *server = carg->server;
    int       fd     = carg->client_fd;
    int       snake_id = -1;
    int       slot     = -1;

    if (server == NULL || fd < 0) {
        if (fd >= 0) {
            close_client_fd(fd);
        }
        free(carg);
        active_clients_dec();
        return NULL;
    }

    /* Reserve a client-fd slot before blocking for JOIN.  This lets
     * server_cleanup find and shutdown even a slow client that connected
     * but has not sent its first message yet.  A snake id of -1 means
     * "connected, but not ready for broadcasts." */
    pthread_mutex_lock(&server->board_mutex);
    if (server->running) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (server->client_fds[i] == -1) {
                slot = i;
                server->client_fds[i]       = fd;
                server->client_snake_ids[i] = -1;
                break;
            }
        }
    }
    pthread_mutex_unlock(&server->board_mutex);

    if (slot == -1) {
        uint8_t err[2];
        int n = protocol_serialize_error(err, sizeof(err), ERR_GAME_FULL);
        if (n > 0) {
            (void)send_all(fd, err, (size_t)n);
        }
        goto cleanup;
    }

    /* --- Step 1: expect JOIN as the first message. --------------------- */
    uint8_t client_buf[CLIENT_MSG_SIZE];
    if (recv_exact(fd, client_buf, CLIENT_MSG_SIZE) != 0) {
        goto cleanup;
    }
    uint8_t mtype, mpayload;
    if (protocol_deserialize_client_msg(client_buf, CLIENT_MSG_SIZE,
                                        &mtype, &mpayload) != 0 ||
        mtype != MSG_JOIN) {
        uint8_t err[2];
        int n = protocol_serialize_error(err, sizeof(err), ERR_INVALID_MSG);
        if (n > 0) {
            (void)send_all(fd, err, (size_t)n);
        }
        goto cleanup;
    }

    /* --- Step 2: add a snake under the mutex. -------------------------- */
    int board_size, max_players;
    int add_ok;
    pthread_mutex_lock(&server->board_mutex);
    if (server->running && slot >= 0 &&
        server->client_fds[slot] == fd &&
        server->client_snake_ids[slot] == -1) {
        add_ok = board_add_snake(&server->board, &snake_id);
    } else {
        add_ok = -1;
    }
    board_size  = server->board.size;
    max_players = server->board.max_snakes;
    pthread_mutex_unlock(&server->board_mutex);

    if (add_ok != 0) {
        uint8_t err[2];
        int n = protocol_serialize_error(err, sizeof(err), ERR_GAME_FULL);
        if (n > 0) {
            (void)send_all(fd, err, (size_t)n);
        }
        goto cleanup;
    }

    /* --- Step 3: send WELCOME. ---------------------------------------- */
    uint8_t welcome[4];
    int wn = protocol_serialize_welcome(welcome, sizeof(welcome),
                                        snake_id, board_size, max_players);
    if (wn <= 0 || send_all(fd, welcome, (size_t)wn) != 0) {
        goto cleanup;
    }

    /* Publish the snake id only after WELCOME has been sent, so the game
     * loop cannot race ahead and deliver GAME_STATE as the client's first
     * message. */
    int published = 0;
    pthread_mutex_lock(&server->board_mutex);
    if (server->running && slot >= 0 &&
        server->client_fds[slot] == fd &&
        server->client_snake_ids[slot] == -1) {
        server->client_snake_ids[slot] = snake_id;
        published = 1;
    }
    pthread_mutex_unlock(&server->board_mutex);

    if (!published) {
        goto cleanup;
    }

    PRINT_CLIENT_CONNECTED("client", snake_id);

    /* --- Step 4: process subsequent messages until disconnect. -------- */
    while (1) {
        if (recv_exact(fd, client_buf, CLIENT_MSG_SIZE) != 0) {
            break;
        }
        if (protocol_deserialize_client_msg(client_buf, CLIENT_MSG_SIZE,
                                            &mtype, &mpayload) != 0) {
            uint8_t err[2];
            int n = protocol_serialize_error(err, sizeof(err), ERR_INVALID_MSG);
            if (n > 0) {
                (void)send_all(fd, err, (size_t)n);
            }
            continue;
        }

        if (mtype == MSG_LEAVE) {
            break;
        } else if (mtype == MSG_DIRECTION) {
            pthread_mutex_lock(&server->board_mutex);
            if (snake_id >= 0 && snake_id < MAX_PLAYERS &&
                server->board.snakes[snake_id].alive) {
                (void)snake_set_direction(&server->board.snakes[snake_id],
                                          (direction_t)mpayload);
            }
            pthread_mutex_unlock(&server->board_mutex);
        } else if (mtype == MSG_JOIN) {
            /* Already joined - report a protocol error but keep going. */
            uint8_t err[2];
            int n = protocol_serialize_error(err, sizeof(err),
                                             ERR_ALREADY_JOINED);
            if (n > 0) {
                (void)send_all(fd, err, (size_t)n);
            }
        } else {
            uint8_t err[2];
            int n = protocol_serialize_error(err, sizeof(err), ERR_INVALID_MSG);
            if (n > 0) {
                (void)send_all(fd, err, (size_t)n);
            }
        }
    }

cleanup:
    /* Clean up the slot/snake under the mutex. */
    pthread_mutex_lock(&server->board_mutex);
    if (slot == -1) {
        slot = find_my_slot(server, fd);
    }
    if (slot >= 0) {
        server->client_fds[slot]       = -1;
        server->client_snake_ids[slot] = -1;
    }
    if (snake_id >= 0 && snake_id < MAX_PLAYERS &&
        server->board.snakes[snake_id].alive) {
        (void)board_remove_snake(&server->board, snake_id);
    }
    pthread_mutex_unlock(&server->board_mutex);

    PRINT_CLIENT_DISCONNECTED(snake_id);

    close_client_fd(fd);
    free(carg);
    active_clients_dec();
    return NULL;
}

/* ---------------------------------------------------------------------------
 * server_start
 * ---------------------------------------------------------------------------
 * Spin up the game-loop thread, then accept() forever.  Each accepted
 * connection gets a freshly malloc'd argument struct and a detached
 * client-handler thread.  The accept loop terminates either when
 * server->running goes false or accept() returns -1 with EBADF/EINVAL
 * (which happens after server_cleanup closes the listening socket).
 */
int server_start(server_t *server) {
    if (server == NULL || server->listen_fd < 0) {
        return -1;
    }

    pthread_t game_thread;
    if (pthread_create(&game_thread, NULL, server_game_loop, server) != 0) {
        return -1;
    }

    while (1) {
        struct sockaddr_in caddr;
        socklen_t          clen = sizeof(caddr);
        int cfd = accept(server->listen_fd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) {
            if (errno == EINTR) {
                /* Re-check the running flag; if we should stop, break out. */
                pthread_mutex_lock(&server->board_mutex);
                int run = server->running;
                pthread_mutex_unlock(&server->board_mutex);
                if (!run) {
                    break;
                }
                continue;
            }
            /* Listening socket closed by cleanup or other fatal error. */
            break;
        }

        /* If we are no longer accepting, drop the connection. */
        pthread_mutex_lock(&server->board_mutex);
        int run = server->running;
        pthread_mutex_unlock(&server->board_mutex);
        if (!run) {
            close(cfd);
            break;
        }

        client_handler_arg_t *carg = malloc(sizeof(*carg));
        if (carg == NULL) {
            close(cfd);
            continue;
        }
        carg->server    = server;
        carg->client_fd = cfd;

        pthread_t tid;
        active_clients_inc();
        if (pthread_create(&tid, NULL, server_client_handler, carg) != 0) {
            close(cfd);
            free(carg);
            active_clients_dec();
            continue;
        }
        pthread_detach(tid);
    }

    /* The accept loop has exited (either because the listening socket was
     * closed by the signal handler, or because of a fatal error).  Mark
     * the server as not running so the game-loop thread will exit on its
     * next iteration, then wait for it to finish. */
    pthread_mutex_lock(&server->board_mutex);
    server->running = 0;
    pthread_mutex_unlock(&server->board_mutex);

    pthread_join(game_thread, NULL);
    return 0;
}

/* ---------------------------------------------------------------------------
 * server_cleanup
 * ---------------------------------------------------------------------------
 * Tear everything down.  Sets running=0, shuts down all client sockets so
 * their handler threads unblock and close their own fds, closes the
 * listening socket, frees the board, destroys the mutex.
 */
void server_cleanup(server_t *server) {
    if (server == NULL) {
        return;
    }

    /* Disarm the signal handler first so it cannot fire mid-cleanup. */
    g_listen_fd = -1;

    /* Mark not running and shutdown client sockets under the mutex.  The
     * shutdown() calls are intentionally not close(): handlers own their
     * client fds and will close them after recv() wakes up. */
    int listen_to_close;

    pthread_mutex_lock(&server->board_mutex);
    server->running = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (server->client_fds[i] >= 0) {
            shutdown(server->client_fds[i], SHUT_RDWR);
        }
        server->client_fds[i]       = -1;
        server->client_snake_ids[i] = -1;
    }
    listen_to_close = server->listen_fd;
    server->listen_fd = -1;
    pthread_mutex_unlock(&server->board_mutex);

    if (listen_to_close >= 0) {
        shutdown(listen_to_close, SHUT_RDWR);
        close(listen_to_close);
    }

    /* Wait for every detached client-handler thread to finish its cleanup
     * section.  Shutting down their sockets above unblocks them; this
     * ensures we do not free the board or destroy the mutex out from
     * under them. */
    active_clients_wait_zero();

    /* Free the board.  No other thread can touch it now. */
    board_free(&server->board);

    pthread_mutex_destroy(&server->board_mutex);
}
