#include "signaling.h"

#include "json.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace vo {
namespace {

using json::int_field;
using json::Object;
using json::string_field;
using Json = Object;

bool is_lower_digit(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool is_alnum(char c) {
    return is_lower_digit(c) || (c >= 'A' && c <= 'Z');
}

bool matches(std::string_view name, std::size_t longest, bool (*allowed)(char)) {
    return !name.empty() && name.size() <= longest && std::ranges::all_of(name, allowed);
}

bool valid_game(std::string_view game) {
    return matches(game, 16, is_lower_digit);
}

bool valid_room(std::string_view room) {
    return matches(room, 64, [](char c) { return is_alnum(c) || c == '_' || c == '-'; });
}

bool valid_host(std::string_view host) {
    return matches(host, 16, [](char c) { return is_alnum(c) || c == ' ' || c == '_' || c == '-'; });
}

bool valid_public_room(std::string_view room) {
    constexpr std::string_view letters = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    return room.size() == 8 && room.front() == 'P' &&
           std::ranges::all_of(room, [&](char c) { return letters.find(c) != std::string_view::npos; });
}

bool valid_build(std::string_view build) {
    return matches(build, 64, [](char c) {
        return is_alnum(c) || c == '.' || c == '_' || c == '+' || c == '-';
    });
}

std::optional<PeerId> peer_field(const cJSON *object, const char *name) {
    const auto hex = string_field(object, name);
    return hex ? parse_peer(*hex) : std::nullopt;
}

// Whether text is written as an IP address, IPv4 or IPv6: only such digits and separators.
bool is_address_text(std::string_view text) {
    const auto ipv4 = [](char c) { return (c >= '0' && c <= '9') || c == '.'; };
    const auto ipv6 = [&](char c) { return ipv4(c) || c == ':' || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
    if (text.empty())
        return false;
    return text.find(':') != std::string_view::npos ? std::ranges::all_of(text, ipv6) : std::ranges::all_of(text, ipv4);
}

// Whether every candidate an SDP lists gives an address.
bool sdp_candidates_are_addresses(std::string_view sdp) {
    constexpr std::string_view candidate_line = "a=candidate:";
    while (!sdp.empty()) {
        const std::size_t end = sdp.find('\n');
        std::string_view line = sdp.substr(0, end);
        sdp = end == std::string_view::npos ? std::string_view{} : sdp.substr(end + 1);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (line.starts_with(candidate_line) && !candidate_is_address(line))
            return false;
    }
    return true;
}

std::optional<Signal> parse_signal(const cJSON *data) {
    if (!cJSON_IsObject(data))
        return std::nullopt;
    const auto kind = string_field(data, "kind");
    if (!kind)
        return std::nullopt;
    const auto generation = int_field(data, "gen", 0, 1 << 30);
    if (!generation)
        return std::nullopt;
    Signal signal;
    signal.generation = static_cast<std::uint32_t>(*generation);
    if (*kind == "restart") {
        signal.kind = Signal::Kind::Restart;
        return signal;
    }
    if (*kind == "offer" || *kind == "answer") {
        signal.kind = *kind == "offer" ? Signal::Kind::Offer : Signal::Kind::Answer;
        auto sdp = string_field(data, "sdp");
        if (!sdp || !sdp_candidates_are_addresses(*sdp))
            return std::nullopt;
        signal.sdp = std::move(*sdp);
        return signal;
    }
    if (*kind == "candidate") {
        auto candidate = string_field(data, "candidate");
        auto mid = string_field(data, "mid");
        if (!candidate || !mid || !candidate_is_address(*candidate))
            return std::nullopt;
        signal.kind = Signal::Kind::Candidate;
        signal.candidate = std::move(*candidate);
        signal.mid = std::move(*mid);
        return signal;
    }
    return std::nullopt;
}

// An RTCIceServer's urls is a string or a list of them; each becomes an IceServer.
bool append_ice_server(const cJSON *server, std::vector<IceServer> &out) {
    if (!cJSON_IsObject(server))
        return false;
    const std::string username = string_field(server, "username").value_or("");
    const std::string credential = string_field(server, "credential").value_or("");
    const cJSON *urls = cJSON_GetObjectItemCaseSensitive(server, "urls");
    if (cJSON_IsString(urls)) {
        out.push_back({urls->valuestring, username, credential});
        return true;
    }
    if (!cJSON_IsArray(urls))
        return false;
    const cJSON *url = nullptr;
    cJSON_ArrayForEach(url, urls) {
        if (!cJSON_IsString(url))
            return false;
        out.push_back({url->valuestring, username, credential});
    }
    return true;
}

std::optional<ServiceFrame> parse_welcome(const cJSON *json) {
    ServiceFrame frame;
    frame.kind = ServiceFrame::Kind::Welcome;
    const auto protocol = int_field(json, "protocol", 0, 1 << 30);
    const auto peer = peer_field(json, "peer");
    const cJSON *peers = cJSON_GetObjectItemCaseSensitive(json, "peers");
    if (!protocol || !peer || !cJSON_IsArray(peers))
        return std::nullopt;
    frame.protocol = *protocol;
    frame.peer = *peer;
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, peers) {
        const auto other = cJSON_IsString(item) ? parse_peer(item->valuestring) : std::nullopt;
        if (!other || *other == *peer)
            return std::nullopt;
        frame.peers.push_back(*other);
    }
    return frame;
}

std::optional<ServiceFrame> parse_turn(const cJSON *json) {
    ServiceFrame frame;
    frame.kind = ServiceFrame::Kind::Turn;
    const auto seconds = int_field(json, "seconds", 1, 7 * 24 * 60 * 60);
    const cJSON *servers = cJSON_GetObjectItemCaseSensitive(json, "iceServers");
    if (!seconds || !cJSON_IsArray(servers))
        return std::nullopt;
    frame.seconds = *seconds;
    const cJSON *server = nullptr;
    cJSON_ArrayForEach(server, servers) {
        if (!append_ice_server(server, frame.ice_servers))
            return std::nullopt;
    }
    return frame;
}

std::optional<ListedRoom> parse_listed_room(const cJSON *row) {
    if (!cJSON_IsObject(row))
        return std::nullopt;
    auto room = string_field(row, "room");
    auto host = string_field(row, "host");
    const auto floor = int_field(row, "floor", 0, static_cast<int>(floor_most));
    const auto players = int_field(row, "players", 0, 64);
    const auto most = int_field(row, "most", 1, 64);
    if (!room || !valid_room(*room) || !host || !valid_host(*host) || !floor || !players || !most)
        return std::nullopt;
    return ListedRoom{std::move(*room), std::move(*host), static_cast<std::uint32_t>(*floor),
                      static_cast<std::uint32_t>(*players), static_cast<std::uint32_t>(*most)};
}

std::optional<ServiceFrame> parse_rooms(const cJSON *json) {
    const cJSON *rooms = cJSON_GetObjectItemCaseSensitive(json, "rooms");
    if (!cJSON_IsArray(rooms) || static_cast<std::size_t>(cJSON_GetArraySize(rooms)) > listed_rooms_most)
        return std::nullopt;
    ServiceFrame frame;
    frame.kind = ServiceFrame::Kind::Rooms;
    const cJSON *row = nullptr;
    cJSON_ArrayForEach(row, rooms) {
        auto room = parse_listed_room(row);
        if (!room)
            return std::nullopt;
        frame.rooms.push_back(std::move(*room));
    }
    return frame;
}

// The service's address as a WebSocket one, without a trailing slash; nothing when it is not
// http:// or https://.
std::optional<std::string> socket_base(std::string_view service_url) {
    std::string url;
    if (service_url.starts_with("https://"))
        url = "wss://" + std::string(service_url.substr(8));
    else if (service_url.starts_with("http://"))
        url = "ws://" + std::string(service_url.substr(7));
    else
        return std::nullopt;
    while (url.ends_with('/'))
        url.pop_back();
    return url;
}

// A query string's value: '+' would be a space, ' ' is not allowed.
std::string query_value(std::string_view value) {
    std::string out;
    for (const char c : value)
        out += c == '+' ? std::string("%2B") : c == ' ' ? std::string("%20") : std::string(1, c);
    return out;
}

ServiceFrame::Kind peer_frame_kind(std::string_view type) {
    if (type == "join")
        return ServiceFrame::Kind::Join;
    if (type == "leave")
        return ServiceFrame::Kind::Leave;
    return ServiceFrame::Kind::Link;
}

std::optional<ServiceFrame> parse_frame(const cJSON *json) {
    const auto type = string_field(json, "type");
    if (!type)
        return std::nullopt;
    if (*type == "welcome")
        return parse_welcome(json);
    if (*type == "turn")
        return parse_turn(json);
    if (*type == "rooms")
        return parse_rooms(json);
    if (*type == "join" || *type == "leave" || *type == "link") {
        const auto peer = peer_field(json, "peer");
        if (!peer)
            return std::nullopt;
        ServiceFrame frame;
        frame.kind = peer_frame_kind(*type);
        frame.peer = *peer;
        return frame;
    }
    if (*type == "signal") {
        const auto from = peer_field(json, "from");
        auto signal = parse_signal(cJSON_GetObjectItemCaseSensitive(json, "data"));
        if (!from || !signal)
            return std::nullopt;
        ServiceFrame frame;
        frame.kind = ServiceFrame::Kind::Signal;
        frame.peer = *from;
        frame.signal = std::move(*signal);
        return frame;
    }
    if (*type == "error") {
        auto code = string_field(json, "code");
        if (!code)
            return std::nullopt;
        ServiceFrame frame;
        frame.kind = ServiceFrame::Kind::Error;
        frame.code = std::move(*code);
        frame.message = string_field(json, "message").value_or("");
        return frame;
    }
    return std::nullopt;
}

std::string print(const cJSON *json) {
    std::unique_ptr<char, decltype(&cJSON_free)> text(cJSON_PrintUnformatted(json), cJSON_free);
    return text ? std::string(text.get()) : std::string();
}

} // namespace

bool candidate_is_address(std::string_view candidate) {
    // foundation, component, transport, priority, then the address
    constexpr int address_field = 4;
    std::size_t at = 0;
    for (int field = 0; field < address_field; ++field) {
        at = candidate.find(' ', at);
        if (at == std::string_view::npos)
            return false;
        at = candidate.find_first_not_of(' ', at);
        if (at == std::string_view::npos)
            return false;
    }
    return is_address_text(candidate.substr(at, candidate.find(' ', at) - at));
}

std::optional<PeerId> parse_peer(std::string_view hex) {
    if (hex.size() != 16)
        return std::nullopt;
    PeerId peer = 0;
    for (const char c : hex) {
        int digit = 0;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else
            return std::nullopt;
        peer = peer << 4 | static_cast<PeerId>(digit);
    }
    if (peer == 0)
        return std::nullopt;
    return peer;
}

std::string peer_hex(PeerId peer) {
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string hex(16, '0');
    for (std::size_t i = 16; i-- > 0; peer >>= 4)
        hex[i] = digits[peer & 0xf];
    return hex;
}

std::optional<std::string> room_url(std::string_view service_url, std::string_view game,
                                    std::string_view room, std::string_view build,
                                    std::string_view listed_as) {
    if (!valid_game(game) || !valid_room(room) || !valid_build(build))
        return std::nullopt;
    if (!listed_as.empty() && (!valid_host(listed_as) || !valid_public_room(room)))
        return std::nullopt;
    auto url = socket_base(service_url);
    if (!url)
        return std::nullopt;
    *url += "/v1/rooms/" + std::string(game) + "/" + std::string(room) +
            "?protocol=" + std::to_string(protocol_version) + "&build=" + query_value(build);
    if (!listed_as.empty())
        *url += "&listed=1&host=" + query_value(listed_as);
    return url;
}

std::optional<std::string> lobby_url(std::string_view service_url, std::string_view game, std::string_view build) {
    if (!valid_game(game) || !valid_build(build))
        return std::nullopt;
    auto url = socket_base(service_url);
    if (!url)
        return std::nullopt;
    *url += "/v1/lobby/" + std::string(game) + "?protocol=" + std::to_string(protocol_version) +
            "&build=" + query_value(build);
    return url;
}

std::optional<std::string> floor_records_url(std::string_view service_url, std::string_view game,
                                             std::uint32_t area, std::uint32_t seed) {
    if (!valid_game(game))
        return std::nullopt;
    auto url = socket_base(service_url);
    if (!url)
        return std::nullopt;
    *url += "/v1/floors/" + std::string(game) + "/" + std::to_string(area) + "/records?protocol=" +
            std::to_string(protocol_version) + "&seed=" + std::to_string(seed);
    return url;
}

std::optional<std::string> floor_mesh_url(std::string_view service_url, std::string_view game,
                                          std::uint32_t area, std::string_view build) {
    if (!valid_game(game) || !valid_build(build))
        return std::nullopt;
    auto url = socket_base(service_url);
    if (!url)
        return std::nullopt;
    *url += "/v1/floors/" + std::string(game) + "/" + std::to_string(area) + "/mesh?protocol=" +
            std::to_string(protocol_version) + "&build=" + query_value(build);
    return url;
}

std::optional<ServiceFrame> parse_service_frame(std::string_view text) {
    if (text.size() > frame_bytes)
        return std::nullopt;
    const Json json(cJSON_ParseWithLength(text.data(), text.size()));
    if (!cJSON_IsObject(json.get()))
        return std::nullopt;
    return parse_frame(json.get());
}

std::string signal_frame(PeerId to, const Signal &signal) {
    const Json frame(cJSON_CreateObject());
    cJSON_AddStringToObject(frame.get(), "type", "signal");
    cJSON_AddStringToObject(frame.get(), "to", peer_hex(to).c_str());
    cJSON *data = cJSON_AddObjectToObject(frame.get(), "data");
    cJSON_AddNumberToObject(data, "gen", signal.generation);
    switch (signal.kind) {
    case Signal::Kind::Restart:
        cJSON_AddStringToObject(data, "kind", "restart");
        break;
    case Signal::Kind::Offer:
    case Signal::Kind::Answer:
        cJSON_AddStringToObject(data, "kind", signal.kind == Signal::Kind::Offer ? "offer" : "answer");
        cJSON_AddStringToObject(data, "sdp", signal.sdp.c_str());
        break;
    case Signal::Kind::Candidate:
        cJSON_AddStringToObject(data, "kind", "candidate");
        cJSON_AddStringToObject(data, "candidate", signal.candidate.c_str());
        cJSON_AddStringToObject(data, "mid", signal.mid.c_str());
        break;
    }
    return print(frame.get());
}

std::string turn_frame() {
    return R"({"type":"turn"})";
}

std::string describe_frame(std::uint32_t floor) {
    return R"({"type":"describe","floor":)" + std::to_string(std::min(floor, floor_most)) + "}";
}

} // namespace vo
