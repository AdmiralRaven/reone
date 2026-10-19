/*
 * Copyright (c) 2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "reone/game/jasabermoves.h"

using namespace reone::graphics;

namespace reone {

namespace game {

namespace {

// Quadrants as animation.cfg spells them in clip names, and as the move
// names spell them in attacks.
const std::array<const char *, 8> kQuadCodes {"BR", "_R", "TR", "T_", "TL", "_L", "BL", "B_"};
const std::array<const char *, 8> kQuadNames {"BR", "R", "TR", "T", "TL", "L", "BL", "B"};

// The fast style's spins, from one quadrant to another, as clip names spell
// them (PM_SpinningSaberAnim, bg_pmove.cpp:7022-7038).
const std::array<const char *, 12> kSpins {
    "BR_BL", "_R__L", "_R_BL", "TR_BL", "BR_TL", "BR__L", "TL_BR", "_L_BR", "_L__R", "BL_BR", "BL__R", "BL_TR"};

struct AttackSpec {
    JaQuad start;
    JaQuad end;
    int blendMs;
};

// The seven ordinary attacks, in the table's order (bg_panimate.cpp:133-145).
const std::array<AttackSpec, 7> kAttacks {{
    {JaQuad::TL, JaQuad::BR, 100},
    {JaQuad::L, JaQuad::R, 100},
    {JaQuad::BL, JaQuad::TR, 50},
    {JaQuad::BR, JaQuad::TL, 100},
    {JaQuad::R, JaQuad::L, 100},
    {JaQuad::TR, JaQuad::BL, 100},
    {JaQuad::T, JaQuad::B, 100},
}};

constexpr int kNumAttacks = 7;
constexpr int kFirstAttack = 1;
constexpr int kFirstStart = kFirstAttack + kNumAttacks;
constexpr int kFirstReturn = kFirstStart + kNumAttacks;
constexpr int kFirstTransition = kFirstReturn + kNumAttacks;
constexpr int kTransitionSources = 7; // every quadrant but the bottom
constexpr int kNumMoves = kFirstTransition + kTransitionSources * (kTransitionSources - 1);

constexpr int kReadyBlendMs = 350; // bg_panimate.cpp:127
constexpr int kMoveBlendMs = 100;  // starts, returns and transitions (205-264)
constexpr float kFastTransitionScale = 1.5f;

int quadIndex(JaQuad quad) {
    return static_cast<int>(quad);
}

int transitionId(int from, int to) {
    return kFirstTransition + from * (kTransitionSources - 1) + (to < from ? to : to - 1);
}

int sign(int value) {
    return (value > 0) - (value < 0);
}

} // namespace

const JaSaberMoves &JaSaberMoves::fast() {
    static const JaSaberMoves moves;
    return moves;
}

JaSaberMoves::JaSaberMoves() {
    _moves.resize(kNumMoves);

    JaSaberMove &ready = _moves[0];
    ready.id = 0;
    ready.name = "LS_READY";
    ready.clip = "BOTH_SABERFAST_STANCE";
    ready.kind = JaMoveKind::Ready;
    ready.hold = JaMoveHold::Idle;
    ready.blendMs = kReadyBlendMs;

    for (int a = 0; a < kNumAttacks; ++a) {
        const AttackSpec &spec = kAttacks[a];
        int start = quadIndex(spec.start);
        int end = quadIndex(spec.end);
        std::string name = std::string(kQuadNames[start]) + "2" + kQuadNames[end];

        JaSaberMove &attack = _moves[kFirstAttack + a];
        attack.id = kFirstAttack + a;
        attack.name = "LS_A_" + name;
        attack.clip = std::string("BOTH_A1_") + kQuadCodes[start] + "_" + kQuadCodes[end];
        attack.kind = JaMoveKind::Attack;
        attack.startQuad = spec.start;
        attack.endQuad = spec.end;
        attack.hold = JaMoveHold::Active;
        attack.blendMs = spec.blendMs;
        attack.chainAttack = kFirstReturn + a;

        JaSaberMove &startMove = _moves[kFirstStart + a];
        startMove.id = kFirstStart + a;
        startMove.name = "LS_S_" + name;
        startMove.clip = std::string("BOTH_S1_S1_") + kQuadCodes[start];
        startMove.kind = JaMoveKind::Start;
        startMove.startQuad = JaQuad::R;
        startMove.endQuad = spec.start;
        startMove.hold = JaMoveHold::Active;
        startMove.blendMs = kMoveBlendMs;
        startMove.chainAttack = attack.id;

        JaSaberMove &ret = _moves[kFirstReturn + a];
        ret.id = kFirstReturn + a;
        ret.name = "LS_R_" + name;
        ret.clip = std::string("BOTH_R1_") + kQuadCodes[end] + "_S1";
        ret.kind = JaMoveKind::Return;
        ret.startQuad = spec.end;
        ret.endQuad = JaQuad::R;
        ret.hold = JaMoveHold::Finish;
        ret.blendMs = kMoveBlendMs;
        ret.chainAttack = ready.id;
    }

    for (int from = 0; from < kTransitionSources; ++from) {
        for (int to = 0; to < kTransitionSources; ++to) {
            if (to == from) {
                continue;
            }
            std::string name = std::string(kQuadCodes[from]) + "_" + kQuadCodes[to];
            JaSaberMove &move = _moves[transitionId(from, to)];
            move.id = transitionId(from, to);
            move.name = "LS_T1_" + name;
            move.clip = "BOTH_T1_" + name;
            move.kind = JaMoveKind::Transition;
            move.startQuad = static_cast<JaQuad>(from);
            move.endQuad = static_cast<JaQuad>(to);
            move.hold = JaMoveHold::Active;
            move.blendMs = kMoveBlendMs;
            move.timeScale = kFastTransitionScale;
            move.spin = std::find(kSpins.begin(), kSpins.end(), name) != kSpins.end();
            // A transition goes on to the attack that starts where it ends.
            move.chainAttack = attackFrom(move.endQuad);
        }
    }

    // Pressing attack with no direction from ready swings right to left (bg_panimate.cpp:127).
    ready.chainAttack = startOf(attackFrom(JaQuad::R));

    for (auto &move : _moves) {
        _byName.insert({move.name, move.id});
    }
}

const JaSaberMove &JaSaberMoves::move(int id) const {
    if (id < 0 || id >= static_cast<int>(_moves.size())) {
        throw std::out_of_range("No saber move " + std::to_string(id));
    }
    return _moves[id];
}

int JaSaberMoves::find(const std::string &name) const {
    auto it = _byName.find(name);
    return it != _byName.end() ? it->second : kNone;
}

int JaSaberMoves::attackFrom(JaQuad quad) const {
    for (int a = 0; a < kNumAttacks; ++a) {
        if (kAttacks[a].start == quad) {
            return kFirstAttack + a;
        }
    }
    return kNone;
}

int JaSaberMoves::startOf(int attack) const {
    if (move(attack).kind != JaMoveKind::Attack) {
        throw std::invalid_argument("Not an attack: " + move(attack).name);
    }
    return kFirstStart + (attack - kFirstAttack);
}

int JaSaberMoves::returnOf(int attack) const {
    if (move(attack).kind != JaMoveKind::Attack) {
        throw std::invalid_argument("Not an attack: " + move(attack).name);
    }
    return kFirstReturn + (attack - kFirstAttack);
}

int JaSaberMoves::attackOf(int startOrReturn) const {
    switch (move(startOrReturn).kind) {
    case JaMoveKind::Start:
        return kFirstAttack + (startOrReturn - kFirstStart);
    case JaMoveKind::Return:
        return kFirstAttack + (startOrReturn - kFirstReturn);
    default:
        throw std::invalid_argument("Neither a start nor a return: " + move(startOrReturn).name);
    }
}

int JaSaberMoves::transition(JaQuad from, JaQuad to) const {
    if (to == JaQuad::B || from == to) {
        return kNone;
    }
    if (from == JaQuad::B) {
        // The bottom's row is the bottom right's, but for the bottom right
        // itself (bg_panimate.cpp:399-406).
        from = to == JaQuad::BR ? JaQuad::BL : JaQuad::BR;
    }
    return transitionId(quadIndex(from), quadIndex(to));
}

int JaSaberMoves::attackForMovement(int forward, int right, float pitchDegrees, int current) const {
    forward = sign(forward);
    right = sign(right);
    if (right > 0) {
        // bg_panimate.cpp:3546-3557
        return attackFrom(forward > 0 ? JaQuad::TL : (forward < 0 ? JaQuad::BL : JaQuad::L));
    }
    if (right < 0) {
        // bg_panimate.cpp:3611-3640
        return attackFrom(forward > 0 ? JaQuad::TR : (forward < 0 ? JaQuad::BR : JaQuad::R));
    }
    if (forward != 0) {
        // Forward or back alone: top to bottom; there is no plain uppercut (3732, 3873).
        return attackFrom(JaQuad::T);
    }
    if (current == ready() && std::abs(pitchDegrees) > 30.0f) {
        return attackFrom(JaQuad::T);
    }
    return kNone;
}

int JaSaberMoves::nextMove(int current, int attack) const {
    const JaSaberMove &next = move(attack);
    if (next.kind != JaMoveKind::Attack) {
        throw std::invalid_argument("Not an attack: " + next.name);
    }
    const JaSaberMove &from = move(current);
    switch (from.kind) {
    case JaMoveKind::Ready:
        // Only ready plays a start (bg_panimate.cpp:4001-4018).
        return startOf(attack);
    case JaMoveKind::Attack:
    case JaMoveKind::Return: {
        // bg_panimate.cpp:4050-4146: straight on when the attack starts where
        // this move ends, else by the transition there.
        if (from.endQuad == next.startQuad) {
            return attack;
        }
        int via = transition(from.endQuad, next.startQuad);
        return via != kNone ? via : attack;
    }
    default:
        return attack;
    }
}

int jaFrameLerp(float fps) {
    if (fps == 0.0f) {
        fps = 1.0f;
    }
    return fps < 0.0f ? static_cast<int>(std::floor(1000.0f / fps)) : static_cast<int>(std::ceil(1000.0f / fps));
}

double jaCountDown(double timerMs, float dt) {
    // In double precision, so that the rounding stays far below the slack
    // however long the hold.
    double left = timerMs - std::max(0.0, static_cast<double>(dt) * 1000.0);
    return left < kJaTimerSlackMs ? 0.0 : left;
}

// The rate JaSequencer samples a clip at.
static float ownRate(const GhoulAnimationClip &clip) {
    return clip.fps != 0.0f ? std::abs(clip.fps) : 20.0f;
}

JaMoveTiming jaMoveTiming(const JaSaberMove &move, const GhoulAnimationClip &clip) {
    int frameMs = std::abs(jaFrameLerp(clip.fps));
    JaMoveTiming timing;
    timing.durationMs = static_cast<int>(static_cast<float>(std::max(0, clip.numFrames - 1) * frameMs) / move.timeScale);
    switch (move.hold) {
    case JaMoveHold::Active:
        // SETANIM_FLAG_HOLDLESS takes a millisecond off at normal speed.
        if (move.timeScale == 1.0f) {
            timing.holdMs = timing.durationMs > 1 ? timing.durationMs - 1 : frameMs;
        } else {
            timing.holdMs = timing.durationMs;
        }
        break;
    case JaMoveHold::Finish:
        timing.holdMs = timing.durationMs;
        break;
    default:
        timing.holdMs = 0;
        break;
    }
    timing.rate = 1000.0f / static_cast<float>(frameMs) * move.timeScale / ownRate(clip);
    timing.blendMs = clip.fps < 0.0f ? 0 : move.blendMs;
    return timing;
}

JaSaberStateMachine::JaSaberStateMachine(
    const JaSaberMoves &moves,
    const std::map<std::string, GhoulAnimationClip> &clips,
    int fireTimeMs) :
    _moves(moves),
    _fireTimeMs(fireTimeMs) {
    std::vector<std::string> missing;
    for (auto &move : _moves.moves()) {
        auto clip = clips.find(boost::to_upper_copy(move.clip));
        if (clip == clips.end() || clip->second.numFrames < 1) {
            missing.push_back(move.clip);
            _timings.emplace_back();
            continue;
        }
        _timings.push_back(jaMoveTiming(move, clip->second));
    }
    if (!missing.empty()) {
        throw std::runtime_error("The JA animations lack saber clips: " + boost::join(missing, ", "));
    }
    reset();
}

void JaSaberStateMachine::reset() {
    _move = _moves.ready();
    _torsoTimer = 0.0;
    _weaponTime = 0.0;
}

bool JaSaberStateMachine::set(int id, std::vector<int> &started) {
    const JaSaberMove &move = _moves.move(id);
    if (id == _move && move.kind == JaMoveKind::Ready) {
        return true;
    }
    // Only a move that cuts in may replace a clip that still holds the body
    // (bg_panimate.cpp:4866; bg_pmove.cpp:9528, 9637-9638).
    if (move.hold != JaMoveHold::Active && _torsoTimer > 0.0) {
        return false;
    }
    _move = id;
    _torsoTimer = static_cast<double>(_timings[id].holdMs);
    started.push_back(id);
    return true;
}

std::vector<int> JaSaberStateMachine::tick(float dt, const JaSaberInput &input) {
    std::vector<int> started;
    // PM_DropTimers and PM_WeaponLightsaber (bg_pmove.cpp:14320-14361, 11947-11953)
    _torsoTimer = jaCountDown(_torsoTimer, dt);
    _weaponTime = jaCountDown(_weaponTime, dt);
    bool direction = input.forward != 0 || input.right != 0;
    const JaSaberMove &current = _moves.move(_move);

    if (_weaponTime > 0.0) {
        // Busy (bg_pmove.cpp:12224-12276). Moving during a return with the
        // button up frees the saber, though the return plays on (12231-12244,
        // 12497, 12541-12549).
        if (current.kind == JaMoveKind::Return && !input.attack && direction) {
            _weaponTime = 0.0;
        }
    } else if (!input.attack) {
        // The button is up as the move ends (12495-12550).
        _weaponTime = 0.0;
        int next = JaSaberMoves::kNone;
        switch (current.kind) {
        case JaMoveKind::Start:
            next = _moves.attackOf(_move);
            break;
        case JaMoveKind::Attack:
            next = _moves.returnOf(_move);
            break;
        case JaMoveKind::Transition:
            next = current.chainAttack;
            break;
        default:
            break;
        }
        if (next != JaSaberMoves::kNone) {
            set(next, started);
            _weaponTime = _torsoTimer;
        }
    } else {
        // The button is held as the move ends (12589-12769). Starts and
        // transitions go on to their own attack without reading the direction.
        int next;
        if (current.kind == JaMoveKind::Transition) {
            next = current.chainAttack;
        } else if (current.kind == JaMoveKind::Start) {
            next = _moves.attackOf(_move);
        } else {
            next = _moves.attackForMovement(input.forward, input.right, input.pitch, _move);
            next = next != JaSaberMoves::kNone ? _moves.nextMove(_move, next) : current.chainAttack;
        }
        set(next, started);
        // 12787-12791, 12806-12889
        _weaponTime = _torsoTimer > 0.0 ? _torsoTimer : static_cast<double>(_fireTimeMs);
    }

    // The idle torso (bg_panimate.cpp:5200-5223): a return goes back to ready
    // once the saber is free and the return has run out.
    if (_weaponTime <= 0.0 && _moves.move(_move).kind == JaMoveKind::Return) {
        set(_moves.ready(), started);
    }
    return started;
}

float jaLegsYawTarget(int forward, int right) {
    forward = sign(forward);
    right = sign(right);
    if (forward == 0 && right == 0) {
        return 0.0f;
    }
    float angle = glm::degrees(std::atan2(static_cast<float>(-right), static_cast<float>(forward)));
    if (std::abs(angle) > 150.0f) {
        return 0.0f;
    }
    angle = glm::clamp(angle, -60.0f, 60.0f);
    // Running backwards, the legs turn the other way (cg_players.cpp:1888-1891).
    return forward < 0 ? -angle : angle;
}

std::string jaLocomotionClip(int forward, bool walk) {
    if (forward < 0) {
        return walk ? "BOTH_WALKBACK2" : "BOTH_RUNBACK2";
    }
    return walk ? "BOTH_WALK2" : "BOTH_RUN2";
}

float jaFootSlideRate(const GhoulAnimationClip &clip, bool walk, float unitsPerSecond) {
    int frameMs = std::abs(jaFrameLerp(clip.fps));
    float speed = 50.0f / static_cast<float>(frameMs); // Ghoul2's units: 1 is 20 frames a second
    if (clip.fps > 0.0f) {
        speed *= unitsPerSecond / (walk ? 50.0f : 150.0f);
        speed = glm::clamp(speed, 0.01f, 1.5f);
    }
    return speed * 20.0f / ownRate(clip);
}

} // namespace game

} // namespace reone
