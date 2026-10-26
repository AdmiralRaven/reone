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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "reone/game/jadefense.h"
#include "reone/game/jasabermoves.h"

using namespace reone;
using namespace reone::game;
using namespace testing;

namespace {

constexpr float kScale = 0.025f; // metres per JA unit

// Draws that always come up the same: \p value, kept within the range.
JaRandom always(int value) {
    return [value](int low, int high) { return glm::clamp(value, low, high); };
}

JaRandomFloat noJitter() {
    return [](float, float) { return 0.0f; };
}

// Draws from a list, in turn.
JaRandom inTurn(std::vector<int> values) {
    auto list = std::make_shared<std::vector<int>>(std::move(values));
    auto next = std::make_shared<size_t>(0);
    return [list, next](int low, int high) {
        int value = (*list)[*next % list->size()];
        ++*next;
        return glm::clamp(value, low, high);
    };
}

// An upright body 1.5 m tall and 0.2 m round, standing at the origin.
std::vector<JaBodyPart> post() {
    JaBodyPart part;
    part.a = glm::vec3(0.0f, 0.0f, 0.0f);
    part.b = glm::vec3(0.0f, 0.0f, 1.5f);
    part.radius = 0.2f;
    return {part};
}

void expectNear(const glm::vec3 &expected, const glm::vec3 &actual, float tolerance = 1e-4f) {
    EXPECT_NEAR(expected.x, actual.x, tolerance);
    EXPECT_NEAR(expected.y, actual.y, tolerance);
    EXPECT_NEAR(expected.z, actual.z, tolerance);
}

const JaSaberMoves &moves() {
    return JaSaberMoves::fast();
}

} // namespace

// Blocks

TEST(JaBlock, should_pick_the_block_from_the_impacts_height_and_side_as_ja_does) {
    // above 5 units below the eye: 0.3 either way
    EXPECT_EQ(JaBlock::UpperRight, jaBlockFor(0.0f, 0.31f));
    EXPECT_EQ(JaBlock::Top, jaBlockFor(0.0f, 0.3f));
    EXPECT_EQ(JaBlock::Top, jaBlockFor(-4.9f, -0.3f));
    EXPECT_EQ(JaBlock::UpperLeft, jaBlockFor(-4.9f, -0.31f));
    // from 5 down to 22: 0.1
    EXPECT_EQ(JaBlock::UpperRight, jaBlockFor(-5.0f, 0.11f));
    EXPECT_EQ(JaBlock::Top, jaBlockFor(-21.9f, 0.1f));
    EXPECT_EQ(JaBlock::UpperLeft, jaBlockFor(-21.9f, -0.11f));
    // lower: right from 0 on
    EXPECT_EQ(JaBlock::LowerRight, jaBlockFor(-22.0f, 0.0f));
    EXPECT_EQ(JaBlock::LowerLeft, jaBlockFor(-40.0f, -0.01f));
}

TEST(JaBlock, should_turn_world_positions_into_a_block_by_the_view_and_the_scale) {
    // given: eyes 1.6 m up at the origin, looking along +Y (facing 0)
    glm::vec3 eye(0.0f, 0.0f, 1.6f);

    // then: an impact ahead and right at eye height, the upper right; one a
    // metre down, 40 units at this scale, the lower left on the left
    EXPECT_EQ(JaBlock::UpperRight, jaBlockAt(glm::vec3(0.5f, 1.0f, 1.6f), eye, 0.0f, kScale));
    EXPECT_EQ(JaBlock::LowerLeft, jaBlockAt(glm::vec3(-0.5f, 1.0f, 0.6f), eye, 0.0f, kScale));
    // and straight ahead, the top
    EXPECT_EQ(JaBlock::Top, jaBlockAt(glm::vec3(0.0f, 1.0f, 1.7f), eye, 0.0f, kScale));
    // facing the other way round, left and right swap
    EXPECT_EQ(JaBlock::UpperLeft, jaBlockAt(glm::vec3(0.5f, -1.0f, 1.6f), eye, glm::pi<float>(), kScale));
}

TEST(JaBlock, should_name_parries_and_their_clips_as_jas_table_does) {
    struct Case {
        JaBlock block;
        const char *parry;
        const char *parryClip;
        const char *reflect;
        const char *reflectClip;
    };
    // bg_panimate.cpp:308-322: a bolt's parries mirror some of the blow's clips
    std::vector<Case> cases {
        {JaBlock::Top, "LS_PARRY_UP", "BOTH_P1_S1_T_", "LS_REFLECT_UP", "BOTH_P1_S1_T_"},
        {JaBlock::UpperRight, "LS_PARRY_UR", "BOTH_P1_S1_TR", "LS_REFLECT_UR", "BOTH_P1_S1_TL"},
        {JaBlock::UpperLeft, "LS_PARRY_UL", "BOTH_P1_S1_TL", "LS_REFLECT_UL", "BOTH_P1_S1_TR"},
        {JaBlock::LowerRight, "LS_PARRY_LR", "BOTH_P1_S1_BL", "LS_REFLECT_LR", "BOTH_P1_S1_BR"},
        {JaBlock::LowerLeft, "LS_PARRY_LL", "BOTH_P1_S1_BR", "LS_REFLECT_LL", "BOTH_P1_S1_BL"},
    };
    for (auto &c : cases) {
        const auto &parry = moves().move(moves().parry(c.block));
        const auto &reflect = moves().move(moves().reflect(c.block));
        EXPECT_EQ(std::string(c.parry), parry.name);
        EXPECT_EQ(std::string(c.parryClip), parry.clip);
        EXPECT_FALSE(parry.reflect);
        EXPECT_EQ(std::string(c.reflect), reflect.name);
        EXPECT_EQ(std::string(c.reflectClip), reflect.clip);
        EXPECT_TRUE(reflect.reflect);
        for (auto *move : {&parry, &reflect}) {
            EXPECT_EQ(JaMoveKind::Parry, move->kind) << move->name;
            EXPECT_EQ(JaMoveHold::Active, move->hold) << move->name;
            EXPECT_EQ(50, move->blendMs) << move->name;
            EXPECT_EQ(JaQuad::R, move->startQuad) << move->name;
            EXPECT_EQ(JaMoveKind::Attack, moves().move(move->chainAttack).kind) << move->name;
        }
    }
    EXPECT_EQ(std::string("LS_A_TR2BL"), moves().move(moves().move(moves().parry(JaBlock::UpperRight)).chainAttack).name);
    EXPECT_EQ(std::string("LS_A_TL2BR"), moves().move(moves().move(moves().reflect(JaBlock::UpperRight)).chainAttack).name);
}

// Guards

TEST(JaGuard, should_be_wide_in_the_stance_and_parries_tight_in_attacks_and_none_in_transitions) {
    EXPECT_EQ(JaGuard::Wide, jaGuardOf(moves().move(moves().ready())));
    EXPECT_EQ(JaGuard::Wide, jaGuardOf(moves().move(moves().reflect(JaBlock::Top))));
    EXPECT_EQ(JaGuard::Tight, jaGuardOf(moves().move(moves().find("LS_A_TL2BR"))));
    EXPECT_EQ(JaGuard::Tight, jaGuardOf(moves().move(moves().find("LS_S_TL2BR"))));
    EXPECT_EQ(JaGuard::Tight, jaGuardOf(moves().move(moves().find("LS_R_TL2BR"))));
    for (auto &move : moves().moves()) {
        if (move.kind == JaMoveKind::Transition) {
            EXPECT_EQ(JaGuard::None, jaGuardOf(move)) << move.name;
        }
    }
}

TEST(JaGuard, should_grow_the_body_by_8_units_and_move_it_12_forward_for_a_wide_guard) {
    auto guard = jaWideGuard(post(), 0.0f, kScale);
    ASSERT_EQ(1u, guard.size());
    EXPECT_NEAR(0.2f + 8.0f * kScale, guard[0].radius, 1e-5f);
    expectNear(glm::vec3(0.0f, 12.0f * kScale, 0.0f), guard[0].a);
    expectNear(glm::vec3(0.0f, 12.0f * kScale, 1.5f), guard[0].b);
}

TEST(JaGuard, should_make_each_blade_a_capsule_16_units_thick_for_a_tight_guard) {
    JaBlade blade {glm::vec3(0.0f, 0.5f, 1.0f), glm::vec3(0.0f, 1.5f, 1.0f)};
    auto guard = jaTightGuard({blade}, kScale);
    ASSERT_EQ(1u, guard.size());
    EXPECT_NEAR(16.0f * kScale, guard[0].radius, 1e-5f);
    expectNear(blade.base, guard[0].a);
    expectNear(blade.tip, guard[0].b);
}

// Sweeping a bolt

TEST(JaSweepBall, should_find_where_a_ball_first_touches_a_capsule) {
    // when: a bolt 0.05 m thick flies along +Y at 1 m up, from 2 m before
    // the post to 2 m past it
    auto contact = jaSweepBall(glm::vec3(0.0f, -2.0f, 1.0f), glm::vec3(0.0f, 2.0f, 1.0f), 0.05f, post());

    // then: it touches it once within 0.25 m of its axis, 1.75 m on: 7/16 of the way
    ASSERT_TRUE(contact);
    EXPECT_EQ(0u, contact->part);
    EXPECT_NEAR(1.75f / 4.0f, contact->time, 1e-4f);
    expectNear(glm::vec3(0.0f, -0.25f, 1.0f), contact->point, 1e-3f);
}

TEST(JaSweepBall, should_miss_a_capsule_it_passes_beside_and_find_the_nearer_of_two) {
    // a bolt passing 0.3 m beside the post misses it
    EXPECT_FALSE(jaSweepBall(glm::vec3(0.3f, -2.0f, 1.0f), glm::vec3(0.3f, 2.0f, 1.0f), 0.05f, post()));
    // and stopping short of it, too
    EXPECT_FALSE(jaSweepBall(glm::vec3(0.0f, -2.0f, 1.0f), glm::vec3(0.0f, -0.3f, 1.0f), 0.05f, post()));

    // of two posts on its way, it touches the nearer first
    auto parts = post();
    auto far = parts[0];
    far.a.y = far.b.y = 1.0f;
    auto near = parts[0];
    near.a.y = near.b.y = -1.0f;
    auto contact = jaSweepBall(glm::vec3(0.0f, -2.0f, 1.0f), glm::vec3(0.0f, 2.0f, 1.0f), 0.05f, {far, near});
    ASSERT_TRUE(contact);
    EXPECT_EQ(1u, contact->part);
    EXPECT_NEAR(0.75f / 4.0f, contact->time, 1e-4f);

    // one that starts inside touches at once
    auto inside = jaSweepBall(glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 2.0f, 1.0f), 0.05f, post());
    ASSERT_TRUE(inside);
    EXPECT_FLOAT_EQ(0.0f, inside->time);
}

TEST(JaSweepBall, should_find_a_graze_where_the_way_passes_nearest) {
    // a bolt passing 0.24 m from the axis, within the reach of 0.25 m
    auto contact = jaSweepBall(glm::vec3(0.24f, -2.0f, 1.0f), glm::vec3(0.24f, 2.0f, 1.0f), 0.05f, post());
    ASSERT_TRUE(contact);
    // it touches 0.07 m before passing the axis
    EXPECT_NEAR((2.0f - std::sqrt(0.25f * 0.25f - 0.24f * 0.24f)) / 4.0f, contact->time, 1e-3f);
}

// Bolts against the guard

TEST(JaBolts, should_pass_through_the_guard_from_behind_and_one_time_in_d2_plus_1) {
    // from behind, always
    EXPECT_TRUE(jaBoltPassesGuard(3, false, always(5)));
    // in front, when the draw from 0 to d² comes up 0
    EXPECT_TRUE(jaBoltPassesGuard(1, true, always(0)));
    EXPECT_FALSE(jaBoltPassesGuard(1, true, always(1)));
    EXPECT_FALSE(jaBoltPassesGuard(3, true, always(9)));
    // at rank 0 the draw is from 0 to 0: always through
    EXPECT_TRUE(jaBoltPassesGuard(0, true, always(7)));

    // the draws' ranges: 0-1, 0-4, 0-9
    for (int d = 1; d <= 3; ++d) {
        int high = -1;
        jaBoltPassesGuard(d, true, [&high](int low, int h) { high = h; return low; });
        EXPECT_EQ(d * d, high) << d;
    }
}

TEST(JaBolts, should_send_back_a_stopped_bolt_unless_the_draw_from_0_to_1_3_or_10_comes_up_0) {
    EXPECT_FALSE(jaBoltSentBack(0, always(1)));
    EXPECT_FALSE(jaBoltSentBack(1, always(0)));
    EXPECT_TRUE(jaBoltSentBack(1, always(1)));
    std::map<int, int> highs;
    for (int d = 1; d <= 3; ++d) {
        jaBoltSentBack(d, [&highs, d](int low, int high) { highs[d] = high; return high; });
    }
    EXPECT_EQ((std::map<int, int> {{1, 1}, {2, 3}, {3, 10}}), highs);
}

TEST(JaBolts, should_wait_300_150_and_50_ms_between_parries_and_hold_the_saber_only_at_low_ranks) {
    EXPECT_EQ(300, jaBoltParryDebounceMs(1));
    EXPECT_EQ(150, jaBoltParryDebounceMs(2));
    EXPECT_EQ(50, jaBoltParryDebounceMs(3));
    EXPECT_EQ(500, jaParryWeaponTimeMs(0));
    EXPECT_EQ(300, jaParryWeaponTimeMs(1));
    EXPECT_EQ(0, jaParryWeaponTimeMs(2));
    EXPECT_EQ(0, jaParryWeaponTimeMs(3));
}

TEST(JaBolts, should_see_in_front_level_for_the_guard_and_with_height_for_the_scanner) {
    glm::vec3 me(0.0f);
    // level: strictly more than 0.2 along the facing
    EXPECT_TRUE(jaInFrontLevel(glm::vec3(0.0f, 1.0f, 5.0f), me, 0.0f, kJaReflectCone));
    EXPECT_FALSE(jaInFrontLevel(glm::vec3(1.0f, 0.2f, 0.0f), me, 0.0f, 0.2f));
    EXPECT_FALSE(jaInFrontLevel(glm::vec3(0.0f, -1.0f, 0.0f), me, 0.0f, kJaReflectCone));
    // with height kept: a bolt high above barely counts
    EXPECT_FALSE(jaInFront(glm::vec3(0.0f, 1.0f, 5.0f), me, 0.0f, kJaReflectCone));
    EXPECT_TRUE(jaInFront(glm::vec3(0.0f, 1.0f, 0.5f), me, 0.0f, kJaReflectCone));
}

// Where a bolt sent back goes

TEST(JaReflect, should_aim_at_the_current_enemys_head_at_rank_3) {
    JaReflectTargets targets;
    targets.currentEnemyHead = glm::vec3(0.0f, 10.0f, 1.7f);
    targets.otherEnemyHead = glm::vec3(5.0f, 10.0f, 1.7f);
    targets.shooter = glm::vec3(-5.0f, 10.0f, 0.6f);

    // when: the draw for the current enemy, from 0 to 3, comes up 0, and the
    // offsets' 0, from a steady parry
    auto direction = jaReflectDirection(glm::vec3(0.0f), targets, 3, JaReflectSteadiness::Steady, kScale, always(0), noJitter());
    // then: at the other enemy, one time in four
    expectNear(glm::normalize(glm::vec3(5.0f, 10.0f, 1.7f)), direction);

    // and when it comes up anything else, at the current enemy
    direction = jaReflectDirection(glm::vec3(0.0f), targets, 3, JaReflectSteadiness::Steady, kScale, inTurn({1, 0, 0, 0}), noJitter());
    expectNear(glm::normalize(glm::vec3(0.0f, 10.0f, 1.7f)), direction);
}

TEST(JaReflect, should_offset_the_head_by_up_to_4_units_across_and_16_down) {
    JaReflectTargets targets;
    targets.currentEnemyHead = glm::vec3(0.0f, 10.0f, 1.7f);
    // the offsets' draws, after the enemy's: 4, -4, -16
    auto direction = jaReflectDirection(glm::vec3(0.0f), targets, 3, JaReflectSteadiness::Steady, kScale, inTurn({1, 4, -4, -16}), noJitter());
    expectNear(glm::normalize(glm::vec3(4.0f * kScale, 10.0f - 4.0f * kScale, 1.7f - 16.0f * kScale)), direction);
}

TEST(JaReflect, should_go_back_at_the_shooter_at_rank_1_and_mostly_at_rank_2) {
    JaReflectTargets targets;
    targets.currentEnemyHead = glm::vec3(0.0f, 10.0f, 1.7f);
    targets.shooter = glm::vec3(0.0f, 10.0f, 0.6f);
    // rank 1: never at an enemy, so the draws aren't used for one
    int draws = 0;
    auto counting = [&draws](int low, int) { ++draws; return low; };
    auto direction = jaReflectDirection(glm::vec3(0.0f), targets, 1, JaReflectSteadiness::Steady, kScale, counting, noJitter());
    EXPECT_EQ(0, draws);
    expectNear(glm::normalize(targets.shooter), direction);
    // rank 2: at an enemy only when the draw from 0 to 3 comes up 0
    direction = jaReflectDirection(glm::vec3(0.0f), targets, 2, JaReflectSteadiness::Steady, kScale, always(1), noJitter());
    expectNear(glm::normalize(targets.shooter), direction);
}

TEST(JaReflect, should_jitter_more_at_low_ranks_and_while_swinging) {
    JaReflectTargets targets;
    targets.shooter = glm::vec3(0.0f, 10.0f, 0.0f);
    std::vector<float> amounts;
    auto recording = [&amounts](float, float high) { amounts.push_back(high); return 0.0f; };

    jaReflectDirection(glm::vec3(0.0f), targets, 1, JaReflectSteadiness::Steady, kScale, always(1), recording);
    EXPECT_EQ(std::vector<float>({0.4f, 0.4f, 0.4f}), amounts);
    amounts.clear();
    jaReflectDirection(glm::vec3(0.0f), targets, 2, JaReflectSteadiness::Swinging, kScale, always(1), recording);
    EXPECT_EQ(std::vector<float>({0.2f, 0.2f, 0.2f, 0.3f, 0.3f, 0.3f}), amounts);
    amounts.clear();
    jaReflectDirection(glm::vec3(0.0f), targets, 2, JaReflectSteadiness::Other, kScale, always(1), recording);
    EXPECT_EQ(std::vector<float>({0.2f, 0.2f, 0.2f, 0.1f, 0.1f, 0.1f}), amounts);

    // aimed at an enemy: no extra jitter from a steady move, 0.2 swinging
    targets.currentEnemyHead = glm::vec3(0.0f, 10.0f, 1.7f);
    amounts.clear();
    jaReflectDirection(glm::vec3(0.0f), targets, 3, JaReflectSteadiness::Steady, kScale, always(1), recording);
    EXPECT_TRUE(amounts.empty());
    jaReflectDirection(glm::vec3(0.0f), targets, 3, JaReflectSteadiness::Swinging, kScale, always(1), recording);
    EXPECT_EQ(std::vector<float>({0.2f, 0.2f, 0.2f}), amounts);
}

TEST(JaReflect, should_count_starts_and_returns_as_swinging) {
    // JA's larger jitter goes with PM_SaberInAttack or PM_SaberInTransitionAny,
    // and the latter counts starts and returns
    int starts = 0;
    int returns = 0;
    for (auto &move : JaSaberMoves::fast().moves()) {
        switch (move.kind) {
        case JaMoveKind::Ready:
        case JaMoveKind::Parry:
            EXPECT_EQ(JaReflectSteadiness::Steady, jaReflectSteadiness(move)) << move.name;
            break;
        default:
            EXPECT_EQ(JaReflectSteadiness::Swinging, jaReflectSteadiness(move)) << move.name;
            break;
        }
        starts += move.kind == JaMoveKind::Start ? 1 : 0;
        returns += move.kind == JaMoveKind::Return ? 1 : 0;
    }
    EXPECT_GT(starts, 0);
    EXPECT_GT(returns, 0);
}

// Parries in the state machine

namespace {

std::map<std::string, graphics::GhoulAnimationClip> clipsForEveryMove() {
    std::map<std::string, graphics::GhoulAnimationClip> clips;
    for (auto &move : JaSaberMoves::fast().moves()) {
        clips[move.clip] = {0, 7, -1, 20.0f};
    }
    // JA's parry clips are two frames long
    for (auto clip : {"BOTH_P1_S1_T_", "BOTH_P1_S1_TR", "BOTH_P1_S1_TL", "BOTH_P1_S1_BL", "BOTH_P1_S1_BR"}) {
        clips[clip] = {0, 2, -1, 20.0f};
    }
    return clips;
}

JaSaberInput holding(bool attack, int forward = 0, int right = 0) {
    JaSaberInput input;
    input.attack = attack;
    input.forward = forward;
    input.right = right;
    return input;
}

constexpr float kTick = 1.0f / 60.0f;

} // namespace

TEST(JaSaberParry, should_parry_from_ready_on_the_next_tick_then_go_back_to_ready) {
    // given
    auto clips = clipsForEveryMove();
    JaSaberStateMachine machine(moves(), clips);
    int parry = moves().reflect(JaBlock::UpperRight);

    // when
    EXPECT_TRUE(machine.canBlock());
    EXPECT_TRUE(machine.block(parry));
    auto started = machine.tick(kTick, holding(false));

    // then: the parry starts, holding the body for its clip, 49 ms
    EXPECT_EQ(std::vector<int>({parry}), started);
    EXPECT_EQ(parry, machine.move());
    EXPECT_NEAR(49.0f, machine.holdTimerMs(), 1e-3f);

    // and once it runs out, back to ready
    std::vector<int> later;
    for (int tick = 0; tick < 10; ++tick) {
        for (int id : machine.tick(kTick, holding(false))) {
            later.push_back(id);
        }
    }
    EXPECT_EQ(std::vector<int>({moves().ready()}), later);
}

TEST(JaSaberParry, should_not_parry_in_an_attack_and_take_the_first_of_two_parries_in_a_tick) {
    auto clips = clipsForEveryMove();
    JaSaberStateMachine machine(moves(), clips);
    machine.tick(kTick, holding(true));
    ASSERT_EQ(JaMoveKind::Start, moves().move(machine.move()).kind);
    EXPECT_FALSE(machine.canBlock());
    EXPECT_FALSE(machine.block(moves().parry(JaBlock::Top)));

    JaSaberStateMachine other(moves(), clips);
    EXPECT_TRUE(other.block(moves().parry(JaBlock::Top)));
    EXPECT_TRUE(other.block(moves().parry(JaBlock::LowerLeft)));
    other.tick(kTick, holding(false));
    EXPECT_EQ(moves().parry(JaBlock::Top), other.move());

    EXPECT_THROW(other.block(moves().ready()), std::invalid_argument);
}

TEST(JaSaberParry, should_go_on_from_a_parry_to_an_attack_from_where_it_ends_with_the_button_held) {
    auto clips = clipsForEveryMove();
    JaSaberStateMachine machine(moves(), clips);
    int parry = moves().parry(JaBlock::UpperRight); // ends top left
    machine.block(parry);
    machine.tick(kTick, holding(false));

    // when: the button is held, with no direction, as the parry runs out
    std::vector<int> started;
    for (int tick = 0; tick < 10 && started.empty(); ++tick) {
        started = machine.tick(kTick, holding(true));
    }

    // then: the parry's own attack, from the top right, through the transition from the top left
    ASSERT_EQ(1u, started.size());
    EXPECT_EQ(std::string("LS_T1_TL_TR"), moves().move(started[0]).name);

    // and with a direction, that one's attack, from the parry's end
    JaSaberStateMachine aimed(moves(), clips);
    aimed.block(moves().parry(JaBlock::Top)); // ends at the top
    aimed.tick(kTick, holding(false));
    started.clear();
    for (int tick = 0; tick < 10 && started.empty(); ++tick) {
        started = aimed.tick(kTick, holding(true, 1, 0)); // forward: top to bottom
    }
    ASSERT_EQ(1u, started.size());
    EXPECT_EQ(std::string("LS_A_T2B"), moves().move(started[0]).name);
}

TEST(JaSaberParry, should_let_the_button_break_out_of_a_bolts_parry_at_once_but_not_a_blows_held) {
    auto clips = clipsForEveryMove();
    // a bolt's parry, held for 300 ms as at rank 1
    JaSaberStateMachine machine(moves(), clips);
    machine.block(moves().reflect(JaBlock::Top), 300);
    machine.tick(kTick, holding(false));
    ASSERT_EQ(moves().reflect(JaBlock::Top), machine.move());
    auto started = machine.tick(kTick, holding(true));
    ASSERT_EQ(1u, started.size());
    EXPECT_EQ(JaMoveKind::Attack, moves().move(started[0]).kind);

    // a blow's parry, held for 300 ms, keeps the saber
    JaSaberStateMachine held(moves(), clips);
    held.block(moves().parry(JaBlock::Top), 300);
    held.tick(kTick, holding(false));
    for (int tick = 0; tick < 12; ++tick) {
        EXPECT_TRUE(held.tick(kTick, holding(true)).empty()) << tick;
    }
}
