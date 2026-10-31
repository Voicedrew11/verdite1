// verdite_online.h's board: one game's records on the area the player is in, from the floor's
// records socket (the service's sample, posts, ratings, the author's own list) and the floor mesh
// (gossip between players; online/protocol/PROTOCOL.md, "Records").
//
// Threads: every vo_board_* call is on the game's thread. libdatachannel's callbacks on the records
// socket only post what they saw to its mailbox, and the mesh's session hands its events over the
// same way through vo_poll; a call reads both, so the board's state has one thread and needs no
// lock.

#include "verdite_online.h"

#include "floor_frames.h"
#include "mesh.h"
#include "record.h"
#include "signaling.h"

#include <rtc/rtc.hpp>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using vo::Record;
using vo::RecordKey;
using vo::RecordPool;

// The mesh checks no build, but the service asks for one: a name in the form of the record format.
constexpr const char *mesh_build = "records-1";
constexpr auto mine_refresh = std::chrono::seconds(5); // counts change without a word to us
constexpr std::size_t serial_file_bytes = 16;

// What a records socket's libdatachannel thread saw.
struct SocketEvent {
    enum class Kind { Open, Text, Closed };
    Kind kind = Kind::Closed;
    std::string text; // Text: a frame. Closed: why, when it failed
};

class SocketMailbox {
  public:
    // On a libdatachannel thread.
    void post(SocketEvent event) {
        const std::scoped_lock lock(mutex_);
        events_.push_back(std::move(event));
    }
    std::deque<SocketEvent> take() {
        const std::scoped_lock lock(mutex_);
        return std::exchange(events_, {});
    }

  private:
    std::mutex mutex_;
    std::deque<SocketEvent> events_;
};

// Callbacks hold the mailbox weakly, so one that fires after the board left its area finds it gone.
void post_to(const std::weak_ptr<SocketMailbox> &box, SocketEvent event) {
    if (const auto held = box.lock())
        held->post(std::move(event));
}

enum class SocketState { None, Connecting, Open, Closed };

// A request on the records socket. The service answers in the order they came, so the first one
// still awaiting is the one an answer belongs to.
struct Request {
    enum class Kind { Post, Rate, Mine };
    Kind kind = Kind::Mine;
    std::uint64_t id = 0; // Post, Rate
    std::string frame;    // set only while queued until the socket opens
};

// A record this install holds, with its good count. posted: the service has taken it, so the next
// mine list has it.
struct OwnRecord {
    Record record;
    std::uint64_t id = 0;
    std::uint32_t good = 0;
    bool posted = false;
};

struct MeshClose {
    void operator()(vo_session *session) const {
        vo_close(session);
    }
};
using MeshPtr = std::unique_ptr<vo_session, MeshClose>;

std::uint32_t random_seed() {
    std::random_device random;
    return random();
}

// The serial in a file: 0 when there is no file yet, nothing when the file is not a number.
std::optional<std::uint32_t> read_serial(const std::string &path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? std::optional<std::uint32_t>(0) : std::nullopt;
    std::array<char, serial_file_bytes> text{};
    const ssize_t got = ::read(fd, text.data(), text.size());
    const bool closed = ::close(fd) == 0;
    if (got <= 0 || !closed)
        return std::nullopt;
    std::string_view digits(text.data(), static_cast<std::size_t>(got));
    if (digits.ends_with('\n'))
        digits.remove_suffix(1);
    if (digits.empty() || digits.size() > 10 ||
        !std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    const unsigned long long value = std::stoull(std::string(digits));
    if (value > UINT32_MAX)
        return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

// Writes the serial through a temporary file and its rename, synced: a restart reads either this
// serial or the one before, never a part.
bool store_serial(const std::string &path, std::uint32_t serial) {
    const std::string temp = path + ".tmp";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    const std::string text = std::to_string(serial) + "\n";
    const bool written = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    const bool synced = ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    if (!written || !synced || !closed) {
        ::unlink(temp.c_str());
        return false;
    }
    return ::rename(temp.c_str(), path.c_str()) == 0;
}

vo_record to_vo_record(const Record &record, std::uint64_t id, std::uint32_t good, bool mine) {
    vo_record out{};
    out.id = id;
    out.area = record.area;
    out.place = record.place;
    std::ranges::copy(record.payload, out.payload);
    out.payload_size = record.payload_size;
    out.mine = mine ? 1 : 0;
    out.good = good;
    return out;
}

} // namespace

struct vo_board {
    vo_board(std::string service, std::string game_name, vo::RecordSigner key,
             std::string serial_file, std::uint32_t last_serial);

    // The game's thread: what the records socket and the mesh said since the last call.
    void pump();
    void enter(std::uint32_t entered);
    void leave();
    vo_record_id post(std::uint32_t place, std::span<const std::uint8_t> payload);
    void rate(vo_record_id id);
    std::size_t copy_records(vo_record *out, std::size_t most, int *changed);
    std::size_t copy_mine(vo_record *out, std::size_t most, int *changed);

  private:
    // The records socket
    void open_records(std::uint32_t entered);
    void close_records();
    void drain_socket();
    void on_socket_open();
    void on_socket_text(const std::string &text);
    void on_socket_closed(const std::string &why);
    void send_request(Request request);
    void send_now(const Request &request);
    std::optional<Request> answered();
    void request_mine();
    void refresh_mine_if_due();
    void forget_unanswered();
    void on_sample(const std::vector<vo::CountedRecord> &sample);
    void on_posted(std::uint64_t id, std::uint32_t good);
    void on_rated(std::uint64_t id, std::uint32_t good);
    void on_mine(const std::vector<vo::CountedRecord> &records);
    void on_error(const std::string &code, const std::string &message);

    // The mesh
    void open_mesh(std::uint32_t entered);
    void pump_mesh();
    void handle_mesh_event(const vo_event &event);
    void send_shown(vo_peer peer);
    void on_gossip(vo_peer peer, std::span<const std::uint8_t> bytes);
    void on_shown(vo_peer peer, const std::vector<Record> &records);
    void on_new(vo_peer peer, const Record &record, std::uint8_t hops);
    void forward(vo_peer from, const Record &record, std::uint8_t hops);
    std::optional<RecordPool::Added> take_from(vo_peer peer, const Record &record);
    void ignore_peer(vo_peer peer, const char *why);
    std::vector<Record> shown_records() const;
    bool send_control(vo_peer peer, const std::vector<std::uint8_t> &packet);

    // The records
    RecordKey own_key() const {
        return signer.public_key();
    }
    bool in_area(const Record &record) const {
        return area && record.area == *area;
    }
    const RecordPool::Held *find_held(std::uint64_t id) const;
    OwnRecord *find_own_post(std::uint64_t id);
    bool in_mine_list(std::uint64_t id) const;
    bool is_own(std::uint64_t id) const;
    void set_good(std::uint64_t id, std::uint32_t good);
    void raise_serial(std::uint32_t seen);

    std::string service_url;
    std::string game;
    vo::RecordSigner signer;
    std::string serial_path;
    std::uint32_t serial = 0; // the last one used, signed or not
    std::uint32_t seed = 0;   // the sample's, for the board's life
    RecordPool pool;
    std::optional<std::uint32_t> area;

    std::set<std::uint64_t> sample_ids; // the service's sample of this area: shown to a new link
    std::set<std::uint64_t> rated;      // every record this board has rated
    std::set<vo_peer> ignored;          // peers that sent a forged or malformed packet
    std::set<vo_peer> open_peers;       // the mesh's peers whose channels are open

    std::shared_ptr<SocketMailbox> mail;
    std::shared_ptr<rtc::WebSocket> socket;
    SocketState socket_state = SocketState::None;
    std::deque<Request> outbox;   // while connecting
    std::deque<Request> awaiting; // sent, answer not in yet
    std::vector<OwnRecord> own_posts;
    std::vector<OwnRecord> mine_list; // the service's list of this author's records
    Clock::time_point mine_asked_at{};

    MeshPtr mesh;

    std::uint64_t records_version = 1;
    std::uint64_t copied_records_version = 0;
    std::uint64_t mine_version = 1;
    std::uint64_t copied_mine_version = 0;
};

vo_board::vo_board(std::string service, std::string game_name, vo::RecordSigner key,
                   std::string serial_file, std::uint32_t last_serial)
    : service_url(std::move(service)), game(std::move(game_name)), signer(key),
      serial_path(std::move(serial_file)), serial(last_serial), seed(random_seed()), pool(game) {}

// ---- the game's calls -------------------------------------------------------------------------

void vo_board::pump() {
    drain_socket();
    pump_mesh();
    refresh_mine_if_due();
}

void vo_board::enter(std::uint32_t entered) {
    if (area == entered)
        return;
    leave();
    area = entered;
    open_records(entered);
    open_mesh(entered);
}

void vo_board::leave() {
    if (!area)
        return;
    close_records();
    mesh.reset(); // vo_close: waits for the mesh's threads, as a room's close does
    area.reset();
    pool.clear();
    sample_ids.clear();
    ignored.clear();
    open_peers.clear();
    forget_unanswered();
    ++records_version;
}

vo_record_id vo_board::post(std::uint32_t place, std::span<const std::uint8_t> payload) {
    if (!area || payload.size() > vo::record_payload_bytes || serial == UINT32_MAX)
        return 0;
    const std::uint32_t next = serial + 1;
    if (!store_serial(serial_path, next)) // the serial is on disk before anything is signed with it
        return 0;
    serial = next;

    Record unsigned_record;
    unsigned_record.area = *area;
    unsigned_record.place = place;
    unsigned_record.payload_size = static_cast<std::uint8_t>(payload.size());
    std::ranges::copy(payload, unsigned_record.payload.begin());
    unsigned_record.serial = next;
    const Record posted = signer.sign(unsigned_record, game);
    const std::uint64_t id = vo::record_id(posted);

    pool.add_own(posted);
    ++records_version;
    own_posts.push_back(OwnRecord{posted, id, 0, false});
    ++mine_version;
    send_request(Request{Request::Kind::Post, id, vo::post_frame(posted)});
    if (mesh) {
        const auto packet = vo::new_packet(posted, vo::new_hops_most);
        vo_broadcast(mesh.get(), VO_CHANNEL_CONTROL, packet.data(), packet.size());
    }
    return id;
}

void vo_board::rate(vo_record_id id) {
    if (!area || id == 0 || rated.contains(id) || is_own(id))
        return;
    const vo::Rating rating = vo::rating_sign(signer, id, game);
    rated.insert(id);
    send_request(Request{Request::Kind::Rate, id, vo::rate_frame(rating)});
}

std::size_t vo_board::copy_records(vo_record *out, std::size_t most, int *changed) {
    if (changed != nullptr)
        *changed = records_version != copied_records_version ? 1 : 0;
    copied_records_version = records_version;
    const auto held = pool.held();
    const std::size_t count = std::min(most, held.size());
    const RecordKey own = own_key();
    for (std::size_t i = 0; i < count; ++i) {
        const RecordPool::Held &each = held[i];
        out[i] = to_vo_record(each.record, each.id, each.good, each.record.author == own);
    }
    return count;
}

std::size_t vo_board::copy_mine(vo_record *out, std::size_t most, int *changed) {
    if (changed != nullptr)
        *changed = mine_version != copied_mine_version ? 1 : 0;
    copied_mine_version = mine_version;
    // The service's list, then our posts it has not listed yet; newest first
    std::vector<const OwnRecord *> view;
    view.reserve(mine_list.size() + own_posts.size());
    for (const OwnRecord &own : mine_list)
        view.push_back(&own);
    for (const OwnRecord &own : own_posts)
        if (!in_mine_list(own.id))
            view.push_back(&own);
    std::ranges::sort(view, [](const OwnRecord *a, const OwnRecord *b) {
        return a->record.serial > b->record.serial;
    });
    const std::size_t count = std::min(most, view.size());
    for (std::size_t i = 0; i < count; ++i)
        out[i] = to_vo_record(view[i]->record, view[i]->id, view[i]->good, true);
    return count;
}

// ---- the records socket -----------------------------------------------------------------------

void vo_board::open_records(std::uint32_t entered) {
    const auto url = vo::floor_records_url(service_url, game, entered, seed);
    if (!url)
        return;
    try {
        rtc::WebSocket::Configuration config;
        config.maxMessageSize = vo::frame_bytes;
        mail = std::make_shared<SocketMailbox>();
        socket = std::make_shared<rtc::WebSocket>(config);
        const std::weak_ptr<SocketMailbox> box = mail;
        socket->onOpen([box] { post_to(box, SocketEvent{SocketEvent::Kind::Open, {}}); });
        socket->onMessage([](const rtc::binary &) {}, // binary frames are not the records protocol
                          [box](rtc::string text) {
                              post_to(box, SocketEvent{SocketEvent::Kind::Text, std::move(text)});
                          });
        socket->onClosed([box] { post_to(box, SocketEvent{SocketEvent::Kind::Closed, {}}); });
        socket->onError([box](const rtc::string &error) {
            post_to(box, SocketEvent{SocketEvent::Kind::Closed, error});
        });
        socket_state = SocketState::Connecting;
        socket->open(*url);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[ONLINE] the floor's records: %s\n", error.what());
        close_records();
    }
}

void vo_board::close_records() {
    if (socket) {
        try {
            socket->resetCallbacks(); // what its thread posts after this goes nowhere
            socket->close();
        } catch (const std::exception &error) {
            std::fprintf(stderr, "[ONLINE] closing the floor's records: %s\n", error.what());
        }
    }
    socket.reset();
    mail.reset();
    socket_state = SocketState::None;
    outbox.clear();
    awaiting.clear();
}

void vo_board::drain_socket() {
    if (!mail)
        return;
    for (const SocketEvent &event : mail->take()) {
        switch (event.kind) {
        case SocketEvent::Kind::Open:
            on_socket_open();
            break;
        case SocketEvent::Kind::Text:
            on_socket_text(event.text);
            break;
        case SocketEvent::Kind::Closed:
            on_socket_closed(event.text);
            break;
        }
    }
}

void vo_board::on_socket_open() {
    socket_state = SocketState::Open;
    for (const Request &request : std::exchange(outbox, {}))
        send_now(request);
    request_mine();
}

void vo_board::on_socket_text(const std::string &text) {
    const auto frame = vo::parse_floor_frame(text);
    if (!frame) {
        std::fprintf(stderr, "[ONLINE] ignoring a records frame the protocol does not have\n");
        return;
    }
    switch (frame->kind) {
    case vo::FloorFrame::Kind::Records:
        on_sample(frame->records);
        break;
    case vo::FloorFrame::Kind::Posted:
        on_posted(frame->id, frame->good);
        break;
    case vo::FloorFrame::Kind::Rated:
        on_rated(frame->id, frame->good);
        break;
    case vo::FloorFrame::Kind::Mine:
        on_mine(frame->records);
        break;
    case vo::FloorFrame::Kind::Error:
        on_error(frame->code, frame->message);
        break;
    }
}

void vo_board::on_socket_closed(const std::string &why) {
    std::fprintf(stderr, "[ONLINE] the floor's records closed%s%s\n", why.empty() ? "" : ": ",
                 why.c_str());
    socket_state = SocketState::Closed;
    outbox.clear();
    awaiting.clear();
    forget_unanswered();
}

void vo_board::send_request(Request request) {
    switch (socket_state) {
    case SocketState::Connecting:
        outbox.push_back(std::move(request));
        return;
    case SocketState::Open:
        send_now(request);
        return;
    case SocketState::None:
    case SocketState::Closed:
        return; // no answer will come
    }
}

void vo_board::send_now(const Request &request) {
    try {
        socket->send(request.frame);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[ONLINE] the floor's records send: %s\n", error.what());
        return;
    }
    awaiting.push_back(Request{request.kind, request.id, {}});
}

std::optional<Request> vo_board::answered() {
    if (awaiting.empty())
        return std::nullopt;
    Request first = std::move(awaiting.front());
    awaiting.pop_front();
    return first;
}

void vo_board::request_mine() {
    mine_asked_at = Clock::now();
    send_request(Request{Request::Kind::Mine, 0, vo::mine_frame(own_key())});
}

// The counts of a record change without a word to this socket, so the list is asked for again now
// and then.
void vo_board::refresh_mine_if_due() {
    if (socket_state == SocketState::Open && Clock::now() - mine_asked_at >= mine_refresh)
        request_mine();
}

// Our posts the service has not confirmed are gone with the socket that would have answered.
void vo_board::forget_unanswered() {
    const std::size_t before = own_posts.size();
    std::erase_if(own_posts, [](const OwnRecord &own) { return !own.posted; });
    if (own_posts.size() != before)
        ++mine_version;
}

void vo_board::on_sample(const std::vector<vo::CountedRecord> &sample) {
    for (const vo::CountedRecord &counted : sample) {
        if (!in_area(counted.record))
            continue;
        const RecordPool::Added added = pool.add(counted.record);
        if (added == RecordPool::Added::Forged || added == RecordPool::Added::Retired)
            continue;
        if (added == RecordPool::Added::New)
            ++records_version;
        const std::uint64_t id = vo::record_id(counted.record);
        sample_ids.insert(id);
        set_good(id, counted.good);
    }
}

void vo_board::on_posted(std::uint64_t id, std::uint32_t good) {
    answered();
    if (OwnRecord *own = find_own_post(id))
        own->posted = true;
    set_good(id, good);
    request_mine(); // the list has the post now
}

void vo_board::on_rated(std::uint64_t id, std::uint32_t good) {
    answered();
    set_good(id, good);
}

void vo_board::on_mine(const std::vector<vo::CountedRecord> &records) {
    answered();
    mine_list.clear();
    std::uint32_t newest = 0;
    for (const vo::CountedRecord &counted : records) {
        if (counted.record.author != own_key())
            continue;
        mine_list.push_back(
            OwnRecord{counted.record, vo::record_id(counted.record), counted.good, true});
        newest = std::max(newest, counted.record.serial);
    }
    // The list answers every post sent before it, so those the service has taken are in it now.
    std::erase_if(own_posts, [](const OwnRecord &own) { return own.posted; });
    raise_serial(newest);
    ++mine_version;
}

void vo_board::on_error(const std::string &code, const std::string &message) {
    const std::optional<Request> refused = answered();
    std::fprintf(stderr, "[ONLINE] the floor's records: %s: %s\n", code.c_str(), message.c_str());
    if (!refused || refused->kind != Request::Kind::Post)
        return;
    const auto before = own_posts.size();
    std::erase_if(own_posts, [id = refused->id](const OwnRecord &own) { return own.id == id; });
    if (own_posts.size() != before)
        ++mine_version;
}

// ---- the mesh ---------------------------------------------------------------------------------

void vo_board::open_mesh(std::uint32_t entered) {
    mesh.reset(vo_open_mesh(service_url.c_str(), game.c_str(), entered, mesh_build));
}

void vo_board::pump_mesh() {
    if (!mesh)
        return;
    vo_event event{};
    while (vo_poll(mesh.get(), &event) != 0)
        handle_mesh_event(event);
}

void vo_board::handle_mesh_event(const vo_event &event) {
    switch (event.kind) {
    case VO_EVENT_JOINED:
        return;
    case VO_EVENT_PEER_OPEN:
        open_peers.insert(event.peer);
        send_shown(event.peer);
        return;
    case VO_EVENT_PEER_CLOSED:
        open_peers.erase(event.peer);
        return;
    case VO_EVENT_PACKET:
        if (event.channel == VO_CHANNEL_CONTROL)
            on_gossip(event.peer, std::span<const std::uint8_t>(event.data, event.size));
        return;
    case VO_EVENT_ERROR:
        std::fprintf(stderr, "[ONLINE] the floor's mesh: %s\n", event.message ? event.message : "");
        return;
    }
}

void vo_board::send_shown(vo_peer peer) {
    send_control(peer, vo::shown_packet(shown_records()));
}

std::vector<Record> vo_board::shown_records() const {
    std::vector<Record> out;
    const RecordKey own = own_key();
    for (const RecordPool::Held &held : pool.held()) {
        if (out.size() == vo::shown_most)
            break;
        if (sample_ids.contains(held.id) || held.record.author == own)
            out.push_back(held.record);
    }
    return out;
}

bool vo_board::send_control(vo_peer peer, const std::vector<std::uint8_t> &packet) {
    if (vo_send(mesh.get(), peer, VO_CHANNEL_CONTROL, packet.data(), packet.size()) != 0)
        return true;
    std::fprintf(stderr, "[ONLINE] a mesh packet was not sent to %s\n", vo::peer_hex(peer).c_str());
    return false;
}

void vo_board::on_gossip(vo_peer peer, std::span<const std::uint8_t> bytes) {
    if (ignored.contains(peer))
        return;
    const auto packet = vo::parse_gossip(bytes);
    if (!packet) {
        ignore_peer(peer, "a malformed packet");
        return;
    }
    if (packet->kind == vo::GossipPacket::Kind::Shown) {
        on_shown(peer, packet->records);
        return;
    }
    on_new(peer, packet->records.front(), packet->hops);
}

void vo_board::on_shown(vo_peer peer, const std::vector<Record> &records) {
    for (const Record &record : records)
        if (!take_from(peer, record))
            return;
}

// A post a player did not hold before goes on, with one hop fewer, while hops are left.
void vo_board::on_new(vo_peer peer, const Record &record, std::uint8_t hops) {
    const auto added = take_from(peer, record);
    if (!added || *added != RecordPool::Added::New || hops <= 1)
        return;
    forward(peer, record, static_cast<std::uint8_t>(hops - 1));
}

void vo_board::forward(vo_peer from, const Record &record, std::uint8_t hops) {
    const auto packet = vo::new_packet(record, hops);
    for (const vo_peer other : open_peers)
        if (other != from)
            send_control(other, packet);
}

// The pool's answer for a record a peer sent; nothing when the record is forged or of another
// area, and then the peer is ignored from here on.
std::optional<RecordPool::Added> vo_board::take_from(vo_peer peer, const Record &record) {
    const RecordPool::Added added = in_area(record) ? pool.add(record) : RecordPool::Added::Forged;
    if (added == RecordPool::Added::Forged) {
        ignore_peer(peer, "a forged record or one of another area");
        return std::nullopt;
    }
    if (added == RecordPool::Added::New)
        ++records_version;
    return added;
}

void vo_board::ignore_peer(vo_peer peer, const char *why) {
    ignored.insert(peer);
    std::fprintf(stderr, "[ONLINE] %s sent %s: its packets are ignored\n",
                 vo::peer_hex(peer).c_str(), why);
}

// ---- records ----------------------------------------------------------------------------------

const RecordPool::Held *vo_board::find_held(std::uint64_t id) const {
    for (const RecordPool::Held &held : pool.held())
        if (held.id == id)
            return &held;
    return nullptr;
}

OwnRecord *vo_board::find_own_post(std::uint64_t id) {
    const auto found = std::ranges::find(own_posts, id, &OwnRecord::id);
    return found == own_posts.end() ? nullptr : &*found;
}

bool vo_board::in_mine_list(std::uint64_t id) const {
    return std::ranges::any_of(mine_list, [id](const OwnRecord &own) { return own.id == id; });
}

bool vo_board::is_own(std::uint64_t id) const {
    if (in_mine_list(id) ||
        std::ranges::any_of(own_posts, [id](const OwnRecord &own) { return own.id == id; }))
        return true;
    const RecordPool::Held *held = find_held(id);
    return held != nullptr && held->record.author == own_key();
}

// A good count the service gave: the record the board shows, and ours in the list, wherever held.
void vo_board::set_good(std::uint64_t id, std::uint32_t good) {
    const RecordPool::Held *held = find_held(id);
    if (held != nullptr && held->good != good) {
        pool.set_good(id, good);
        ++records_version;
    }
    bool changed = false;
    for (std::vector<OwnRecord> *list : {&mine_list, &own_posts}) {
        for (OwnRecord &own : *list) {
            if (own.id == id && own.good != good) {
                own.good = good;
                changed = true;
            }
        }
    }
    if (changed)
        ++mine_version;
}

// The author's newest serial as the service knows it, kept on disk so a restart never reuses one.
void vo_board::raise_serial(std::uint32_t seen) {
    if (seen <= serial)
        return;
    if (store_serial(serial_path, seen))
        serial = seen;
    else
        std::fprintf(stderr, "[ONLINE] could not store the record serial in %s\n",
                     serial_path.c_str());
}

// ---- the C interface --------------------------------------------------------------------------

extern "C" {

vo_board *vo_board_open(const char *service_url, const char *game, const char *key_path) {
    if (service_url == nullptr || game == nullptr || key_path == nullptr)
        return nullptr;
    if (!vo::floor_records_url(service_url, game, 0, 0)) // the names' check
        return nullptr;
    try {
        std::optional<vo::RecordSigner> signer = vo::RecordSigner::load(key_path);
        if (!signer)
            return nullptr;
        const std::string serial_path = std::string(key_path) + ".serial";
        const std::optional<std::uint32_t> serial = read_serial(serial_path);
        if (!serial)
            return nullptr;
        return std::make_unique<vo_board>(service_url, game, *signer, serial_path, *serial)
            .release();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[ONLINE] opening the board: %s\n", error.what());
        return nullptr;
    }
}

void vo_board_close(vo_board *board) {
    const std::unique_ptr<vo_board> owned(board);
    if (!owned)
        return;
    try {
        owned->leave();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[ONLINE] closing the board: %s\n", error.what());
    }
}

void vo_board_enter(vo_board *board, uint32_t area) {
    if (board == nullptr)
        return;
    try {
        board->enter(area);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[ONLINE] entering the area: %s\n", error.what());
    }
}

void vo_board_leave(vo_board *board) {
    if (board == nullptr)
        return;
    try {
        board->leave();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[ONLINE] leaving the area: %s\n", error.what());
    }
}

vo_record_id vo_board_post(vo_board *board, uint32_t place, const uint8_t *payload, size_t size) {
    if (board == nullptr || (payload == nullptr && size > 0))
        return 0;
    try {
        return board->post(place, std::span<const std::uint8_t>(payload, size));
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[ONLINE] posting: %s\n", error.what());
        return 0;
    }
}

void vo_board_rate(vo_board *board, vo_record_id id) {
    if (board == nullptr)
        return;
    try {
        board->rate(id);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[ONLINE] rating: %s\n", error.what());
    }
}

size_t vo_board_records(vo_board *board, vo_record *records, size_t most, int *changed) {
    if (changed != nullptr)
        *changed = 0;
    if (board == nullptr || (records == nullptr && most > 0))
        return 0;
    try {
        board->pump();
        return board->copy_records(records, most, changed);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[ONLINE] the records: %s\n", error.what());
        return 0;
    }
}

size_t vo_board_mine(vo_board *board, vo_record *records, size_t most, int *changed) {
    if (changed != nullptr)
        *changed = 0;
    if (board == nullptr || (records == nullptr && most > 0))
        return 0;
    try {
        board->pump();
        return board->copy_mine(records, most, changed);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[ONLINE] the list of mine: %s\n", error.what());
        return 0;
    }
}

} // extern "C"
