// RLState.hpp: extracts the per-tick observation (player + portal state)
// using the named-field datamap accessors from Entity.hpp (SE(ent)->...),
// the same mechanism SourceAutoRecord uses, which resolves field offsets
// by name at runtime via the engine's own datamap/SendTable reflection --
// robust across minor game updates since it doesn't hardcode byte
// offsets for entity fields (only for the handful of engine/interface
// offsets in Offsets.hpp, which are looked up once per build).

#pragma once

#include <string>
#include <cstdint>

namespace RLState {

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;
};

struct PortalState {
    bool placed = false;
    Vec3 pos;
    Vec3 normal;
};

struct GameState {
    int tick = 0;

    Vec3 player_pos;
    Vec3 player_vel;
    Vec3 player_ang;  // pitch, yaw, roll (degrees)
    bool on_ground = false;

    PortalState portal1;  // blue
    PortalState portal2;  // orange

    std::string map_name;
    bool map_finished = false;
    bool in_loading = false;
};

// Populates `state` from the local player entity and any prop_portal
// entities found by iterating RLEngine::m_EntPtrArray. `tick`, `map_name`,
// `map_finished`, and `in_loading` are supplied by the caller (RLBridge),
// which tracks tick counters and map-transition status independently.
void Populate(GameState *state, int tick, const std::string &map_name,
              bool map_finished, bool in_loading);

}  // namespace RLState
