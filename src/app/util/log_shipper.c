#include "log_shipper.h"

#include <SDL.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/** One datagram per line, so a saturated queue drops oldest-newest rather than blocking. */
#define SHIP_QUEUE_SLOTS 128
#define SHIP_LINE_MAX 512
#define SHIP_DEFAULT_PORT 5514
/** Report lost lines after this many, so a flood is visible without becoming one. */
#define SHIP_DROP_REPORT 256

#ifndef AURORA_LOG_SHIP_TARGET
#define AURORA_LOG_SHIP_TARGET ""
#endif

/* Index order matches commons_log_level. */
static const char LEVEL_LETTERS[] = {'F', 'E', 'W', 'I', 'D', 'V'};

static char ship_queue[SHIP_QUEUE_SLOTS][SHIP_LINE_MAX];
static int ship_head = 0;
static int ship_count = 0;

static SDL_mutex *ship_lock = NULL;
static SDL_cond *ship_wakeup = NULL;
static SDL_Thread *ship_thread = NULL;
static volatile bool ship_running = false;
static bool ship_active = false;

static int ship_socket = -1;
static struct sockaddr_in ship_target;
static struct sockaddr_in ship_broadcast;
static bool ship_has_target = false;
static bool ship_has_broadcast = false;
static unsigned int ship_sent = 0;
static unsigned int ship_dropped = 0;

/**
 * Set on the writer thread only. Lines produced there (a drop report, a socket error)
 * must not be shipped, or the listener would feed the queue it is draining.
 */
static __thread bool ship_quiet_thread = false;

static void ship_queue_line(const char *line);

static int ship_writer_thread(void *userdata);

/**
 * Split `host` / `host:port` into an address. Only IPv4 is supported: this is a
 * debug sink on a LAN, and a name is resolved once at startup, when a blocking
 * lookup is still cheap.
 */
static bool ship_parse_address(const char *spec, size_t spec_len, struct sockaddr_in *out) {
    char host[128];
    int port = SHIP_DEFAULT_PORT;
    const char *colon = memchr(spec, ':', spec_len);
    size_t host_len = colon != NULL ? (size_t) (colon - spec) : spec_len;
    if (host_len == 0 || host_len >= sizeof(host)) {
        return false;
    }
    memcpy(host, spec, host_len);
    host[host_len] = '\0';
    if (colon != NULL && colon[1] != '\0') {
        port = atoi(colon + 1);
        if (port <= 0 || port > 65535) {
            return false;
        }
    }

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons((unsigned short) port);
    if (inet_pton(AF_INET, host, &out->sin_addr) == 1) {
        return true;
    }
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *resolved = NULL;
    if (getaddrinfo(host, NULL, &hints, &resolved) != 0 || resolved == NULL) {
        if (resolved != NULL) {
            freeaddrinfo(resolved);
        }
        return false;
    }
    memcpy(&out->sin_addr, &((struct sockaddr_in *) resolved->ai_addr)->sin_addr, sizeof(struct in_addr));
    freeaddrinfo(resolved);
    return true;
}

static void ship_start(void) {
    const char *spec = AURORA_LOG_SHIP_TARGET;
    if (spec[0] == '\0') {
        /* Built without a sink; keep the writer's cost at one bool test. */
        return;
    }
    if (ship_active) {
        return;
    }

    ship_has_target = ship_parse_address(spec, strlen(spec), &ship_target);
    memset(&ship_broadcast, 0, sizeof(ship_broadcast));
    ship_broadcast.sin_family = AF_INET;
    ship_broadcast.sin_port = ship_target.sin_port;
    ship_broadcast.sin_addr.s_addr = INADDR_BROADCAST;
    ship_has_broadcast = true;

    if (!ship_has_target) {
        commons_log_warn("LogShip", "Target '%s' did not resolve; log shipping stays off", spec);
        return;
    }

    ship_socket = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (ship_socket < 0) {
        commons_log_warn("LogShip", "No UDP socket for log shipping: %s", strerror(errno));
        return;
    }
    int on = 1;
    setsockopt(ship_socket, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    if (ship_lock == NULL) {
        ship_lock = SDL_CreateMutex();
    }
    if (ship_wakeup == NULL) {
        ship_wakeup = SDL_CreateCond();
    }
    ship_head = 0;
    ship_count = 0;
    ship_sent = 0;
    ship_dropped = 0;
    ship_running = true;
    ship_thread = SDL_CreateThread(ship_writer_thread, "AuroraLogShip", NULL);
    if (ship_thread == NULL) {
        commons_log_warn("LogShip", "Writer thread failed: %s", SDL_GetThreadName(NULL));
        ship_running = false;
        close(ship_socket);
        ship_socket = -1;
        return;
    }
    ship_active = true;
    commons_log_info("LogShip", "Shipping %s (udp) and 255.255.255.255:%d logs", spec,
                     ntohs(ship_target.sin_port));
    commons_log_info("LogShip", "Aurora " APP_VERSION " hello from the TV: log shipper is live");
}

bool log_shipper_has_target(void) {
    return AURORA_LOG_SHIP_TARGET[0] != '\0';
}

const char *log_shipper_target(void) {
    return AURORA_LOG_SHIP_TARGET;
}

bool log_shipper_is_enabled(void) {
    return ship_active;
}

void log_shipper_set_enabled(bool enabled) {
    if (enabled) {
        ship_start();
    } else {
        log_shipper_deinit();
    }
}

void log_shipper_deinit(void) {
    if (!ship_active) {
        return;
    }
    ship_running = false;
    if (ship_lock != NULL) {
        SDL_LockMutex(ship_lock);
        SDL_CondBroadcast(ship_wakeup);
        SDL_UnlockMutex(ship_lock);
    }
    if (ship_thread != NULL) {
        SDL_WaitThread(ship_thread, NULL);
        ship_thread = NULL;
    }
    if (ship_socket >= 0) {
        close(ship_socket);
        ship_socket = -1;
    }
    ship_active = false;
    commons_log_info("LogShip", "Stopped: %u line(s) sent, %u dropped", ship_sent, ship_dropped);
}

void log_shipper_write(commons_log_level level, const char *tag, const char *message) {
    if (!ship_active || ship_quiet_thread || message == NULL) {
        return;
    }
    if (level > COMMONS_LOG_LEVEL_INFO) {
        /* DEBUG and VERBOSE are per-event on a running stream; shipping them would
         * only fill the queue and hide the lines this sink exists to catch. */
        return;
    }
    char line[SHIP_LINE_MAX];
    snprintf(line, sizeof(line), "%c %10" SDL_PRIu32 " [%s] %s",
             LEVEL_LETTERS[level <= COMMONS_LOG_LEVEL_VERBOSE ? level : COMMONS_LOG_LEVEL_VERBOSE],
             SDL_GetTicks(), tag != NULL ? tag : "-", message);
    ship_queue_line(line);
}

/** Caller-provided text; used by the writer thread too, so it must not log. */
static void ship_queue_line(const char *line) {
    if (ship_lock == NULL) {
        return;
    }
    SDL_LockMutex(ship_lock);
    if (ship_count >= SHIP_QUEUE_SLOTS) {
        ship_dropped++;
        SDL_UnlockMutex(ship_lock);
        return;
    }
    int slot = (ship_head + ship_count) % SHIP_QUEUE_SLOTS;
    snprintf(ship_queue[slot], SHIP_LINE_MAX, "%s\n", line);
    ship_count++;
    SDL_CondSignal(ship_wakeup);
    SDL_UnlockMutex(ship_lock);
}

static int ship_writer_thread(void *userdata) {
    (void) userdata;
    ship_quiet_thread = true;
    while (ship_running) {
        char line[SHIP_LINE_MAX];
        bool have_line = false;

        SDL_LockMutex(ship_lock);
        while (ship_count == 0 && ship_running) {
            SDL_CondWait(ship_wakeup, ship_lock);
        }
        if (ship_count > 0) {
            memcpy(line, ship_queue[ship_head], SHIP_LINE_MAX);
            ship_head = (ship_head + 1) % SHIP_QUEUE_SLOTS;
            ship_count--;
            have_line = true;
            ship_sent++;
        }
        SDL_UnlockMutex(ship_lock);

        if (!have_line) {
            continue;
        }
        size_t len = strlen(line);
        if (ship_has_target && ship_socket >= 0) {
            sendto(ship_socket, line, len, 0, (struct sockaddr *) &ship_target, sizeof(ship_target));
        }
        if (ship_has_broadcast && ship_socket >= 0) {
            sendto(ship_socket, line, len, 0, (struct sockaddr *) &ship_broadcast, sizeof(ship_broadcast));
        }

        SDL_LockMutex(ship_lock);
        unsigned int lost = ship_dropped;
        SDL_UnlockMutex(ship_lock);
        if (lost >= SHIP_DROP_REPORT) {
            char notice[SHIP_LINE_MAX];
            snprintf(notice, sizeof(notice), "W %10" SDL_PRIu32 " [LogShip] %u queued line(s) dropped\n",
                     SDL_GetTicks(), lost);
            ship_dropped = 0;
            ship_queue_line(notice);
        }
    }
    return 0;
}
