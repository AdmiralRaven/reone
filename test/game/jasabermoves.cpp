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

#include <gtest/gtest.h>

#include "reone/game/jasabermoves.h"

using namespace reone;
using namespace reone::game;
using namespace reone::graphics;

namespace {

const JaSaberMoves &moves() {
    return JaSaberMoves::fast();
}

int id(const std::string &name) {
    int found = moves().find(name);
    if (found == JaSaberMoves::kNone) {
        throw std::logic_error("no move " + name);
    }
    return found;
}

std::string name(int move) {
    return move == JaSaberMoves::kNone ? std::string("none") : moves().move(move).name;
}

// A clip for every move: 7 frames at 20 fps unless given, the transitions
// from the right to the bottom right and from the top right to the right
// played in reverse, as animation.cfg plays them.
std::map<std::string, GhoulAnimationClip> clipsForAllMoves(const std::map<std::string, GhoulAnimationClip> &given = {}) {
    std::map<std::string, GhoulAnimationClip> clips;
    for (auto &move : moves().moves()) {
        clips[move.clip] = {0, 7, -1, 20.0f};
    }
    clips["BOTH_T1__R_BR"] = {0, 4, -1, -20.0f};
    clips["BOTH_T1_TR__R"] = {0, 4, -1, -20.0f};
    for (auto &[clip, value] : given) {
        clips[clip] = value;
    }
    return clips;
}

// The swing #21 names, with the frame counts and rates of Jedi Academy's
// animation.cfg (#24's spec, section e).
std::map<std::string, GhoulAnimationClip> namedSwingClips() {
    return clipsForAllMoves({{"BOTH_S1_S1_TL", {12464, 8, -1, 20.0f}},
                             {"BOTH_A1_TL_BR", {87, 6, -1, 30.0f}},
                             {"BOTH_R1_BR_S1", {11110, 6, -1, 30.0f}},
                             {"BOTH_T1_BR_TL", {16269, 13, -1, 20.0f}},
                             {"BOTH_A1_BR_TL", {24, 6, -1, 30.0f}},
                             {"BOTH_SABERFAST_STANCE", {13052, 2, -1, 60.0f}}});
}

struct Started {
    int timeMs;
    std::string move;

    bool operator==(const Started &other) const {
        return timeMs == other.timeMs && move == other.move;
    }
};

std::ostream &operator<<(std::ostream &out, const Started &started) {
    return out << started.move << " at " << started.timeMs << " ms";
}

// Runs the machine in 1 ms ticks; input(t) gives the input at t ms.
std::vector<Started> run(JaSaberStateMachine &machine, int untilMs, const std::function<JaSaberInput(int)> &input) {
    std::vector<Started> result;
    for (int t = 0; t <= untilMs; ++t) {
        for (int move : machine.tick(t == 0 ? 0.0f : 0.001f, input(t))) {
            result.push_back({t, name(move)});
        }
    }
    return result;
}

JaSaberInput holding(bool attack, int forward, int right) {
    JaSaberInput input;
    input.attack = attack;
    input.forward = forward;
    input.right = right;
    return input;
}

} // namespace

TEST(JaSaberMoves, should_build_the_fast_styles_moves_from_their_quadrants) {
    // 64 for attacking, and 10 parries: five of blows, five of bolts
    EXPECT_EQ(74ll, moves().moves().size());
    std::set<std::string> names;
    for (auto &move : moves().moves()) {
        EXPECT_EQ(move.id, moves().find(move.name));
        names.insert(move.name);
    }
    EXPECT_EQ(74ll, names.size());

    const auto &ready = moves().move(moves().ready());
    EXPECT_EQ(std::string("LS_READY"), ready.name);
    EXPECT_EQ(std::string("BOTH_SABERFAST_STANCE"), ready.clip);
    EXPECT_EQ(350, ready.blendMs);
    EXPECT_EQ(JaMoveHold::Idle, ready.hold);
    EXPECT_EQ(std::string("LS_S_R2L"), name(ready.chainAttack));

    const auto &attack = moves().move(id("LS_A_TL2BR"));
    EXPECT_EQ(std::string("BOTH_A1_TL_BR"), attack.clip);
    EXPECT_EQ(JaQuad::TL, attack.startQuad);
    EXPECT_EQ(JaQuad::BR, attack.endQuad);
    EXPECT_EQ(JaMoveHold::Active, attack.hold);
    EXPECT_EQ(100, attack.blendMs);
    EXPECT_EQ(std::string("LS_R_TL2BR"), name(attack.chainAttack));
    EXPECT_EQ(50, moves().move(id("LS_A_BL2TR")).blendMs);
    EXPECT_EQ(std::string("BOTH_A1__L__R"), moves().move(id("LS_A_L2R")).clip);
    EXPECT_EQ(std::string("BOTH_A1_T__B_"), moves().move(id("LS_A_T2B")).clip);

    const auto &start = moves().move(id("LS_S_TL2BR"));
    EXPECT_EQ(std::string("BOTH_S1_S1_TL"), start.clip);
    EXPECT_EQ(JaQuad::R, start.startQuad);
    EXPECT_EQ(JaQuad::TL, start.endQuad);
    EXPECT_EQ(std::string("BOTH_S1_S1_T_"), moves().move(id("LS_S_T2B")).clip);

    const auto &ret = moves().move(id("LS_R_TL2BR"));
    EXPECT_EQ(std::string("BOTH_R1_BR_S1"), ret.clip);
    EXPECT_EQ(JaMoveHold::Finish, ret.hold);
    EXPECT_EQ(std::string("LS_READY"), name(ret.chainAttack));
    EXPECT_EQ(std::string("BOTH_R1_B__S1"), moves().move(id("LS_R_T2B")).clip);

    const auto &transition = moves().move(id("LS_T1_BR__R"));
    EXPECT_EQ(std::string("BOTH_T1_BR__R"), transition.clip);
    EXPECT_FLOAT_EQ(1.5f, transition.timeScale);
    EXPECT_EQ(std::string("LS_A_R2L"), name(transition.chainAttack));
    EXPECT_EQ(std::string("BOTH_T1_T___R"), moves().move(id("LS_T1_T___R")).clip);

    // The table's own consistency: starts end where their attack starts, attacks
    // end where their return starts, returns end on the right, transitions lead
    // to the attack that starts where they end.
    for (auto &move : moves().moves()) {
        switch (move.kind) {
        case JaMoveKind::Attack:
            EXPECT_EQ(move.startQuad, moves().move(moves().startOf(move.id)).endQuad) << move.name;
            EXPECT_EQ(move.endQuad, moves().move(moves().returnOf(move.id)).startQuad) << move.name;
            break;
        case JaMoveKind::Return:
            EXPECT_EQ(JaQuad::R, move.endQuad) << move.name;
            break;
        case JaMoveKind::Transition:
            EXPECT_EQ(move.endQuad, moves().move(move.chainAttack).startQuad) << move.name;
            EXPECT_EQ(move.id, moves().transition(move.startQuad, move.endQuad)) << move.name;
            break;
        default:
            break;
        }
    }
}

TEST(JaSaberMoves, should_pick_the_attack_from_the_held_direction) {
    int ready = moves().ready();
    EXPECT_EQ("LS_A_TL2BR", name(moves().attackForMovement(1, 1, 0.0f, ready)));
    EXPECT_EQ("LS_A_L2R", name(moves().attackForMovement(0, 1, 0.0f, ready)));
    EXPECT_EQ("LS_A_BL2TR", name(moves().attackForMovement(-1, 1, 0.0f, ready)));
    EXPECT_EQ("LS_A_TR2BL", name(moves().attackForMovement(1, -1, 0.0f, ready)));
    EXPECT_EQ("LS_A_R2L", name(moves().attackForMovement(0, -1, 0.0f, ready)));
    EXPECT_EQ("LS_A_BR2TL", name(moves().attackForMovement(-1, -1, 0.0f, ready)));
    EXPECT_EQ("LS_A_T2B", name(moves().attackForMovement(1, 0, 0.0f, ready)));
    EXPECT_EQ("LS_A_T2B", name(moves().attackForMovement(-1, 0, 0.0f, ready)));
    // Only the signs count.
    EXPECT_EQ("LS_A_TL2BR", name(moves().attackForMovement(127, 64, 0.0f, ready)));
    // No direction: none from ready, unless looking well up or down; none in a chain.
    EXPECT_EQ("none", name(moves().attackForMovement(0, 0, 0.0f, ready)));
    EXPECT_EQ("none", name(moves().attackForMovement(0, 0, 30.0f, ready)));
    EXPECT_EQ("LS_A_T2B", name(moves().attackForMovement(0, 0, 31.0f, ready)));
    EXPECT_EQ("LS_A_T2B", name(moves().attackForMovement(0, 0, -31.0f, ready)));
    EXPECT_EQ("none", name(moves().attackForMovement(0, 0, 45.0f, id("LS_A_TL2BR"))));
}

TEST(JaSaberMoves, should_chain_straight_on_only_where_the_next_attack_starts) {
    int ready = moves().ready();
    EXPECT_EQ("LS_S_TL2BR", name(moves().nextMove(ready, id("LS_A_TL2BR"))));

    std::set<std::pair<std::string, std::string>> direct;
    std::vector<int> attacks;
    for (auto &move : moves().moves()) {
        if (move.kind == JaMoveKind::Attack) {
            attacks.push_back(move.id);
        }
    }
    for (int from : attacks) {
        for (int to : attacks) {
            int next = moves().nextMove(from, to);
            if (next == to) {
                direct.insert({name(from), name(to)});
            } else {
                EXPECT_EQ(JaMoveKind::Transition, moves().move(next).kind) << name(from) << " to " << name(to);
            }
        }
    }
    std::set<std::pair<std::string, std::string>> expected {
        {"LS_A_TL2BR", "LS_A_BR2TL"}, {"LS_A_BR2TL", "LS_A_TL2BR"}, {"LS_A_L2R", "LS_A_R2L"}, {"LS_A_R2L", "LS_A_L2R"}, {"LS_A_BL2TR", "LS_A_TR2BL"}, {"LS_A_TR2BL", "LS_A_BL2TR"}};
    EXPECT_EQ(expected, direct);

    // The same attack again goes round through a transition.
    EXPECT_EQ("LS_T1_BR_TL", name(moves().nextMove(id("LS_A_TL2BR"), id("LS_A_TL2BR"))));
    // From the bottom, the bottom right's transitions, but to the bottom right itself.
    EXPECT_EQ("LS_T1_BR__L", name(moves().nextMove(id("LS_A_T2B"), id("LS_A_L2R"))));
    EXPECT_EQ("LS_T1_BL_BR", name(moves().nextMove(id("LS_A_T2B"), id("LS_A_BR2TL"))));
    // Returns end on the right.
    EXPECT_EQ("LS_A_R2L", name(moves().nextMove(id("LS_R_TL2BR"), id("LS_A_R2L"))));
    EXPECT_EQ("LS_T1__R_TL", name(moves().nextMove(id("LS_R_TL2BR"), id("LS_A_TL2BR"))));
    // Starts and transitions go straight on.
    EXPECT_EQ("LS_A_L2R", name(moves().nextMove(id("LS_S_TL2BR"), id("LS_A_L2R"))));
    EXPECT_EQ("none", name(moves().transition(JaQuad::T, JaQuad::T)));
    EXPECT_EQ("none", name(moves().transition(JaQuad::BR, JaQuad::B)));
}

TEST(JaSaberMoves, should_mark_the_fast_styles_spins) {
    // PM_SpinningSaberAnim's clips for the fast style (bg_pmove.cpp:7027-7038)
    std::set<std::string> expected {
        "BOTH_T1_BR_BL", "BOTH_T1__R__L", "BOTH_T1__R_BL", "BOTH_T1_TR_BL", "BOTH_T1_BR_TL", "BOTH_T1_BR__L",
        "BOTH_T1_TL_BR", "BOTH_T1__L_BR", "BOTH_T1__L__R", "BOTH_T1_BL_BR", "BOTH_T1_BL__R", "BOTH_T1_BL_TR"};
    std::set<std::string> spins;
    for (auto &move : moves().moves()) {
        if (move.spin) {
            EXPECT_EQ(JaMoveKind::Transition, move.kind) << move.name;
            spins.insert(move.clip);
        }
    }
    EXPECT_EQ(expected, spins);

    // Holding forward and right goes round through one, as can going on from
    // the bottom; going on from a return to the same attack doesn't.
    EXPECT_TRUE(moves().move(moves().nextMove(id("LS_A_TL2BR"), id("LS_A_TL2BR"))).spin);
    EXPECT_TRUE(moves().move(moves().nextMove(id("LS_A_T2B"), id("LS_A_BR2TL"))).spin);
    EXPECT_FALSE(moves().move(moves().nextMove(id("LS_R_TL2BR"), id("LS_A_TL2BR"))).spin);
}

TEST(JaSaberMoves, should_time_moves_as_ghoul2_holds_them) {
    EXPECT_EQ(34, jaFrameLerp(30.0f));
    EXPECT_EQ(50, jaFrameLerp(20.0f));
    EXPECT_EQ(17, jaFrameLerp(60.0f));
    EXPECT_EQ(-50, jaFrameLerp(-20.0f));
    EXPECT_EQ(-67, jaFrameLerp(-15.0f));
    EXPECT_EQ(1000, jaFrameLerp(0.0f));

    auto timing = [](const std::string &move, GhoulAnimationClip clip) {
        return jaMoveTiming(moves().move(id(move)), clip);
    };
    // An attack: 5 frames of 34 ms, held a millisecond less.
    auto attack = timing("LS_A_TL2BR", {87, 6, -1, 30.0f});
    EXPECT_EQ(170, attack.durationMs);
    EXPECT_EQ(169, attack.holdMs);
    EXPECT_NEAR(1000.0f / 34.0f / 30.0f, attack.rate, 1e-6f);
    EXPECT_EQ(100, attack.blendMs);
    // A return is held for its whole length.
    EXPECT_EQ(170, timing("LS_R_TL2BR", {11110, 6, -1, 30.0f}).holdMs);
    EXPECT_EQ(349, timing("LS_S_TL2BR", {12464, 8, -1, 20.0f}).holdMs);
    // Transitions play half as fast again, and are held in full.
    auto spin = timing("LS_T1_BR_TL", {16269, 13, -1, 20.0f});
    EXPECT_EQ(400, spin.holdMs);
    EXPECT_FLOAT_EQ(1.5f, spin.rate);
    EXPECT_EQ(366, timing("LS_T1_TR_BL", {16347, 12, -1, 20.0f}).holdMs);
    // Played in reverse, without a blend.
    auto reversed = timing("LS_T1__R_BR", {16312, 4, -1, -20.0f});
    EXPECT_EQ(100, reversed.holdMs);
    EXPECT_EQ(0, reversed.blendMs);
    EXPECT_FLOAT_EQ(1.5f, reversed.rate);
    // The ready stance holds nothing.
    auto ready = timing("LS_READY", {13052, 2, -1, 60.0f});
    EXPECT_EQ(17, ready.durationMs);
    EXPECT_EQ(0, ready.holdMs);
    EXPECT_EQ(350, ready.blendMs);
    // A one-frame attack still holds a frame.
    EXPECT_EQ(50, timing("LS_A_TL2BR", {0, 1, -1, 20.0f}).holdMs);
}

TEST(JaSaberStateMachine, should_name_missing_clips) {
    auto clips = clipsForAllMoves();
    clips.erase("BOTH_T1_BR_TL");
    clips.erase("BOTH_SABERFAST_STANCE");
    try {
        JaSaberStateMachine machine(moves(), clips);
        FAIL() << "expected an error";
    } catch (const std::runtime_error &e) {
        std::string message(e.what());
        EXPECT_NE(std::string::npos, message.find("BOTH_T1_BR_TL")) << message;
        EXPECT_NE(std::string::npos, message.find("BOTH_SABERFAST_STANCE")) << message;
    }
}

TEST(JaSaberStateMachine, should_swing_once_and_return_when_the_button_is_tapped) {
    // given
    JaSaberStateMachine machine(moves(), namedSwingClips());

    // when: forward and right with the button, for one tick
    auto started = run(machine, 1000, [](int t) { return t == 0 ? holding(true, 1, 1) : JaSaberInput(); });

    // then: start, attack, return, ready (#24's spec, section c.8)
    std::vector<Started> expected {{0, "LS_S_TL2BR"}, {349, "LS_A_TL2BR"}, {518, "LS_R_TL2BR"}, {688, "LS_READY"}};
    EXPECT_EQ(expected, started);
}

TEST(JaSaberStateMachine, should_go_round_again_while_the_same_direction_is_held) {
    JaSaberStateMachine machine(moves(), namedSwingClips());

    auto started = run(machine, 1200, [](int) { return holding(true, 1, 1); });

    std::vector<Started> expected {{0, "LS_S_TL2BR"}, {349, "LS_A_TL2BR"}, {518, "LS_T1_BR_TL"}, {918, "LS_A_TL2BR"}, {1087, "LS_T1_BR_TL"}};
    EXPECT_EQ(expected, started);
}

TEST(JaSaberStateMachine, should_chain_opposite_attacks_straight_on) {
    JaSaberStateMachine machine(moves(), namedSwingClips());

    // forward and right until the first attack ends, then back and left, then
    // forward and right again
    auto started = run(machine, 860, [](int t) {
        return t < 518 ? holding(true, 1, 1) : (t < 687 ? holding(true, -1, -1) : holding(true, 1, 1));
    });

    std::vector<Started> expected {{0, "LS_S_TL2BR"}, {349, "LS_A_TL2BR"}, {518, "LS_A_BR2TL"}, {687, "LS_A_TL2BR"}, {856, "LS_T1_BR_TL"}};
    EXPECT_EQ(expected, started);
}

TEST(JaSaberStateMachine, should_swing_right_to_left_with_no_direction_and_wait_before_the_next) {
    JaSaberStateMachine machine(moves(), clipsForAllMoves());

    auto started = run(machine, 1100, [](int) { return holding(true, 0, 0); });

    // 7 frames at 20 fps: 299 ms for a start or an attack, 300 for a return,
    // then the fire time at ready before the next swing
    std::vector<Started> expected {{0, "LS_S_R2L"}, {299, "LS_A_R2L"}, {598, "LS_R_R2L"}, {898, "LS_READY"}, {998, "LS_S_R2L"}};
    EXPECT_EQ(expected, started);
}

TEST(JaSaberStateMachine, should_finish_a_started_attack_when_the_button_is_let_go) {
    JaSaberStateMachine machine(moves(), clipsForAllMoves());

    auto started = run(machine, 1000, [](int t) { return t < 100 ? holding(true, 0, 1) : JaSaberInput(); });

    std::vector<Started> expected {{0, "LS_S_L2R"}, {299, "LS_A_L2R"}, {598, "LS_R_L2R"}, {898, "LS_READY"}};
    EXPECT_EQ(expected, started);
}

TEST(JaSaberStateMachine, should_let_a_new_attack_cut_a_return_short_after_moving) {
    JaSaberStateMachine machine(moves(), clipsForAllMoves());

    // given: a left-to-right attack, let go at its end, then moving right with
    // the button up
    auto started = run(machine, 700, [](int t) {
        if (t < 300) {
            return holding(true, 0, 1);
        }
        if (t < 650) {
            return holding(false, 0, 1);
        }
        // when: forward with the button, before the return has run out
        return holding(true, 1, 0);
    });

    // then: the transition from the right to the top cuts the return short
    std::vector<Started> expected {{0, "LS_S_L2R"}, {299, "LS_A_L2R"}, {598, "LS_R_L2R"}, {650, "LS_T1__R_T_"}};
    EXPECT_EQ(expected, started);
}

TEST(JaSaberStateMachine, should_play_a_return_out_when_standing_still) {
    JaSaberStateMachine machine(moves(), clipsForAllMoves());

    // The button is pressed again mid-return without moving first: the return
    // plays out, then the attack follows by the transition from the right.
    auto started = run(machine, 1300, [](int t) {
        if (t < 300) {
            return holding(true, 0, 1);
        }
        if (t < 650) {
            return JaSaberInput();
        }
        return holding(true, 1, 0);
    });

    // The transition plays 1.5 times as fast: 6 frames of 50 ms in 200 ms.
    std::vector<Started> expected {{0, "LS_S_L2R"}, {299, "LS_A_L2R"}, {598, "LS_R_L2R"}, {898, "LS_T1__R_T_"}, {1098, "LS_A_T2B"}};
    EXPECT_EQ(expected, started);
}

TEST(JaSaberStateMachine, should_decide_on_the_first_tick_after_a_move_runs_out) {
    // given: 60 ticks a second
    JaSaberStateMachine machine(moves(), namedSwingClips());
    std::vector<Started> started;

    // when: the swing is tapped
    for (int frame = 0; frame <= 60; ++frame) {
        auto input = frame == 0 ? holding(true, 1, 1) : JaSaberInput();
        for (int move : machine.tick(frame == 0 ? 0.0f : 1.0f / 60.0f, input)) {
            started.push_back({frame, name(move)});
        }
    }

    // then: each move starts on the first tick at or after its predecessor's
    // hold ran out, counted from the tick that predecessor started: 349 ms is
    // 20.9 ticks, 169 is 10.1 and 170 is 10.2, so 21, 11 and 11 ticks
    std::vector<Started> expected {{0, "LS_S_TL2BR"}, {21, "LS_A_TL2BR"}, {32, "LS_R_TL2BR"}, {43, "LS_READY"}};
    EXPECT_EQ(expected, started);
}

TEST(JaSaberStateMachine, should_run_a_hold_of_whole_ticks_out_on_its_last_tick) {
    for (int hz : {30, 60, 120, 144, 240}) {
        // given: Jedi Academy's swing, whose spin holds for 400 ms, 24 ticks
        // at 60 Hz
        JaSaberStateMachine machine(moves(), namedSwingClips());
        ASSERT_EQ(400, machine.timing(id("LS_T1_BR_TL")).holdMs);
        std::vector<Started> started;

        // when: holding forward, right and the button, round once
        for (int frame = 0; frame <= 2 * hz && started.size() < 4; ++frame) {
            for (int move : machine.tick(frame == 0 ? 0.0f : 1.0f / static_cast<float>(hz), holding(true, 1, 1))) {
                started.push_back({frame, name(move)});
            }
        }

        // then: each move starts on the first tick at or after its
        // predecessor's hold ran out, the spin's too: at 60 Hz on its 24th
        // tick, not the 25th
        auto ticks = [hz](int ms) { return (ms * hz + 999) / 1000; };
        int attack = ticks(349);
        int spin = attack + ticks(169);
        std::vector<Started> expected {{0, "LS_S_TL2BR"}, {attack, "LS_A_TL2BR"}, {spin, "LS_T1_BR_TL"}, {spin + ticks(400), "LS_A_TL2BR"}};
        EXPECT_EQ(expected, started) << hz << " Hz";
    }
}

TEST(JaSaberTimers, should_run_out_on_the_first_tick_at_or_after_their_time) {
    for (int hz : {24, 30, 50, 60, 75, 120, 144, 165, 240}) {
        float dt = 1.0f / static_cast<float>(hz);
        for (int hold = 1; hold <= 3000; ++hold) {
            double left = hold;
            int ticks = 0;
            while (left > 0.0) {
                left = jaCountDown(left, dt);
                ++ticks;
            }
            ASSERT_EQ((hold * hz + 999) / 1000, ticks) << hold << " ms at " << hz << " Hz";
        }
    }
    // and nothing counts it up
    EXPECT_DOUBLE_EQ(400.0, jaCountDown(400.0, 0.0f));
    EXPECT_DOUBLE_EQ(400.0, jaCountDown(400.0, -1.0f));
}

TEST(JaSaberLocomotion, should_turn_the_legs_toward_the_movement) {
    EXPECT_FLOAT_EQ(0.0f, jaLegsYawTarget(0, 0));
    EXPECT_FLOAT_EQ(0.0f, jaLegsYawTarget(1, 0));
    EXPECT_FLOAT_EQ(-45.0f, jaLegsYawTarget(1, 1));
    EXPECT_FLOAT_EQ(45.0f, jaLegsYawTarget(1, -1));
    EXPECT_FLOAT_EQ(-60.0f, jaLegsYawTarget(0, 1));
    EXPECT_FLOAT_EQ(60.0f, jaLegsYawTarget(0, -1));
    // Backwards the legs turn the other way, and straight back not at all.
    EXPECT_FLOAT_EQ(60.0f, jaLegsYawTarget(-1, 1));
    EXPECT_FLOAT_EQ(-60.0f, jaLegsYawTarget(-1, -1));
    EXPECT_FLOAT_EQ(0.0f, jaLegsYawTarget(-1, 0));
}

TEST(JaSaberLocomotion, should_pick_the_saber_out_walk_or_run) {
    EXPECT_EQ(std::string("BOTH_RUN2"), jaLocomotionClip(1, false));
    EXPECT_EQ(std::string("BOTH_RUN2"), jaLocomotionClip(0, false));
    EXPECT_EQ(std::string("BOTH_RUNBACK2"), jaLocomotionClip(-1, false));
    EXPECT_EQ(std::string("BOTH_WALK2"), jaLocomotionClip(1, true));
    EXPECT_EQ(std::string("BOTH_WALKBACK2"), jaLocomotionClip(-1, true));
}

TEST(JaSaberLocomotion, should_scale_walks_and_runs_to_the_speed) {
    GhoulAnimationClip run {12331, 26, 0, 40.0f};
    GhoulAnimationClip walk {20492, 24, 0, 20.0f};
    // 40 fps is 2 in Ghoul2's units; at half the run's speed, 1: half the clip's own rate.
    EXPECT_FLOAT_EQ(0.5f, jaFootSlideRate(run, false, 75.0f));
    // Never faster than 30 frames a second.
    EXPECT_FLOAT_EQ(0.75f, jaFootSlideRate(run, false, 1000.0f));
    EXPECT_FLOAT_EQ(0.005f, jaFootSlideRate(run, false, 0.0f));
    EXPECT_FLOAT_EQ(0.5f, jaFootSlideRate(walk, true, 25.0f));
    // Clips played in reverse keep their own rate.
    EXPECT_FLOAT_EQ(1.0f, jaFootSlideRate({0, 10, 0, -20.0f}, false, 500.0f));
}
