// Online play: a room joined through the Verdite online service (online/README.md), our pose sent
// to the others once per game update, and theirs played back for the automap and the world. Opt-in:
// nothing touches the network until a room is joined.
#include "coop_floor_frames.h"
#include "coop_death_play.h"
#include "coop_floor_play.h"
#include "internal.hpp"
#include "online_playback.h"
#include "online_pose.h"
#include "online_stride.h"

#include <imgui.h>
#include <kf/platform/automap.h>
#include <kf/platform/input.h>
#include <kf/platform/online_players.h>
#include <kf/platform/title.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if KF_ONLINE
#include "build_info.h"
#include <verdite_online.h>
#endif

namespace kf::ext {
namespace {

std::string start_room; // --online-room
std::uint32_t own_update = 0; // game updates counted by online_update, numbering the poses sent

#if KF_ONLINE

constexpr const char *default_service = "https://verdite-online.voicedrew11.workers.dev";
constexpr std::size_t room_most = 64;
constexpr int playback_delay_ms = 100; // behind the newest pose heard, to ride out late ones
constexpr int vblanks_per_second = 60;

struct SessionClose {
    void operator()(vo_session *session) const { vo_close(session); }
};

struct Peer {
    OnlinePlayback playback;
    std::optional<OnlinePose> shown;   // as played back for this update
    OnlineStride stride;
};

struct LobbyClose {
    void operator()(vo_lobby *lobby) const { vo_lobby_close(lobby); }
};

constexpr std::size_t listed_most = 50;

struct Online {
    std::unique_ptr<vo_session, SessionClose> session;
    std::string room;
    vo_peer self = 0;
    std::optional<RoomRole> expected;   // a title join: what the room must find us as
    bool listed = false;
    OnlineFailure failure = OnlineFailure::None;
    std::unique_ptr<vo_lobby, LobbyClose> lobby;
    std::map<vo_peer, Peer> players; // open peers
    std::string status;   // what the overlay says when not in a room, or why we left
    std::array<char, room_most + 1> room_field{};
    std::optional<std::string> leave_after_poll;   // why, once the session's events are handled
};
Online online;

std::string service_url() {
    const char *service = std::getenv("KF_ONLINE_SERVICE");
    return service && *service ? service : default_service;
}

// Peers must have the same build: the version and the commit, as crash reports name it.
std::string build_name() {
    std::string name = std::string("kf1-") + KF_VERSION + "-" + KF_BUILD_COMMIT;
    for (char &c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '+' && c != '-')
            c = '-';
    return name.substr(0, 64);
}

void leave(std::string why) {
    if (online.session)
        coop_room_left();
    online.session.reset();
    online.room.clear();
    online.self = 0;
    online.players.clear();
    online.status = std::move(why);
}

void join(const std::string &room, std::string_view listed_as = {}) {
    leave("");
    online.expected.reset();
    online.failure = OnlineFailure::None;
    const std::string service = service_url();
    const std::string build = build_name();
    const std::string host(listed_as);
    const vo_config config{service.c_str(), "kf1", build.c_str(), room.c_str(), host.empty() ? nullptr : host.c_str()};
    online.session.reset(vo_open(&config));
    if (!online.session) {
        online.status = "A room code is 1 to 64 letters, digits, - or _.";
        online.failure = OnlineFailure::Other;
        return;
    }
    online.room = room;
    online.listed = !host.empty();
    std::printf("[online] joining room %s on %s as %s\n", room.c_str(), service.c_str(), build.c_str());
}

// At least two updates, so one late pose is not a stall.
std::uint32_t playback_delay_updates() {
    const int update_ms = world_update_vblanks * 1000 / vblanks_per_second;
    return static_cast<std::uint32_t>(std::max(2, (playback_delay_ms + update_ms - 1) / update_ms));
}

std::string hex(vo_peer peer) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(peer));
    return text;
}

OnlineFailure failure_of(vo_error error) {
    switch (error) {
    case VO_ERROR_FULL: return OnlineFailure::Full;
    case VO_ERROR_BUILD_MISMATCH: return OnlineFailure::OtherBuild;
    case VO_ERROR_UNREACHABLE:
    case VO_ERROR_DISCONNECTED: return OnlineFailure::Unreachable;
    default: return OnlineFailure::Other;
    }
}

void on_event(const vo_event &event) {
    switch (event.kind) {
    case VO_EVENT_JOINED: {
        const RoomRole found = event.older == 0 ? RoomRole::First : RoomRole::Later;
        if (online.expected && *online.expected != found) {
            online.failure = found == RoomRole::First ? OnlineFailure::NoHost : OnlineFailure::InUse;
            online.leave_after_poll = found == RoomRole::First ? "No one is hosting that room." : "Someone is already in that room.";
            break;
        }
        online.self = event.peer;
        std::printf("[online] in room %s as %s\n", online.room.c_str(), hex(event.peer).c_str());
        coop_room_joined(online.room, found == RoomRole::First);
        break;
    }
    case VO_EVENT_PEER_OPEN:
        online.players.erase(event.peer);
        online.players.emplace(event.peer, Peer{OnlinePlayback(playback_delay_updates()), {}, {}});
        std::printf("[online] %s is here\n", hex(event.peer).c_str());
        coop_peer_open(event.peer, event.older != 0);
        break;
    case VO_EVENT_PEER_CLOSED:
        online.players.erase(event.peer);
        std::printf("[online] %s left\n", hex(event.peer).c_str());
        coop_peer_closed(event.peer);
        break;
    case VO_EVENT_PACKET: {
        const auto known = online.players.find(event.peer);
        if (known == online.players.end())
            break;
        if (event.channel == VO_CHANNEL_CONTROL) {
            coop_control_packet(event.peer, std::span(event.data, event.size));
            break;
        }
        if (event.size > 0 && event.data[0] == coop_frame_tag) {
            coop_floors_state_packet(event.peer, std::span(event.data, event.size));
            break;
        }
        if (const auto pose = online_pose_decode(std::span(event.data, event.size)))
            known->second.playback.receive(*pose);
        break;
    }
    case VO_EVENT_ERROR:
        std::printf("[online] %s\n", event.message ? event.message : "error");
        // a failed peer is followed by its PEER_CLOSED; anything else ends the session
        if (event.error != VO_ERROR_PEER_FAILED) {
            online.status = event.message ? event.message : "The connection failed.";
            online.failure = failure_of(event.error);
        }
        break;
    }
}

void send_pose(const OnlinePlayerPose &own) {
    if (own.floor < 1 || own.floor > automap_floor_count)
        return;
    if (online.listed)
        vo_describe(online.session.get(), static_cast<std::uint32_t>(own.floor));
    const auto bytes = online_pose_encode({own.floor, own.x, own.z, own.yaw, own.weapon, own.attack_phase, own.pitch,
        own_update, own.foot_height, coop_death_down(), coop_death_fighting()});
    vo_broadcast(online.session.get(), VO_CHANNEL_STATE, bytes.data(), bytes.size());
}

void play_on(vo_peer peer, Peer &known) {
    const auto next = known.playback.step();
    if (!next)
        return;
    if (!known.shown || known.shown->floor != next->floor)
        std::printf("[online] %s is on floor %d\n", hex(peer).c_str(), next->floor);
    if (known.shown)
        known.stride = online_stride_step(known.stride, *known.shown, *next);
    known.shown = next;
}

#endif

}

void online_join_at_start(std::string_view room) {
    start_room = room;
}

#if KF_ONLINE

void online_start() {
    if (!start_room.empty())
        join(start_room);
}

void online_poll() {
    if (!online.session)
        return;
    vo_event event{};
    while (online.session && vo_poll(online.session.get(), &event)) {
        on_event(event);
        if (event.kind == VO_EVENT_ERROR && event.error != VO_ERROR_PEER_FAILED)
            leave(online.status); // fatal: the session only waits to be closed
    }
    if (online.leave_after_poll) {
        leave(*online.leave_after_poll);
        online.leave_after_poll.reset();
    }
}

void online_send_control(std::uint64_t peer, std::span<const std::uint8_t> bytes) {
    if (online.session && !vo_send(online.session.get(), peer, VO_CHANNEL_CONTROL, bytes.data(), bytes.size()))
        std::printf("[online] a message to %s was not sent\n", hex(peer).c_str());
}

void online_broadcast_control(std::span<const std::uint8_t> bytes) {
    if (online.session)
        vo_broadcast(online.session.get(), VO_CHANNEL_CONTROL, bytes.data(), bytes.size());
}

void online_broadcast_state(std::span<const std::uint8_t> bytes) {
    if (online.session)
        vo_broadcast(online.session.get(), VO_CHANNEL_STATE, bytes.data(), bytes.size());
}

std::uint64_t online_self_peer() {
    return online.session ? online.self : 0;
}

void online_leave_room(std::string_view why) {
    if (online.session)
        online.leave_after_poll = std::string(why);
}

void online_stop() {
    leave("");
    online.lobby.reset();
}

void online_join_from_title(std::string_view room, RoomRole expected, std::string_view listed_as) {
    join(std::string(room), listed_as);
    if (online.session)
        online.expected = expected;
}

OnlineStatus online_status() {
    return {online.session && online.self == 0, online.session && online.self != 0, online.failure};
}

void online_list_open() {
    if (online.lobby)
        return;
    online.lobby.reset(vo_lobby_open(service_url().c_str(), "kf1", build_name().c_str()));
}

void online_list_close() {
    online.lobby.reset();
}

bool online_rooms(std::vector<verdite::TitleRoom> &rooms, verdite::RoomListState &state) {
    if (!online.lobby) {
        state = verdite::RoomListState::Failed;
        return false;
    }
    const vo_lobby_state now = vo_lobby_state_of(online.lobby.get());
    const verdite::RoomListState was = state;
    state = now == VO_LOBBY_LISTING ? verdite::RoomListState::Listing
            : now == VO_LOBBY_FAILED ? verdite::RoomListState::Failed : verdite::RoomListState::Connecting;
    std::array<vo_listed_room, listed_most> listed{};
    int changed = 0;
    const std::size_t count = vo_lobby_rooms(online.lobby.get(), listed.data(), listed.size(), &changed);
    if (changed) {
        rooms.clear();
        for (const vo_listed_room &row : std::span(listed).first(count))
            rooms.push_back({row.room, row.host, row.floor, row.players, row.most});
    }
    return changed != 0 || state != was;
}

void online_draw_panel() {
    if (online.session) {
        ImGui::Text("Room: %s", online.room.c_str());
        if (const std::string coop_line = coop_panel_line(); !coop_line.empty())
            ImGui::TextUnformatted(coop_line.c_str());
        if (online.self == 0)
            ImGui::TextDisabled("Joining...");
        else if (online.players.empty())
            ImGui::TextDisabled("Waiting for someone to join the same room.");
        for (const auto &[peer, known] : online.players) {
            if (const auto &pose = known.shown)
                ImGui::BulletText("%s  floor %d  (%.1f, %.1f)", hex(peer).c_str(), pose->floor, pose->x, pose->z);
            else
                ImGui::BulletText("%s  connected", hex(peer).c_str());
        }
        if (ImGui::Button("Leave"))
            leave("");
        return;
    }
    ImGui::TextWrapped("Play together: everyone who joins the same room code, with the same build of "
                       "the game, plays one coop world, kept by whoever is in the room first. Joining sets "
                       "this game aside; leaving puts it back.");
    if (title_is_open()) {
        ImGui::TextDisabled("On the title screen, choose CO-OP to host or join a room.");
        return;
    }
    ImGui::InputText("Room code", online.room_field.data(), online.room_field.size());
    if (ImGui::Button("Join") && online.room_field[0] != '\0')
        join(online.room_field.data());
    if (!online.status.empty())
        ImGui::TextWrapped("%s", online.status.c_str());
    ImGui::TextDisabled("Service: %s", service_url().c_str());
}

std::vector<OnlinePose> online_players() {
    std::vector<OnlinePose> poses;
    for (const auto &[peer, known] : online.players)
        if (known.shown && !known.shown->down)
            poses.push_back(*known.shown);
    return poses;
}

std::map<std::uint64_t, OnlinePose> online_room_poses() {
    std::map<std::uint64_t, OnlinePose> poses;
    for (const auto &[peer, known] : online.players)
        if (known.shown)
            poses.emplace(peer, *known.shown);
    return poses;
}

#else

void online_start() {
    if (!start_room.empty())
        std::printf("[online] this build has no online play (built without libdatachannel)\n");
}

void online_poll() {}

void online_stop() {}

void online_send_control(std::uint64_t, std::span<const std::uint8_t>) {}

void online_broadcast_control(std::span<const std::uint8_t>) {}

void online_broadcast_state(std::span<const std::uint8_t>) {}

std::uint64_t online_self_peer() {
    return 0;
}

void online_leave_room(std::string_view) {}

void online_join_from_title(std::string_view, RoomRole, std::string_view) {}

OnlineStatus online_status() {
    return {false, false, OnlineFailure::NoOnline};
}

void online_list_open() {}

void online_list_close() {}

bool online_rooms(std::vector<verdite::TitleRoom> &, verdite::RoomListState &state) {
    const bool changed = state != verdite::RoomListState::Failed;
    state = verdite::RoomListState::Failed;
    return changed;
}

void online_draw_panel() {
    ImGui::TextWrapped("This build has no online play: it was built without libdatachannel.");
}

std::vector<OnlinePose> online_players() {
    return {};
}

std::map<std::uint64_t, OnlinePose> online_room_poses() {
    return {};
}

#endif

}

namespace kf {

bool online_world_runs_while_held() {
    static const bool forced = [] {
        const char *value = std::getenv("KF_TEST_WORLD_RUNS_WHILE_HELD");
        return value != nullptr && std::string_view(value) == "1";
    }();
    if (forced)
        return true;
#if KF_ONLINE
    return ext::online.session && ext::online.self != 0;
#else
    return false;
#endif
}

std::vector<OnlinePlayer> online_players_on_floor(int floor) {
    std::vector<OnlinePlayer> players;
#if KF_ONLINE
    // A player who is down is out of the world (docs/COOP_PLAN.md, "Death is the party's").
    const std::uint64_t eyes = ext::coop_death_eyes_peer();
    for (const auto &[peer, known] : ext::online.players)
        if (const auto &pose = known.shown; pose && pose->floor == floor && !pose->down)
            players.push_back({static_cast<u32>(peer), pose->x, pose->z, pose->yaw, pose->weapon,
                pose->attack_phase, pose->pitch, pose->foot_height, known.stride.phase, known.stride.amount,
                peer == eyes});
#else
    (void)floor;
#endif
    return players;
}

void online_update(const OnlinePlayerPose &own) {
    ++ext::own_update;
#if KF_ONLINE
    if (!ext::online.session || ext::online.self == 0)
        return;
    ext::send_pose(own);
    for (auto &[peer, known] : ext::online.players)
        ext::play_on(peer, known);
#else
    (void)own;
#endif
}

}
