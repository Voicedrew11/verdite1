#include "handover.h"

#include <utility>

namespace vo {

std::vector<Handover::Packet> Handover::received(From from, Packet packet) {
    std::vector<Packet> deliver;
    if (from == From::Old) {
        // nothing follows the peer's "moved" on the old connection but "seen"
        if (!peer_moved_)
            deliver.push_back(std::move(packet));
        return deliver;
    }
    if (peer_moved_) {
        deliver.push_back(std::move(packet));
    } else {
        held_bytes_ += packet.size();
        held_.push_back(std::move(packet));
    }
    return deliver;
}

std::vector<Handover::Packet> Handover::peer_moved() {
    peer_moved_ = true;
    held_bytes_ = 0;
    return std::exchange(held_, {});
}

} // namespace vo
