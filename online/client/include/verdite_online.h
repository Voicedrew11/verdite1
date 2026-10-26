#ifndef VERDITE_ONLINE_H
#define VERDITE_ONLINE_H
/* verdite-online: a room of peers joined through the Verdite online service, and packets between
 * them over WebRTC data channels relayed by Cloudflare TURN (online/protocol/PROTOCOL.md).
 *
 * DRAFT for agreement: the interface only, nothing implements it yet.
 *
 * Plain C, so the C++ ports link it and the C# builds call it through P/Invoke. Knows no game:
 * the game names itself, its build and its room, and gives meaning to the bytes it sends.
 *
 * Threads: every function is called from one thread, the game's. The library runs its own
 * (signaling, ICE, SCTP) and hands what they see to the game as events, read with vo_poll. */
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
} vo_config;

/* Starts joining the room; the copy of config is the session's. NULL when config is malformed;
 * everything that can fail later (the network, the service) arrives as VO_EVENT_ERROR. */
vo_session *vo_open(const vo_config *config);
/* Leaves the room and closes every connection; the session is gone. NULL is ignored. */
void vo_close(vo_session *session);

typedef uint64_t vo_peer; /* the service's peer id, 16 hex digits as a number; 0 is no peer */

typedef enum vo_channel {
    VO_CHANNEL_CONTROL = 0, /* reliable and ordered: events, damage, area changes */
    VO_CHANNEL_STATE = 1,   /* unreliable and unordered: poses, the newest of which wins */
} vo_channel;

typedef enum vo_event_kind {
    VO_EVENT_JOINED,       /* in the room as event.peer; PEER_OPEN for each other peer follows */
    VO_EVENT_PEER_OPEN,    /* event.peer's channels are open: it can be sent to */
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
    VO_ERROR_PEER_FAILED,    /* not fatal: a connection to event.peer failed (PEER_CLOSED follows) */
} vo_error;

typedef struct vo_event {
    vo_event_kind kind;
    vo_peer peer;
    vo_channel channel;
    vo_error error;
    const uint8_t *data; /* PACKET: valid until the next vo_poll or vo_close */
    size_t size;
} vo_event;

/* The next event, oldest first; 0 when there is none. Call it each frame until it returns 0. */
int vo_poll(vo_session *session, vo_event *event);

/* At most VO_PACKET_BYTES, so a STATE packet fits one datagram through TURN. */
enum { VO_PACKET_BYTES = 1100 };
/* Queues data for peer; 0 when the peer is not open or data is too big. CONTROL packets arrive
 * in order, once each, unless the peer closes; STATE packets may not arrive at all. */
int vo_send(vo_session *session, vo_peer peer, vo_channel channel, const uint8_t *data, size_t size);
/* vo_send to every open peer. */
void vo_broadcast(vo_session *session, vo_channel channel, const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* VERDITE_ONLINE_H */
