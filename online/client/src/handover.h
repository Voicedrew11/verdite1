#pragma once
// Moving a peer's CONTROL traffic from its connection to the next one (a new TURN credential)
// without losing, repeating or reordering a packet. Knows no network: the session tells it what
// arrived and delivers what it hands back.
//
// Each side sends "moved" on the old CONTROL channel after its last packet there, then sends on the
// new one. A receiver holds what comes on the new connection until the peer's "moved", which
// arrives after everything the peer sent on the old one. It answers "moved" with "seen", so a sender
// knows its old packets have all arrived; the old connection closes when both have.

#include <cstddef>
#include <vector>

namespace vo {

class Handover {
public:
    using Packet = std::vector<std::byte>;
    enum class From { Old, New };

    // A CONTROL packet arrived; returns what to deliver now, in order.
    std::vector<Packet> received(From from, Packet packet);
    // The peer's "moved" arrived on the old connection; returns the packets held until it.
    std::vector<Packet> peer_moved();
    // Our "moved" went out on the old connection: from now on we send on the new one.
    void sent_moved() {
        sent_moved_ = true;
    }
    // The peer's "seen" arrived: everything we sent on the old connection reached it.
    void peer_saw_moved() {
        peer_saw_moved_ = true;
    }

    // The peer closed the old connection. That is a finish once both sides have moved: the peer
    // closes only after it has our "moved", so our "seen" may be lost in the close but nothing
    // else is. Before then it is a failure, and returns false.
    bool peer_closed_old() {
        if (!sent_moved_ || !peer_moved_)
            return false;
        peer_saw_moved_ = true;
        return true;
    }

    bool sending_on_new() const {
        return sent_moved_;
    }
    // Neither side has anything left on the old connection, so it can close.
    bool finished() const {
        return sent_moved_ && peer_moved_ && peer_saw_moved_;
    }

private:
    bool sent_moved_ = false;
    bool peer_moved_ = false;
    bool peer_saw_moved_ = false;
    std::vector<Packet> held_; // from the new connection, before the peer's "moved"
};

} // namespace vo
