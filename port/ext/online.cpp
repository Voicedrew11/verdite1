// Online play: a room joined through the Verdite online service (online/README.md), our pose sent
// to the others once per game update, and theirs played back for the automap and the world. Opt-in:
// nothing touches the network until a room is joined.
#include "internal.hpp"
#include "online_playback.h"
#include "online_pose.h"
#include "online_stride.h"

#include <imgui.h>
#include <kf/platform/automap.h>
#include <kf/platform/input.h>
#include <kf/platform/online_players.h>

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

struct Online {
    std::unique_ptr<vo_session, SessionClose> session;
    std::string room;
    vo_peer self = 0;
    std::map<vo_peer, Peer> players; // open peers
    std::string status;   // what the overlay says when not in a room, or why we left
    std::array<char, room_most + 1> room_field{};
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
    online.session.reset();
    online.room.clear();
    online.self = 0;
    online.players.clear();
    online.status = std::move(why);
}

void join(const std::string &room) {
    leave("");
    const std::string service = service_url();
    const std::string build = build_name();
    const vo_config config{service.c_str(), "kf1", build.c_str(), room.c_str()};
    online.session.reset(vo_open(&config));
    if (!online.session) {
        online.status = "A room code is 1 to 64 letters, digits, - or _.";
        return;
    }
    online.room = room;
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

void on_event(const vo_event &event) {
    switch (event.kind) {
    case VO_EVENT_JOINED:
        online.self = event.peer;
        std::printf("[online] in room %s as %s\n", online.room.c_str(), hex(event.peer).c_str());
        break;
    case VO_EVENT_PEER_OPEN:
        online.players.erase(event.peer);
        online.players.emplace(event.peer, Peer{OnlinePlayback(playback_delay_updates()), {}, {}});
        std::printf("[online] %s is here\n", hex(event.peer).c_str());
        break;
    case VO_EVENT_PEER_CLOSED:
        online.players.erase(event.peer);
        std::printf("[online] %s left\n", hex(event.peer).c_str());
        break;
    case VO_EVENT_PACKET: {
        const auto known = online.players.find(event.peer);
        if (event.channel != VO_CHANNEL_STATE || known == online.players.end())
            break;
        if (const auto pose = online_pose_decode(std::span(event.data, event.size)))
            known->second.playback.receive(*pose);
        break;
    }
    case VO_EVENT_ERROR:
        std::printf("[online] %s\n", event.message ? event.message : "error");
        // a failed peer is followed by its PEER_CLOSED; anything else ends the session
        if (event.error != VO_ERROR_PEER_FAILED)
            online.status = event.message ? event.message : "The connection failed.";
        break;
    }
}

void send_pose(const OnlinePlayerPose &own) {
    if (own.floor < 1 || own.floor > automap_floor_count)
        return;
    const auto bytes = online_pose_encode({own.floor, own.x, own.z, own.yaw, own.weapon, own.attack_phase, own.pitch,
        own_update, own.foot_height});
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
}

void online_stop() {
    leave("");
}

void online_draw_panel() {
    if (online.session) {
        ImGui::Text("Room: %s", online.room.c_str());
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
    ImGui::TextWrapped("Play alongside others: everyone who joins the same room code, with the same "
                       "build of the game, sees each other on the automap.");
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
        if (known.shown)
            poses.push_back(*known.shown);
    return poses;
}

#else

void online_start() {
    if (!start_room.empty())
        std::printf("[online] this build has no online play (built without libdatachannel)\n");
}

void online_poll() {}

void online_stop() {}

void online_draw_panel() {
    ImGui::TextWrapped("This build has no online play: it was built without libdatachannel.");
}

std::vector<OnlinePose> online_players() {
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
    for (const auto &[peer, known] : ext::online.players)
        if (const auto &pose = known.shown; pose && pose->floor == floor)
            players.push_back({static_cast<u32>(peer), pose->x, pose->z, pose->yaw, pose->weapon,
                pose->attack_phase, pose->pitch, pose->foot_height, known.stride.phase, known.stride.amount});
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
