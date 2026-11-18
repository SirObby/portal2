#include "RLBridge.hpp"

#include "ControlSocket.hpp"
#include "SharedFrameBuffer.hpp"
#include "RLState.hpp"
#include "RLInput.hpp"
#include "InputProbe.hpp"
#include "ChallengeMode.hpp"
#include "Engine.hpp"
#include "Entity.hpp"
#include "Modules/Console.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace RLBridge {

namespace {

RLTransport::ControlSocket g_socket;
RLTransport::SharedFrameBuffer g_frame_buf;

uint16_t g_control_port = 0;

uint32_t g_width = 0;
uint32_t g_height = 0;
bool g_handshake_done = false;
constexpr uint32_t kChannels = 3;  // IMAGE_FORMAT_RGB888

int g_tick = 0;
bool g_simulating = false;

// Tracks the map we last explicitly requested via RESET; used in
// OnLevelInit to distinguish a Python-requested reset from an
// engine-autonomous transition (point_changelevel etc).
std::string g_requested_map;
std::string g_current_map;

// Set true the tick we detect an autonomous level transition starting;
// reported as map_finished on that tick's TICK message, then the plugin
// stops driving input until the next RESET reclaims control (matching
// docs/protocol.md "Level transitions").
bool g_map_finished_pending = false;
bool g_in_loading = true;  // true until ServerActivate first fires

// True until the very first RESET message from Python has been
// received and processed. Only set for the initial launch; subsequent
// resets come through the normal TICK/RESET exchange path (where the
// RESET is already consumed by SendStateAndWait before IssueResetCommand
// is called), so they don't need RecvLine() here.
bool g_need_recv_reset = true;

// True while a map is loading (between IssueResetCommand and the next
// ServerActivate), OR while waiting for the initial map's first ready
// state before sending RESET_ACK. Cleared by OnServerActivate.
bool g_awaiting_reset_ack = true;

enum class MsgType { kAction, kReset, kClose, kUnknown };

struct ClientMessage {
    MsgType type = MsgType::kUnknown;
    RLInput::Action action;
    std::string reset_map;
};

json StateToJson(const RLState::GameState &s) {
    json j;
    j["pos"] = {s.player_pos.x, s.player_pos.y, s.player_pos.z};
    j["vel"] = {s.player_vel.x, s.player_vel.y, s.player_vel.z};
    j["ang"] = {s.player_ang.x, s.player_ang.y, s.player_ang.z};
    j["on_ground"] = s.on_ground;

    j["portal1"] = {
        {"placed", s.portal1.placed},
        {"pos", {s.portal1.pos.x, s.portal1.pos.y, s.portal1.pos.z}},
        {"normal", {s.portal1.normal.x, s.portal1.normal.y, s.portal1.normal.z}},
    };
    j["portal2"] = {
        {"placed", s.portal2.placed},
        {"pos", {s.portal2.pos.x, s.portal2.pos.y, s.portal2.pos.z}},
        {"normal", {s.portal2.normal.x, s.portal2.normal.y, s.portal2.normal.z}},
    };

    j["map"] = s.map_name;
    j["map_finished"] = s.map_finished;
    j["in_loading"] = s.in_loading;
    return j;
}

ClientMessage ParseClientMessage(const std::string &line) {
    ClientMessage msg;

    json j = json::parse(line, nullptr, false);
    if (j.is_discarded()) {
        msg.type = MsgType::kUnknown;
        return msg;
    }

    std::string type = j.value("type", "");
    if (type == "ACTION") {
        msg.type = MsgType::kAction;
        msg.action.move_forward = j.value("move_forward", 0.0f);
        msg.action.move_side = j.value("move_side", 0.0f);
        msg.action.pitch_delta = j.value("pitch_delta", 0.0f);
        msg.action.yaw_delta = j.value("yaw_delta", 0.0f);
        msg.action.jump = j.value("jump", false);
        msg.action.duck = j.value("duck", false);
        msg.action.use = j.value("use", false);
        msg.action.fire_portal1 = j.value("fire_portal1", false);
        msg.action.fire_portal2 = j.value("fire_portal2", false);
    } else if (type == "RESET") {
        msg.type = MsgType::kReset;
        msg.reset_map = j.value("map", "");
    } else if (type == "CLOSE") {
        msg.type = MsgType::kClose;
    }
    return msg;
}

bool DoHandshake() {
    if (!g_socket.AcceptClient()) return false;

    std::string line;
    if (!g_socket.RecvLine(&line)) return false;

    json hello = json::parse(line, nullptr, false);
    if (hello.is_discarded() || hello.value("type", "") != "HELLO") return false;

    g_width = hello.value("width", 1280);
    g_height = hello.value("height", 960);

    // Name the shared memory segment after our control port so multiple
    // Portal 2 instances (each launched with a different
    // +portal2_rl_port) can run concurrently on the same machine without
    // colliding on the same shm segment.
    const std::string shm_name = "portal2_rl_frame_" + std::to_string(g_control_port);
    size_t size = static_cast<size_t>(g_width) * g_height * kChannels;
    if (!g_frame_buf.Create(shm_name, size)) {
        console->Warning("Portal2RL: failed to create shared memory frame buffer\n");
        return false;
    }

    if (!InputProbe::Ready()) {console->Warning("Portal2RL: requested usercmd backend is not ready\n");return false;}
    json ack;
    ack["type"] = "HELLO_ACK";
    ack["shm_name"] = g_frame_buf.ExportName();
    ack["width"] = g_width;
    ack["height"] = g_height;
    ack["channels"] = kChannels;
    ack["input_backend"] = InputProbe::BackendName();
    ack["challenge_requested"] = RLChallenge::Requested();
    ack["sar_submission_blocked"] = RLChallenge::BlockSarSubmission();
    ack["tick_interval"] = (RLEngine::gpGlobals ? RLEngine::gpGlobals->interval_per_tick : 0.0166667f);

    if (!g_socket.SendLine(ack.dump())) return false;

    g_handshake_done = true;
    return true;
}

// Sends a TICK or RESET_ACK message (same payload shape) and blocks for
// the client's reply.
std::optional<ClientMessage> SendStateAndWait(const char *msg_type,
                                                const RLState::GameState &state,
                                                bool frame_ready) {
    if (frame_ready) {
        std::vector<uint8_t> frame;
        uint32_t fw = 0, fh = 0;
        if (RLEngine::ReadScreenPixels(&frame, &fw, &fh) && fw == g_width && fh == g_height) {
            g_frame_buf.Write(frame.data(), frame.size());
        } else {
            frame_ready = false;
        }
    }

    json j;
    j["type"] = msg_type;
    j["tick"] = state.tick;
    j["engine_tick"] = RLEngine::gpGlobals->tickcount;
    j["input_consumed"] = InputProbe::LastConsumed();
    j["input_pending"] = InputProbe::InFlight();
    j["challenge_enabled"] = RLChallenge::Enabled();
    j["challenge_nodes"] = RLChallenge::Nodes();
    j["challenge_finished"] = RLChallenge::Finished();
    j["challenge_hud_hidden"] = RLChallenge::HudHidden();
    j["challenge_seconds"] = RLChallenge::Timer();
    j["simulating"] = g_simulating;
    j["paused"] = RLInput::IsPaused();
    j["frame_ready"] = frame_ready;
    j["state"] = StateToJson(state);

    if (!g_socket.SendLine(j.dump())) return std::nullopt;

    std::string line;
    if (!g_socket.RecvLine(&line)) return std::nullopt;

    return ParseClientMessage(line);
}

void IssueResetCommand(const std::string &map_name) {
    g_requested_map = map_name;
    g_tick = 0;
    g_map_finished_pending = false;
    g_in_loading = true;
    g_awaiting_reset_ack = true;
    g_need_recv_reset = false;  // RESET was already consumed by our caller

    RLInput::ResetAction();

    RLEngine::QueueConsoleCommand("map " + map_name + "\n");
}

}  // namespace

bool Start(uint16_t port) {
    g_control_port = port;
    return g_socket.Listen(port);
}

void OnLevelInit(const char *map_name) {
    std::string new_map = map_name ? map_name : "";

    // If we're not currently mid-reset and the engine loaded a map we
    // didn't request, this is an autonomous transition (e.g.
    // point_changelevel). Mark it so the *next* GameFrame's exchange
    // reports map_finished=true for the previous state before we go
    // in_loading (see docs/protocol.md "Level transitions").
    if (!g_awaiting_reset_ack && !g_current_map.empty() && new_map != g_current_map) {
        g_map_finished_pending = true;
    }

    g_current_map = new_map;
    // Preserve the completed episode's elapsed ticks across an autonomous
    // transition; Python still needs its terminal observation before RESET.
    if (!g_map_finished_pending) g_tick = 0;
    g_in_loading = true;
}

void OnServerActivate() {
    g_in_loading = false;

    // Always (re)install the input hook once entities exist -- harmless
    // to call repeatedly, InstallHookIfNeeded is idempotent.
    void *player = RLEngine::GetLocalPlayer();
    if (player) RLInput::InstallHookIfNeeded(player);

    // Do NOT send RESET_ACK here. ServerActivate can fire before our
    // handshake (DoHandshake, called from OnGameFrame) has even run --
    // e.g. for the very first map loaded from the launch command line,
    // which starts loading before Python ever connects. Sending a
    // protocol message here would race the handshake and/or send on a
    // socket Python isn't listening on yet. Instead, OnGameFrame is the
    // single place that sends RESET_ACK, once both the handshake is done
    // and loading has finished (see below) -- this works whether
    // ServerActivate fired before or after the handshake completed.
}

void OnGameFrame(bool simulating) {
    g_simulating = simulating;
    // A step must advance simulation. CM leaderboard popups used to pause
    // this exchange; hide/block those panels rather than counting paused
    // client frames as simulation steps.
    if (!simulating && !(g_handshake_done && RLChallenge::Finished())) return;

    if (!g_handshake_done) {
        if (!DoHandshake()) return;
        // Handshake just completed. Don't fall through to a normal TICK
        // exchange yet -- if the level already finished loading before
        // the handshake completed (the common case: the launch command
        // line's +map loads while we're blocked in DoHandshake waiting
        // for Python), send RESET_ACK below on this same call rather
        // than waiting for another GameFrame. If the level is still
        // loading, fall through to the g_in_loading check, which returns
        // and waits for a later GameFrame after ServerActivate fires.
    }

    if (g_in_loading) {
        // Still loading -- nothing to exchange this frame. Once
        // ServerActivate fires (g_in_loading becomes false), a later
        // GameFrame call will reach the g_awaiting_reset_ack branch
        // below.
        return;
    }

    if (g_awaiting_reset_ack) {
        std::string requested_map = g_current_map;

        if (g_need_recv_reset) {
            // Initial episode: Python's reset() always sends RESET first,
            // then blocks on RESET_ACK. We must consume that RESET before
            // replying, otherwise Python's message stays in the buffer and
            // gets misread as a reply to something else.
            std::string line;
            if (!g_socket.RecvLine(&line)) return;

            ClientMessage msg = ParseClientMessage(line);
            if (msg.type == MsgType::kReset && !msg.reset_map.empty()) {
                requested_map = msg.reset_map;
            }
            g_need_recv_reset = false;

            // If Python asked for a different map than what's loaded,
            // issue the load and wait for the next cycle.
            // The command-line +map can initialize before plugin loading.
            // Reload once with the Challenge Mode settings hook installed,
            // rather than reporting an enabled cvar without a finish node.
            if (requested_map != g_current_map || (RLChallenge::Requested() && RLChallenge::Nodes()==0)) {
                IssueResetCommand(requested_map);
                return;
            }
        }
        // Subsequent episodes: RESET was already consumed in the normal
        // tick path (SendStateAndWait read it as the reply to the last
        // TICK with map_finished=true), so we go straight to RESET_ACK.

        RLState::GameState state;
        RLState::Populate(&state, g_tick, g_current_map, /*map_finished=*/false,
                           /*in_loading=*/false);

        auto reply = SendStateAndWait("RESET_ACK", state, /*frame_ready=*/true);
        g_awaiting_reset_ack = false;

        if (reply.has_value() && reply->type == MsgType::kAction) {
            RLInput::SetAction(reply->action);
        } else if (reply.has_value() && reply->type == MsgType::kReset) {
            IssueResetCommand(reply->reset_map.empty() ? g_current_map : reply->reset_map);
        }
        return;
    }

    g_tick++;

    // Belt-and-suspenders: install the PlayerRunCommand hook if it
    // somehow wasn't installed via ClientActive (e.g. on a map reload
    // where ClientActive order may differ). Idempotent, safe to call.
    void *player = RLEngine::GetLocalPlayer();
    if (player) RLInput::InstallHookIfNeeded(player);

    RLState::GameState state;
    bool map_finished = g_map_finished_pending;
    RLState::Populate(&state, g_tick, g_current_map, map_finished, /*in_loading=*/false);

    if (map_finished) {
        // Freeze input the instant we detect the transition, mirroring a
        // RESET's input-clearing behavior, since the engine is about to
        // autonomously load the next map regardless of what we send.
        RLInput::ResetAction();
    }

    auto reply = SendStateAndWait("TICK", state, /*frame_ready=*/true);
    if (!reply.has_value() || reply->type == MsgType::kClose) {
        RLInput::ResetAction();
        return;
    }

    if (reply->type == MsgType::kAction) {
        if (!map_finished) {
            RLInput::SetAction(reply->action);
        }
        // If map_finished was true, Python should have sent RESET, not
        // ACTION; we ignore stray ACTIONs in that state since the level
        // is about to change regardless.
    } else if (reply->type == MsgType::kReset) {
        IssueResetCommand(reply->reset_map.empty() ? g_current_map : reply->reset_map);
    }
}

void Shutdown() {
    RLInput::ResetAction();
    g_socket.Close();
    g_frame_buf.Close();
}

}  // namespace RLBridge
