// verdite_online.h over libdatachannel. libdatachannel calls back on its own threads; every
// callback only posts what it saw to the session's mailbox, and vo_poll acts on it on the game's
// thread, so the session's state has one thread and needs no lock.
//
// A connection lives on the TURN credentials it was made with, and dies at the relay's next
// refresh once one of them is revoked or expires. So each time a side renews its credential, the
// pair moves to a new connection (the next generation) made with the new one, and the old one is
// closed once the handover (handover.h) says nothing is left on it. The game sees none of it.

#include "verdite_online.h"

#include "handover.h"
#include "signaling.h"

#include <rtc/rtc.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using vo::Handover;
using vo::PeerId;
using vo::Signal;

constexpr std::size_t state_queue_bytes = std::size_t{16} * 1024; // a STATE packet behind this is dropped
constexpr auto turn_retry_delay = std::chrono::seconds(10);
constexpr auto renew_margin_most = std::chrono::minutes(15);
constexpr auto move_timeout = std::chrono::seconds(30); // a next connection not open by then is dropped
constexpr std::string_view moved_marker = "moved";
constexpr std::string_view seen_marker = "seen";

// What a libdatachannel thread saw.
struct Happening {
    enum class Kind {
        SocketText,
        SocketClosed,
        SocketError,
        LocalDescription,
        LocalCandidate,
        ConnectionState,
        ChannelOpen,
        ChannelClosed,
        ChannelText,
        Packet,
    };
    Kind kind = Kind::SocketClosed;
    PeerId peer = 0;
    std::uint32_t generation = 0;
    vo_channel channel = VO_CHANNEL_CONTROL;
    std::string text; // SocketText, SocketError, ChannelText; the SDP or candidate
    std::string mid_or_type;
    rtc::PeerConnection::State state = rtc::PeerConnection::State::New;
    rtc::binary bytes;
};

class Mailbox {
public:
    // On a libdatachannel thread.
    void post(Happening happening) {
        const std::scoped_lock lock(mutex_);
        happenings_.push_back(std::move(happening));
    }
    std::deque<Happening> take() {
        const std::scoped_lock lock(mutex_);
        return std::exchange(happenings_, {});
    }

private:
    std::mutex mutex_;
    std::deque<Happening> happenings_;
};

Happening seen(Happening::Kind kind, PeerId peer = 0, std::uint32_t generation = 0) {
    Happening happening;
    happening.kind = kind;
    happening.peer = peer;
    happening.generation = generation;
    return happening;
}

// Callbacks hold the mailbox weakly, so one that fires after vo_close finds it gone.
using MailboxRef = std::weak_ptr<Mailbox>;

void post(const MailboxRef &mailbox, Happening happening) {
    if (const auto box = mailbox.lock())
        box->post(std::move(happening));
}

// An event the game has not read yet, owning its bytes.
struct Event {
    vo_event_kind kind = VO_EVENT_ERROR;
    PeerId peer = 0;
    vo_channel channel = VO_CHANNEL_CONTROL;
    vo_error error = VO_ERROR_NONE;
    bool older = false;
    std::string message;
    rtc::binary bytes;
};

Event event_of(vo_event_kind kind, PeerId peer) {
    Event event;
    event.kind = kind;
    event.peer = peer;
    return event;
}

Event error_event(vo_error error, PeerId peer, std::string message) {
    Event event = event_of(VO_EVENT_ERROR, peer);
    event.error = error;
    event.message = std::move(message);
    return event;
}

Event packet_event(PeerId peer, vo_channel channel, rtc::binary bytes) {
    Event event = event_of(VO_EVENT_PACKET, peer);
    event.channel = channel;
    event.bytes = std::move(bytes);
    return event;
}

// One generation of the connection to a peer.
struct Connection {
    std::uint32_t generation = 0;
    std::uint32_t credential = 0; // which of our TURN credentials it was made with
    std::shared_ptr<rtc::PeerConnection> peer_connection; // none until we hold a credential
    std::array<std::shared_ptr<rtc::DataChannel>, 2> channels;
    std::array<bool, 2> channel_open{};
    bool remote_described = false;
    std::vector<Signal> waiting; // signals that came before they could be applied

    bool open() const {
        return channel_open[VO_CHANNEL_CONTROL] && channel_open[VO_CHANNEL_STATE];
    }
};

// A move from a peer's current connection to the next generation.
struct Move {
    Connection next;
    Handover handover;
    Clock::time_point deadline; // dropped if not open by then, unless already sending on it
};

// The connection to one other peer in the room.
struct Link {
    bool older = false;     // joined before us: we offer to it
    bool announced = false; // PEER_OPEN was sent
    Connection current;
    std::optional<Move> move;
    std::optional<Clock::time_point> retry_move_at;
    std::uint32_t newest_generation = 0; // never reused, so a gone one's late news is ignored
    std::vector<Event> early; // packets that came before PEER_OPEN

    Connection *connection_of(std::uint32_t generation) {
        if (current.generation == generation)
            return &current;
        if (move && move->next.generation == generation)
            return &move->next;
        return nullptr;
    }
    const Connection &sending() const {
        if (move && move->handover.sending_on_new())
            return move->next;
        return current;
    }
};

std::size_t packet_limit(vo_channel channel) {
    return channel == VO_CHANNEL_STATE ? VO_STATE_BYTES : VO_CONTROL_BYTES;
}

std::optional<vo_error> fatal_error(std::string_view code) {
    if (code == "full")
        return VO_ERROR_FULL;
    if (code == "build_mismatch")
        return VO_ERROR_BUILD_MISMATCH;
    if (code == "unavailable")
        return VO_ERROR_UNAVAILABLE;
    if (code == "protocol")
        return VO_ERROR_PROTOCOL;
    return std::nullopt;
}

std::vector<rtc::IceServer> ice_servers_of(const std::vector<vo::IceServer> &servers) {
    std::vector<rtc::IceServer> out;
    for (const auto &server : servers) {
        try {
            rtc::IceServer ice(server.url);
            ice.username = server.username;
            ice.password = server.credential;
            out.push_back(std::move(ice));
        } catch (const std::exception &error) {
            std::fprintf(stderr, "[ONLINE] skipping ICE server %s: %s\n", server.url.c_str(),
                         error.what());
        }
    }
    return out;
}

// A connection being closed, kept until libdatachannel says it is: destroying its data channels
// while libdatachannel's thread still closes them races inside libdatachannel. Its callbacks stay
// too, for the same reason; what they post about a generation that is gone is ignored.
struct Retired {
    PeerId peer = 0;
    Connection connection;
};

constexpr auto close_wait = std::chrono::seconds(2); // the longest vo_close waits for that

// A handover marker, sent as text so it never mixes with the game's binary packets.
void send_marker(Connection &connection, std::string_view marker) {
    connection.channels[VO_CHANNEL_CONTROL]->send(std::string(marker));
}

} // namespace

struct vo_session {
    std::string url;
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    std::shared_ptr<rtc::WebSocket> socket;
    PeerId self = 0;
    std::map<PeerId, Link> links;

    std::vector<vo::IceServer> ice_servers; // the credential in hand, for new connections
    std::uint32_t credential = 0;           // how many we have had
    std::optional<Clock::time_point> renew_at; // when it is replaced; none before the first
    std::optional<Clock::time_point> retry_turn_at;
    bool turn_asked = false;

    std::vector<Retired> retired;
    bool fatal = false;
    std::deque<Event> events;
    Event current; // the event vo_poll last returned; its bytes are the game's until the next

    void retire(PeerId peer, Connection connection) {
        if (!connection.peer_connection)
            return;
        connection.peer_connection->close();
        // already closed: libdatachannel has finished with it
        if (connection.peer_connection->state() != rtc::PeerConnection::State::Closed)
            retired.push_back({peer, std::move(connection)});
    }

    void retire_link(PeerId peer, Link &link) {
        retire(peer, std::move(link.current));
        if (link.move)
            retire(peer, std::move(link.move->next));
    }

    void forget_if_closed(const Happening &happening) {
        if (happening.kind != Happening::Kind::ConnectionState ||
            happening.state != rtc::PeerConnection::State::Closed)
            return;
        std::erase_if(retired, [&](const Retired &each) {
            return each.peer == happening.peer && each.connection.generation == happening.generation;
        });
    }

    void push(Event event) {
        if (!fatal)
            events.push_back(std::move(event));
    }

    void fail(vo_error error, std::string message) {
        if (fatal)
            return;
        push(error_event(error, 0, std::move(message)));
        fatal = true;
        for (auto &[peer, link] : links)
            retire_link(peer, link);
        links.clear();
        if (socket)
            socket->close();
    }

    void send_frame(const std::string &frame) {
        try {
            if (socket && socket->isOpen())
                socket->send(frame);
        } catch (const std::exception &error) {
            std::fprintf(stderr, "[ONLINE] signaling send: %s\n", error.what());
        }
    }

    void send_signal(PeerId peer, const Signal &signal) {
        send_frame(vo::signal_frame(peer, signal));
    }

    void ask_for_turn() {
        if (turn_asked)
            return;
        turn_asked = true;
        send_frame(vo::turn_frame());
    }

    bool credential_fresh() const {
        return renew_at && Clock::now() < *renew_at;
    }

    void drop_peer(PeerId peer, std::optional<std::string> failure) {
        const auto it = links.find(peer);
        if (it == links.end())
            return;
        if (failure)
            push(error_event(VO_ERROR_PEER_FAILED, peer, std::move(*failure)));
        if (it->second.announced)
            push(event_of(VO_EVENT_PEER_CLOSED, peer));
        retire_link(peer, it->second);
        links.erase(it);
    }

    // ---- connections --------------------------------------------------------------------------

    void watch_channel(PeerId peer, std::uint32_t generation, vo_channel channel,
                       rtc::DataChannel &data_channel) {
        const MailboxRef box = mailbox;
        data_channel.onOpen([box, peer, generation, channel] {
            Happening happening = seen(Happening::Kind::ChannelOpen, peer, generation);
            happening.channel = channel;
            post(box, std::move(happening));
        });
        data_channel.onClosed([box, peer, generation] {
            post(box, seen(Happening::Kind::ChannelClosed, peer, generation));
        });
        data_channel.onMessage(
            [box, peer, generation, channel](rtc::binary bytes) {
                Happening happening = seen(Happening::Kind::Packet, peer, generation);
                happening.channel = channel;
                happening.bytes = std::move(bytes);
                post(box, std::move(happening));
            },
            [box, peer, generation](rtc::string text) {
                Happening happening = seen(Happening::Kind::ChannelText, peer, generation);
                happening.text = std::move(text);
                post(box, std::move(happening));
            });
    }

    void watch_connection(PeerId peer, std::uint32_t generation, rtc::PeerConnection &connection) {
        const MailboxRef box = mailbox;
        connection.onLocalDescription([box, peer, generation](const rtc::Description &description) {
            Happening happening = seen(Happening::Kind::LocalDescription, peer, generation);
            happening.text = std::string(description);
            happening.mid_or_type = description.typeString();
            post(box, std::move(happening));
        });
        connection.onLocalCandidate([box, peer, generation](const rtc::Candidate &candidate) {
            Happening happening = seen(Happening::Kind::LocalCandidate, peer, generation);
            happening.text = candidate.candidate();
            happening.mid_or_type = candidate.mid();
            post(box, std::move(happening));
        });
        connection.onStateChange([box, peer, generation](rtc::PeerConnection::State state) {
            Happening happening = seen(Happening::Kind::ConnectionState, peer, generation);
            happening.state = state;
            post(box, std::move(happening));
        });
    }

    void connect(PeerId peer, bool offer, Connection &connection) {
        rtc::Configuration config;
        config.iceServers = ice_servers_of(ice_servers);
        // Relay only, so no peer learns another's address; only when the service sent no servers
        // at all (a local service without a TURN key) host candidates, for one machine.
        config.iceTransportPolicy = ice_servers.empty() ? rtc::TransportPolicy::All
                                                        : rtc::TransportPolicy::Relay;
        config.disableAutoNegotiation = true; // the later peer offers, nobody else
        config.maxMessageSize = VO_CONTROL_BYTES;
        connection.credential = credential;
        connection.peer_connection = std::make_shared<rtc::PeerConnection>(config);
        watch_connection(peer, connection.generation, *connection.peer_connection);

        rtc::DataChannelInit control{.negotiated = true, .id = 0};
        rtc::DataChannelInit state{.negotiated = true, .id = 1};
        state.reliability.unordered = true;
        state.reliability.maxRetransmits = 0;
        auto &channels = connection.channels;
        channels[VO_CHANNEL_CONTROL] = connection.peer_connection->createDataChannel("control", control);
        channels[VO_CHANNEL_STATE] = connection.peer_connection->createDataChannel("state", state);
        watch_channel(peer, connection.generation, VO_CHANNEL_CONTROL, *channels[VO_CHANNEL_CONTROL]);
        watch_channel(peer, connection.generation, VO_CHANNEL_STATE, *channels[VO_CHANNEL_STATE]);

        if (offer)
            connection.peer_connection->setLocalDescription(rtc::Description::Type::Offer);
        for (auto &signal : std::exchange(connection.waiting, {}))
            apply(connection, std::move(signal));
    }

    // Applies a peer's offer, answer or candidate, or keeps it until it can be applied.
    void apply(Connection &connection, Signal signal) {
        const bool candidate = signal.kind == Signal::Kind::Candidate;
        if (!connection.peer_connection || (candidate && !connection.remote_described)) {
            connection.waiting.push_back(std::move(signal));
            return;
        }
        auto &peer_connection = *connection.peer_connection;
        switch (signal.kind) {
        case Signal::Kind::Offer:
        case Signal::Kind::Answer:
            if (connection.remote_described)
                return;
            break;
        case Signal::Kind::Candidate:
            peer_connection.addRemoteCandidate(rtc::Candidate(signal.candidate, signal.mid));
            return;
        case Signal::Kind::Restart:
            return;
        }
        const bool offer = signal.kind == Signal::Kind::Offer;
        peer_connection.setRemoteDescription(rtc::Description(signal.sdp, offer ? "offer" : "answer"));
        if (offer)
            peer_connection.setLocalDescription(rtc::Description::Type::Answer);
        connection.remote_described = true;
        for (auto &waiting : std::exchange(connection.waiting, {}))
            apply(connection, std::move(waiting));
    }

    void connect_waiting() {
        std::vector<std::pair<PeerId, std::string>> failed;
        for (auto &[peer, link] : links) {
            if (link.current.peer_connection)
                continue;
            try {
                connect(peer, link.older, link.current);
            } catch (const std::exception &error) {
                failed.emplace_back(peer, std::string("connecting: ") + error.what());
            }
        }
        for (auto &[peer, why] : failed)
            drop_peer(peer, std::move(why));
    }

    // ---- moving to a new credential -----------------------------------------------------------

    void begin_move(PeerId peer, Link &link, std::uint32_t generation) {
        link.newest_generation = generation;
        Move &move = link.move.emplace();
        move.next.generation = generation;
        move.deadline = Clock::now() + move_timeout;
        link.retry_move_at.reset();
        connect(peer, link.older, move.next);
    }

    // Gives up a move nothing has been sent on yet; it is tried again later.
    void abandon_move(PeerId peer, Link &link, const std::string &why) {
        std::fprintf(stderr, "[ONLINE] %s: move abandoned: %s\n", vo::peer_hex(peer).c_str(), why.c_str());
        if (link.move)
            retire(peer, std::move(link.move->next));
        link.move.reset();
        link.retry_move_at = Clock::now() + turn_retry_delay;
    }

    void finish_move(PeerId peer, Link &link) {
        if (!link.move || !link.move->handover.finished())
            return;
        retire(peer, std::move(link.current));
        link.current = std::move(link.move->next);
        link.move.reset();
        std::fprintf(stderr, "[ONLINE] %s: moved to generation %u\n", vo::peer_hex(peer).c_str(),
                     link.current.generation);
    }

    // A peer whose connection is older than our credential moves: we offer the next generation, or
    // ask the peer to, since the later peer always offers.
    void move_if_due(PeerId peer, Link &link, Clock::time_point now) {
        if (link.move) {
            if (!link.move->handover.sending_on_new() && now >= link.move->deadline)
                abandon_move(peer, link, "not open in time");
            return;
        }
        if (!link.announced || link.current.credential >= credential || !credential_fresh())
            return;
        if (link.retry_move_at && now < *link.retry_move_at)
            return;
        const std::uint32_t generation = link.newest_generation + 1;
        if (link.older) {
            begin_move(peer, link, generation);
            return;
        }
        Signal restart;
        restart.kind = Signal::Kind::Restart;
        restart.generation = generation;
        send_signal(peer, restart);
        link.retry_move_at = now + move_timeout;
    }

    // The offerer's signal for a generation after the current one starts our side of a move.
    Connection *answer_move(PeerId peer, Link &link, const Signal &signal) {
        if (link.older || signal.kind != Signal::Kind::Offer || !link.announced ||
            signal.generation <= link.newest_generation)
            return nullptr;
        if (link.move) {
            if (link.move->handover.sending_on_new())
                return nullptr;
            abandon_move(peer, link, "the peer started another");
        }
        begin_move(peer, link, signal.generation);
        return link.move ? &link.move->next : nullptr;
    }

    // ---- the service's frames -----------------------------------------------------------------

    void on_welcome(const vo::ServiceFrame &frame) {
        if (frame.protocol != vo::protocol_version) {
            fail(VO_ERROR_PROTOCOL, "the service speaks protocol " + std::to_string(frame.protocol));
            return;
        }
        self = frame.peer;
        Event joined = event_of(VO_EVENT_JOINED, self);
        joined.older = !frame.peers.empty();
        push(std::move(joined));
        for (const PeerId peer : frame.peers)
            links[peer].older = true;
        if (!links.empty())
            ask_for_turn();
    }

    void on_join(PeerId peer) {
        if (peer == self || links.contains(peer))
            return;
        links[peer].older = false;
        if (credential_fresh())
            connect_waiting();
        else if (!retry_turn_at)
            ask_for_turn();
    }

    void on_signal(PeerId peer, Signal signal) {
        const auto it = links.find(peer);
        if (it == links.end())
            return; // a peer we dropped, or one that left
        Link &link = it->second;
        try {
            if (signal.kind == Signal::Kind::Restart) {
                if (link.older && !link.move && link.announced &&
                    signal.generation > link.current.generation)
                    begin_move(peer, link, std::max(signal.generation, link.newest_generation + 1));
                return;
            }
            Connection *connection = link.connection_of(signal.generation);
            if (!connection)
                connection = answer_move(peer, link, signal);
            if (connection)
                apply(*connection, std::move(signal));
        } catch (const std::exception &error) {
            drop_peer(peer, std::string("bad signal: ") + error.what());
        }
    }

    void on_turn(const vo::ServiceFrame &frame) {
        turn_asked = false;
        std::fprintf(stderr, "[ONLINE] TURN credential for %d s, %zu servers\n", frame.seconds,
                     frame.ice_servers.size());
        retry_turn_at.reset();
        ice_servers = frame.ice_servers;
        ++credential;
        // Renewed halfway, or 15 minutes before the end, so the moves to it finish long before
        // the old one goes.
        const auto lifetime = std::chrono::seconds(frame.seconds);
        renew_at = Clock::now() + lifetime - std::min<Clock::duration>(renew_margin_most, lifetime / 2);
        connect_waiting();
    }

    void on_error(const vo::ServiceFrame &frame) {
        if (const auto error = fatal_error(frame.code)) {
            fail(*error, frame.code + ": " + frame.message);
            return;
        }
        std::fprintf(stderr, "[ONLINE] service: %s: %s\n", frame.code.c_str(), frame.message.c_str());
        // a refused turn: the peer we asked for left meanwhile, or we asked too often
        if (turn_asked && (frame.code == "rate_limited" || frame.code == "alone")) {
            turn_asked = false;
            retry_turn_at = Clock::now() + turn_retry_delay;
        }
    }

    void on_frame(const std::string &text) {
        auto frame = vo::parse_service_frame(text);
        if (!frame) {
            std::fprintf(stderr, "[ONLINE] ignoring a frame the protocol does not have\n");
            return;
        }
        if (self == 0 && frame->kind != vo::ServiceFrame::Kind::Welcome &&
            frame->kind != vo::ServiceFrame::Kind::Error)
            return;
        switch (frame->kind) {
        case vo::ServiceFrame::Kind::Welcome:
            if (self == 0)
                on_welcome(*frame);
            break;
        case vo::ServiceFrame::Kind::Join:
            on_join(frame->peer);
            break;
        case vo::ServiceFrame::Kind::Leave:
            drop_peer(frame->peer, std::nullopt);
            break;
        case vo::ServiceFrame::Kind::Signal:
            on_signal(frame->peer, std::move(frame->signal));
            break;
        case vo::ServiceFrame::Kind::Turn:
            on_turn(*frame);
            break;
        case vo::ServiceFrame::Kind::Error:
            on_error(*frame);
            break;
        }
    }

    // ---- what the threads saw -----------------------------------------------------------------

    void on_channel_open(PeerId peer, std::uint32_t generation, vo_channel channel) {
        const auto it = links.find(peer);
        if (it == links.end())
            return;
        Link &link = it->second;
        Connection *connection = link.connection_of(generation);
        if (!connection)
            return;
        connection->channel_open[channel] = true;
        if (!connection->open())
            return;
        if (connection == &link.current && !link.announced) {
            link.announced = true;
            Event open = event_of(VO_EVENT_PEER_OPEN, peer);
            open.older = link.older;
            push(std::move(open));
            for (auto &packet : std::exchange(link.early, {}))
                push(std::move(packet));
            return;
        }
        if (link.move && connection == &link.move->next && !link.move->handover.sending_on_new()) {
            send_marker(link.current, moved_marker); // after our last packet on the old one
            link.move->handover.sent_moved();
            finish_move(peer, link);
        }
    }

    // A failed or closed connection: the peer is gone, unless it is a move nothing went over yet.
    void on_connection_lost(PeerId peer, std::uint32_t generation, const char *why) {
        const auto it = links.find(peer);
        if (it == links.end())
            return;
        Link &link = it->second;
        if (link.move && link.move->next.generation == generation &&
            !link.move->handover.sending_on_new()) {
            abandon_move(peer, link, why);
            return;
        }
        if (link.move && generation == link.current.generation &&
            link.move->handover.peer_closed_old()) {
            finish_move(peer, link); // the peer finished first and closed the old one
            return;
        }
        if (link.connection_of(generation))
            drop_peer(peer, why);
    }

    void on_marker(PeerId peer, std::uint32_t generation, const std::string &marker) {
        const auto it = links.find(peer);
        if (it == links.end())
            return;
        Link &link = it->second;
        if (!link.move || generation != link.current.generation)
            return;
        Handover &handover = link.move->handover;
        if (marker == moved_marker) {
            for (auto &packet : handover.peer_moved())
                push(packet_event(peer, VO_CHANNEL_CONTROL, std::move(packet)));
            send_marker(link.current, seen_marker);
        } else if (marker == seen_marker) {
            handover.peer_saw_moved();
        }
        finish_move(peer, link);
    }

    void on_packet(Happening &happening) {
        const auto it = links.find(happening.peer);
        if (it == links.end() || happening.bytes.size() > packet_limit(happening.channel))
            return;
        Link &link = it->second;
        const PeerId peer = happening.peer;
        if (!link.announced) {
            if (happening.generation == link.current.generation)
                link.early.push_back(packet_event(peer, happening.channel, std::move(happening.bytes)));
            return;
        }
        if (happening.channel == VO_CHANNEL_STATE || !link.move) {
            if (link.connection_of(happening.generation))
                push(packet_event(peer, happening.channel, std::move(happening.bytes)));
            return;
        }
        Handover::From from = Handover::From::Old;
        if (happening.generation == link.move->next.generation)
            from = Handover::From::New;
        else if (happening.generation != link.current.generation)
            return;
        for (auto &packet : link.move->handover.received(from, std::move(happening.bytes)))
            push(packet_event(peer, VO_CHANNEL_CONTROL, std::move(packet)));
    }

    void on_local_signal(const Happening &happening) {
        const auto it = links.find(happening.peer);
        if (it == links.end() || !it->second.connection_of(happening.generation))
            return;
        Signal signal;
        signal.generation = happening.generation;
        if (happening.kind == Happening::Kind::LocalCandidate) {
            signal.kind = Signal::Kind::Candidate;
            signal.candidate = happening.text;
            signal.mid = happening.mid_or_type;
        } else {
            const bool offer = happening.mid_or_type == "offer";
            signal.kind = offer ? Signal::Kind::Offer : Signal::Kind::Answer;
            signal.sdp = happening.text;
        }
        send_signal(happening.peer, signal);
    }

    // Says which way a connection went, so a run can show it is relayed.
    void log_route(PeerId peer, std::uint32_t generation) {
        const auto it = links.find(peer);
        if (it == links.end())
            return;
        const Connection *connection = it->second.connection_of(generation);
        if (!connection || !connection->peer_connection)
            return;
        rtc::Candidate local;
        rtc::Candidate remote;
        if (!connection->peer_connection->getSelectedCandidatePair(&local, &remote))
            return;
        std::fprintf(stderr, "[ONLINE] %s generation %u connected, %s\n", vo::peer_hex(peer).c_str(),
                     generation, local.type() == rtc::Candidate::Type::Relayed ? "relayed" : "direct");
    }

    void on_happening(Happening &happening) {
        using Kind = Happening::Kind;
        switch (happening.kind) {
        case Kind::SocketText:
            on_frame(happening.text);
            break;
        case Kind::SocketClosed:
        case Kind::SocketError:
            fail(self == 0 ? VO_ERROR_UNREACHABLE : VO_ERROR_DISCONNECTED,
                 happening.text.empty() ? "the service closed" : happening.text);
            break;
        case Kind::LocalDescription:
        case Kind::LocalCandidate:
            on_local_signal(happening);
            break;
        case Kind::ConnectionState:
            if (happening.state == rtc::PeerConnection::State::Connected)
                log_route(happening.peer, happening.generation);
            if (happening.state == rtc::PeerConnection::State::Failed ||
                happening.state == rtc::PeerConnection::State::Closed)
                on_connection_lost(happening.peer, happening.generation, "the connection failed");
            break;
        case Kind::ChannelOpen:
            on_channel_open(happening.peer, happening.generation, happening.channel);
            break;
        case Kind::ChannelClosed:
            on_connection_lost(happening.peer, happening.generation, "the peer closed its channel");
            break;
        case Kind::ChannelText:
            on_marker(happening.peer, happening.generation, happening.text);
            break;
        case Kind::Packet:
            on_packet(happening);
            break;
        }
    }

    // A credential is asked for again when it is due for renewal, or after a refusal's delay,
    // and only while there is a peer to connect to.
    void renew_if_due(Clock::time_point now) {
        if (turn_asked || links.empty())
            return;
        if (retry_turn_at) {
            if (now >= *retry_turn_at)
                ask_for_turn();
            return;
        }
        if (renew_at && now >= *renew_at)
            ask_for_turn();
    }

    void run_timers() {
        const auto now = Clock::now();
        renew_if_due(now);
        std::vector<std::pair<PeerId, std::string>> failed;
        for (auto &[peer, link] : links) {
            try {
                move_if_due(peer, link, now);
            } catch (const std::exception &error) {
                failed.emplace_back(peer, std::string("moving: ") + error.what());
            }
        }
        for (auto &[peer, why] : failed)
            drop_peer(peer, std::move(why));
    }

    void open() {
        rtc::WebSocket::Configuration config;
        config.maxMessageSize = vo::frame_bytes;
        socket = std::make_shared<rtc::WebSocket>(config);
        const MailboxRef box = mailbox;
        socket->onMessage([](const rtc::binary &) {}, // binary frames are not the protocol's
                          [box](rtc::string text) {
                              Happening happening = seen(Happening::Kind::SocketText);
                              happening.text = std::move(text);
                              post(box, std::move(happening));
                          });
        socket->onClosed([box] { post(box, seen(Happening::Kind::SocketClosed)); });
        socket->onError([box](rtc::string error) {
            Happening happening = seen(Happening::Kind::SocketError);
            happening.text = std::move(error);
            post(box, std::move(happening));
        });
        socket->open(url);
    }

    void close() {
        fatal = true; // nothing more to tell
        for (auto &[peer, link] : links)
            retire_link(peer, link);
        links.clear();
        if (socket)
            socket->close(); // what its callbacks post goes to a mailbox that is gone
        // a bounded wait for libdatachannel to finish closing what we hold
        const auto until = Clock::now() + close_wait;
        while (!retired.empty() && Clock::now() < until) {
            for (const auto &happening : mailbox->take())
                forget_if_closed(happening);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

extern "C" {

vo_session *vo_open(const vo_config *config) {
    if (config == nullptr || config->service_url == nullptr || config->game == nullptr ||
        config->build == nullptr || config->room == nullptr)
        return nullptr;
    auto url = vo::room_url(config->service_url, config->game, config->room, config->build);
    if (!url)
        return nullptr;
    auto session = std::make_unique<vo_session>();
    session->url = std::move(*url);
    try {
        session->open();
    } catch (const std::exception &error) {
        session->fail(VO_ERROR_UNREACHABLE, error.what());
    }
    return session.release(); // owned by the caller until vo_close
}

void vo_close(vo_session *session) {
    const std::unique_ptr<vo_session> owned(session);
    if (!owned)
        return;
    try {
        owned->close();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[ONLINE] closing: %s\n", error.what());
    }
}

int vo_poll(vo_session *session, vo_event *event) {
    if (session == nullptr || event == nullptr)
        return 0;
    try {
        for (auto &happening : session->mailbox->take()) {
            session->forget_if_closed(happening);
            if (!session->fatal)
                session->on_happening(happening);
        }
        if (!session->fatal)
            session->run_timers();
    } catch (const std::exception &error) {
        session->fail(VO_ERROR_DISCONNECTED, error.what());
    }
    if (session->events.empty())
        return 0;
    session->current = std::move(session->events.front());
    session->events.pop_front();
    const Event &current = session->current;
    *event = vo_event{
        .kind = current.kind,
        .peer = current.peer,
        .channel = current.channel,
        .error = current.error,
        .older = current.older ? 1 : 0,
        .message = current.message.empty() ? nullptr : current.message.c_str(),
        .data = reinterpret_cast<const uint8_t *>(current.bytes.data()),
        .size = current.bytes.size(),
    };
    return 1;
}

int vo_send(vo_session *session, vo_peer peer, vo_channel channel, const uint8_t *data, size_t size) {
    if (session == nullptr || session->fatal || (data == nullptr && size > 0))
        return 0;
    if (channel != VO_CHANNEL_CONTROL && channel != VO_CHANNEL_STATE)
        return 0;
    if (size > packet_limit(channel))
        return 0;
    const auto it = session->links.find(peer);
    if (it == session->links.end() || !it->second.announced)
        return 0;
    const auto &data_channel = it->second.sending().channels[channel];
    try {
        const std::size_t queued = data_channel->bufferedAmount();
        if (channel == VO_CHANNEL_STATE && queued > state_queue_bytes)
            return 0;
        if (channel == VO_CHANNEL_CONTROL && queued + size > VO_CONTROL_QUEUE_BYTES) {
            session->drop_peer(peer, "the peer fell behind on CONTROL");
            return 0;
        }
        const std::span<const uint8_t> bytes(data, size);
        data_channel->send(reinterpret_cast<const std::byte *>(bytes.data()), bytes.size());
        return 1;
    } catch (const std::exception &error) {
        session->drop_peer(peer, std::string("send: ") + error.what());
        return 0;
    }
}

void vo_broadcast(vo_session *session, vo_channel channel, const uint8_t *data, size_t size) {
    if (session == nullptr)
        return;
    std::vector<PeerId> open;
    for (const auto &[peer, link] : session->links)
        if (link.announced)
            open.push_back(peer);
    for (const PeerId peer : open) // vo_send may drop a peer
        vo_send(session, peer, channel, data, size);
}

vo_link vo_link_of(const vo_session *session, vo_peer peer) {
    if (session == nullptr)
        return {};
    const auto it = session->links.find(peer);
    if (it == session->links.end() || !it->second.announced)
        return {};
    const Connection &connection = it->second.sending();
    try {
        const auto rtt = connection.peer_connection->rtt();
        return vo_link{
            .open = 1,
            .round_trip_ms = rtt ? static_cast<uint32_t>(rtt->count()) : 0,
            .control_queued_bytes = connection.channels[VO_CHANNEL_CONTROL]->bufferedAmount(),
        };
    } catch (const std::exception &) {
        return {};
    }
}

} // extern "C"
