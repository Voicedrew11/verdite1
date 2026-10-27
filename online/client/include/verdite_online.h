#ifndef VERDITE_ONLINE_H
#define VERDITE_ONLINE_H
/* verdite-online: a room of peers joined through the Verdite online service, and packets between
 * them over WebRTC data channels relayed by Cloudflare TURN (online/protocol/PROTOCOL.md).
 *
 * Plain C, so the C++ ports link it and the C# builds call it through P/Invoke. Knows no game:
 * the game names itself, its build and its room, and gives meaning to the bytes it sends.
 *
 * Threads: every function is called from one thread, the game's. The library runs its own
 * (signaling, ICE, SCTP) and hands what they see to the game as events, read with vo_poll.
 *
 * TURN credentials are the library's business: it asks for one when it first has a peer to
 * connect to, and for a new one halfway through its life, and moves each connection to a new one
 * made with it. A move loses and reorders no CONTROL packet and is not an event: sessions have no
 * length limit. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vo_session vo_session;

typedef struct vo_config {
    const char *service_url; /* "https://online.example.net"; http:// for a local wrangler dev */
    const char *game;        /* [a-z0-9]{1,16}: "kf1" */
    const char *build;       /* peers in a room must have the same: "kf1-0.1.0-ab12cd3" */
    const char *room;        /* [A-Za-z0-9_-]{1,64} */
    const char *listed_as;   /* NULL: an unlisted room. Else the host's name ([A-Za-z0-9 _-]{1,16}),
                              * and the room is listed for its game and build while the first
                              * peer in it is this one; only a public code (P and 7 more) */
} vo_config;

/* Starts joining the room; the copy of config is the session's. NULL when config is malformed;
 * everything that can fail later (the network, the service) arrives as VO_EVENT_ERROR. */
vo_session *vo_open(const vo_config *config);
/* Leaves the room and closes every connection without waiting for the peers to acknowledge (it
 * waits up to 2 seconds for its own threads to let go of them); the session is gone. NULL is
 * ignored. */
void vo_close(vo_session *session);

typedef uint64_t vo_peer; /* the service's peer id, 16 hex digits as a number; 0 is no peer */

typedef enum vo_channel {
    VO_CHANNEL_CONTROL = 0, /* reliable and ordered: events, damage, area changes */
    VO_CHANNEL_STATE = 1,   /* unreliable and unordered: poses, the newest of which wins */
} vo_channel;

typedef enum vo_event_kind {
    VO_EVENT_JOINED,       /* in the room as event.peer; PEER_OPEN for each other peer follows.
                            * event.older says whether anyone was in the room before you */
    VO_EVENT_PEER_OPEN,    /* event.peer's channels are open: it can be sent to. event.older
                            * says whether it joined the room before you */
    VO_EVENT_PEER_CLOSED,  /* event.peer left or its connection failed; never sent to again */
    VO_EVENT_PACKET,       /* event.peer sent event.data on event.channel */
    VO_EVENT_ERROR,        /* event.error; after a fatal one, the session only waits to be closed */
} vo_event_kind;

typedef enum vo_error {
    VO_ERROR_NONE = 0,
    VO_ERROR_UNREACHABLE,    /* fatal: the service cannot be reached */
    VO_ERROR_FULL,           /* fatal: the room has its most peers */
    VO_ERROR_BUILD_MISMATCH, /* fatal: the room's peers have another build */
    VO_ERROR_UNAVAILABLE,    /* fatal: the service has no TURN credential to give */
    VO_ERROR_PROTOCOL,       /* fatal: the service speaks another protocol version */
    VO_ERROR_DISCONNECTED,   /* fatal: the signaling connection dropped after joining */
    VO_ERROR_PEER_FAILED,    /* not fatal: a connection to event.peer failed, or its CONTROL
                              * queue passed VO_CONTROL_QUEUE_BYTES (PEER_CLOSED follows if it
                              * was open) */
} vo_error;

typedef struct vo_event {
    vo_event_kind kind;
    vo_peer peer;
    vo_channel channel;
    vo_error error;
    int older;           /* JOINED: nonzero when someone was in the room first; PEER_OPEN: nonzero
                          * when event.peer joined the room before you */
    const char *message; /* ERROR: what went wrong, for the log; valid as data is */
    const uint8_t *data; /* PACKET: valid until the next vo_poll or vo_close */
    size_t size;
} vo_event;

/* The next event, oldest first; 0 when there is none. Call it each frame until it returns 0. */
int vo_poll(vo_session *session, vo_event *event);

enum {
    VO_STATE_BYTES = 1100,           /* a STATE packet's most: one datagram through TURN */
    VO_CONTROL_BYTES = 65536,        /* a CONTROL packet's most: a world's state on joining */
    VO_CONTROL_QUEUE_BYTES = 1 << 20 /* CONTROL bytes queued for a peer before it is failed */
};
/* Queues data for peer; 0 when the peer is not open or data is too big for its channel. CONTROL
 * packets arrive in order, once each, unless the peer closes; STATE packets may not arrive at
 * all, and are dropped rather than queued behind a slow peer. */
int vo_send(vo_session *session, vo_peer peer, vo_channel channel, const uint8_t *data, size_t size);
/* vo_send to every open peer. */
void vo_broadcast(vo_session *session, vo_channel channel, const uint8_t *data, size_t size);

typedef struct vo_link {
    int open;                     /* zero when the peer is not open, and the rest is zero too */
    uint32_t round_trip_ms;       /* the latest measured; 0 before the first */
    size_t control_queued_bytes;  /* queued but not yet sent, to slow down before failing */
} vo_link;
/* How the connection to peer is doing. */
vo_link vo_link_of(const vo_session *session, vo_peer peer);

/* A listed room's floor, as its list row shows it; ignored unless the session's config had
 * listed_as and it is the room's first peer. Sent at most once a second, the latest winning. */
void vo_describe(vo_session *session, uint32_t floor);

/* The public rooms of one game and build, kept up to date by the service while open. */
typedef struct vo_lobby vo_lobby;

typedef enum vo_lobby_state {
    VO_LOBBY_CONNECTING,
    VO_LOBBY_LISTING,  /* vo_lobby_rooms has the service's latest list */
    VO_LOBBY_FAILED,   /* the service cannot be reached or refused; close and open again */
} vo_lobby_state;

typedef struct vo_listed_room {
    char room[65];   /* NUL-terminated, as the service sent it */
    char host[17];
    uint32_t floor;
    uint32_t players;
    uint32_t most;
} vo_listed_room;

/* Starts listening; NULL when a name is malformed. service_url, game and build as in vo_config. */
vo_lobby *vo_lobby_open(const char *service_url, const char *game, const char *build);
/* NULL is ignored. */
void vo_lobby_close(vo_lobby *lobby);
vo_lobby_state vo_lobby_state_of(const vo_lobby *lobby);
/* Copies up to most rows of the latest list, newest room first, and returns how many it copied.
 * changed is set nonzero when the list differs from the one the last call copied. */
size_t vo_lobby_rooms(vo_lobby *lobby, vo_listed_room *rooms, size_t most, int *changed);

#ifdef __cplusplus
}
#endif

#endif /* VERDITE_ONLINE_H */
