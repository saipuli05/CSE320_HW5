/*
 * Snake server entry point.
 *
 * Per the assignment rules, this file contains ONLY #includes, local
 * #defines, and the main() function - no helper functions of any kind.
 *
 * main() parses command-line arguments via getopt, validates them,
 * initialises the server (which itself installs the SIGINT/SIGTERM/
 * SIGPIPE handlers), runs the accept loop, and tears the server down.
 */

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "global.h"
#include "server.h"

int main(int argc, char *argv[]) {
    int port          = -1;
    int board_size    = BOARD_SIZE_DEFAULT;
    int max_players   = MAX_PLAYERS_DEFAULT;
    unsigned int seed = (unsigned int)time(NULL);

    int opt;
    while ((opt = getopt(argc, argv, "p:b:s:m:h")) != -1) {
        switch (opt) {
        case 'p': {
            char *end = NULL;
            errno = 0;
            long v = strtol(optarg, &end, 10);
            if (errno != 0 || end == optarg || *end != '\0' ||
                v <= 0 || v > 65535) {
                ERR_INVALID_PORT(optarg);
                return EXIT_FAILURE;
            }
            port = (int)v;
            break;
        }
        case 'b': {
            char *end = NULL;
            errno = 0;
            long v = strtol(optarg, &end, 10);
            if (errno != 0 || end == optarg || *end != '\0' ||
                v < BOARD_SIZE_MIN || v > BOARD_SIZE_MAX) {
                ERR_INVALID_BOARD_SIZE((int)v);
                return EXIT_FAILURE;
            }
            board_size = (int)v;
            break;
        }
        case 's': {
            char *end = NULL;
            errno = 0;
            long v = strtol(optarg, &end, 10);
            if (errno != 0 || end == optarg || *end != '\0') {
                ERR_MSG("Invalid seed: %s", optarg);
                return EXIT_FAILURE;
            }
            seed = (unsigned int)v;
            break;
        }
        case 'm': {
            char *end = NULL;
            errno = 0;
            long v = strtol(optarg, &end, 10);
            if (errno != 0 || end == optarg || *end != '\0' ||
                v < MAX_PLAYERS_MIN || v > MAX_PLAYERS_MAX) {
                ERR_INVALID_MAX_PLAYERS((int)v);
                return EXIT_FAILURE;
            }
            max_players = (int)v;
            break;
        }
        case 'h':
            PRINT_USAGE();
            return EXIT_SUCCESS;
        case '?':
        default:
            PRINT_USAGE();
            return EXIT_FAILURE;
        }
    }

    if (port == -1) {
        ERR_PORT_REQUIRED();
        return EXIT_FAILURE;
    }

    server_t server;
    if (server_init(&server, port, board_size, max_players, seed) != 0) {
        ERR_BIND_FAILED(port);
        return EXIT_FAILURE;
    }

    PRINT_SERVER_STARTED(port, board_size, max_players);

    int rc = server_start(&server);
    server_cleanup(&server);
    return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
