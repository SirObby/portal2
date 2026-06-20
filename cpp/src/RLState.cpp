#include "RLState.hpp"
#include "Engine.hpp"
#include "Entity.hpp"

#include <cstring>
#include <cmath>

namespace RLState {

namespace {

Vec3 ToVec3(const Vector &v) {
    return {v.x, v.y, v.z};
}

Vec3 ToVec3(const QAngle &a) {
    return {a.x, a.y, a.z};
}

// Computes the forward vector for a given yaw/pitch (degrees), matching
// the engine's AngleVectors() convention: portal entities store their
// "facing out of the wall" direction as their angles' forward vector.
Vec3 ForwardFromAngles(const QAngle &ang) {
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    float pitch = ang.x * kDegToRad;
    float yaw = ang.y * kDegToRad;

    float cp = std::cos(pitch);
    float sp = std::sin(pitch);
    float cy = std::cos(yaw);
    float sy = std::sin(yaw);

    return {cp * cy, cp * sy, -sp};
}

// Iterates RLEngine::m_EntPtrArray (same array SAR's PlayerTrace.cpp
// walks) looking for prop_portal entities. The first one found becomes
// portal1 (blue), the second becomes portal2 (orange) -- consistent with
// `m_bIsPortal2` if present, which we read directly when available and
// use as the authoritative signal, falling back to discovery order only
// if the field can't be read.
void FindPortals(PortalState *portal1, PortalState *portal2) {
    *portal1 = PortalState{};
    *portal2 = PortalState{};

    if (!RLEngine::m_EntPtrArray || !RLEngine::gpGlobals) return;

    int max_ents = RLEngine::gpGlobals->maxEntities;

    for (int i = 0; i < max_ents; ++i) {
        void *ent = RLEngine::m_EntPtrArray[i].m_pEntity;
        if (!ent) continue;

        char *classname = SE(ent)->classname();
        if (!classname || std::strcmp(classname, "prop_portal") != 0) continue;

        bool activated = SE(ent)->field<bool>("m_bActivated");
        if (!activated) continue;

        bool is_portal2 = SE(ent)->field<bool>("m_bIsPortal2");

        PortalState state;
        state.placed = true;
        state.pos = ToVec3(SE(ent)->abs_origin());
        state.normal = ForwardFromAngles(SE(ent)->abs_angles());

        if (is_portal2) {
            *portal2 = state;
        } else {
            *portal1 = state;
        }
    }
}

}  // namespace

void Populate(GameState *state, int tick, const std::string &map_name,
              bool map_finished, bool in_loading) {
    state->tick = tick;
    state->map_name = map_name;
    state->map_finished = map_finished;
    state->in_loading = in_loading;

    if (in_loading) {
        // Entities may be mid-spawn/destroy during a level transition;
        // report zeroed state and let the caller mark frame/state
        // unreliable via in_loading=true (see docs/protocol.md).
        return;
    }

    void *player = RLEngine::GetLocalPlayer();
    if (player) {
        state->player_pos = ToVec3(SE(player)->abs_origin());
        state->player_vel = ToVec3(SE(player)->abs_velocity());

        // Eye angles: the player's view direction is stored on `pl.v_angle`
        // (CPlayerState, see Utils/SDK/Other.hpp CPlayerState), which is
        // what the engine actually uses for the camera/aim direction --
        // distinct from abs_angles() (the entity's model orientation,
        // which Portal 2's player model doesn't meaningfully rotate on
        // pitch).
        state->player_ang = ToVec3(SE(player)->player_state().v_angle);

        int flags = SE(player)->flags();
        state->on_ground = (flags & FL_ONGROUND) != 0;
    }

    FindPortals(&state->portal1, &state->portal2);
}

}  // namespace RLState
