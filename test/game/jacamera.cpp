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

#include "reone/game/jacamera.h"

using namespace reone;
using namespace reone::game;

namespace {

// pmbbl's camera hook: 1.68 m up, so 0.028 m per JA unit.
constexpr float kEyes = 1.68f;
constexpr float kUnit = kEyes / 60.0f;
constexpr float kTolerance = 1e-4f;

const glm::vec3 kFeet(1.0f, 2.0f, 0.0f);

void expectNear(const glm::vec3 &expected, const glm::vec3 &actual, float tolerance = kTolerance) {
    EXPECT_NEAR(expected.x, actual.x, tolerance);
    EXPECT_NEAR(expected.y, actual.y, tolerance);
    EXPECT_NEAR(expected.z, actual.z, tolerance);
}

JaCamera::Obstruction clear() {
    return [](const glm::vec3 &, const glm::vec3 &) { return std::optional<float>(); };
}

// An infinite wall where coordinate \p axis equals \p at.
JaCamera::Obstruction wall(int axis, float at) {
    return [axis, at](const glm::vec3 &from, const glm::vec3 &to) -> std::optional<float> {
        float a = from[axis] - at;
        float b = to[axis] - at;
        if (a * b > 0.0f || a == b) {
            return std::nullopt;
        }
        return glm::length(to - from) * (a / (a - b));
    };
}

// JA's damping: the gap left after dt seconds when each 50 ms leaves (1 - damp) of it.
float left(float damp, float dt) {
    return std::pow(1.0f - damp, dt / 0.05f);
}

} // namespace

TEST(JaCamera, should_scale_ja_units_by_the_eye_height) {
    JaCamera camera(kEyes);

    EXPECT_NEAR(0.028f, camera.scale(), kTolerance);
}

TEST(JaCamera, should_aim_above_the_eyes_and_stand_the_range_behind_on_a_level_view) {
    // given
    JaCamera camera(kEyes);

    // when: the first update, which goes straight to the view as JA's first frame does
    camera.update(1.0f / 60.0f, kFeet, 0.0f, 0.0f, JaCamera::Settings::behind(), clear());

    // then: the aim point is 16 units above the eyes, and the camera 80 units
    // behind it, level with it
    glm::vec3 target(1.0f, 2.0f, kEyes + 16.0f * kUnit);
    expectNear(target, camera.aim());
    expectNear(target - glm::vec3(0.0f, 80.0f * kUnit, 0.0f), camera.eye());
}

TEST(JaCamera, should_rise_behind_when_looking_down_and_sink_when_looking_up) {
    float pitch = glm::radians(30.0f);
    JaCamera down(kEyes);
    JaCamera up(kEyes);

    down.update(0.0f, kFeet, 0.0f, pitch, JaCamera::Settings::behind(), clear());
    up.update(0.0f, kFeet, 0.0f, -pitch, JaCamera::Settings::behind(), clear());

    glm::vec3 target(1.0f, 2.0f, kEyes + 16.0f * kUnit);
    float range = 80.0f * kUnit;
    expectNear(target + glm::vec3(0.0f, -range * std::cos(pitch), range * std::sin(pitch)), down.eye());
    expectNear(target + glm::vec3(0.0f, -range * std::cos(pitch), -range * std::sin(pitch)), up.eye());
    EXPECT_LT(down.aim().z, down.eye().z);
    EXPECT_GT(up.aim().z, up.eye().z);
}

TEST(JaCamera, should_stand_behind_whichever_way_the_view_turns) {
    // given: facing 90 degrees to the left, along -X
    JaCamera camera(kEyes);

    // when
    camera.update(0.0f, kFeet, glm::half_pi<float>(), 0.0f, JaCamera::Settings::behind(), clear());

    // then: the camera stands behind, along +X
    glm::vec3 target(1.0f, 2.0f, kEyes + 16.0f * kUnit);
    expectNear(target + glm::vec3(80.0f * kUnit, 0.0f, 0.0f), camera.eye());
}

TEST(JaCamera, should_cap_the_camera_pitch_at_89_degrees) {
    glm::vec3 forward(JaCamera::viewForward(0.0f, glm::radians(120.0f)));

    EXPECT_NEAR(-std::sin(glm::radians(89.0f)), forward.z, kTolerance);
    EXPECT_NEAR(std::cos(glm::radians(89.0f)), forward.y, kTolerance);
}

TEST(JaCamera, should_close_half_the_aim_gap_and_three_tenths_of_the_camera_gap_per_50_ms) {
    // given: a camera that starts at another camera's view
    JaCamera camera(kEyes);
    glm::vec3 eye(1.0f, -3.0f, 2.0f);
    glm::vec3 aim(1.0f, 2.0f, 1.68f);
    camera.start(eye, aim, 0.0f);
    auto settings = JaCamera::Settings::behind();
    glm::vec3 target(camera.idealTarget(kFeet, 0.0f, settings));
    glm::vec3 location(camera.idealLocation(target, 0.0f, 0.0f, settings));

    // when: 50 ms on a level view that does not turn
    camera.update(0.05f, kFeet, 0.0f, 0.0f, settings, clear());

    // then
    expectNear(target - 0.5f * (target - aim), camera.aim());
    expectNear(location - 0.7f * (location - eye), camera.eye());
}

TEST(JaCamera, should_damp_a_60_hz_frame_by_the_fraction_of_50_ms) {
    JaCamera camera(kEyes);
    glm::vec3 eye(1.0f, -3.0f, 2.0f);
    glm::vec3 aim(1.0f, 2.0f, 1.68f);
    camera.start(eye, aim, 0.0f);
    auto settings = JaCamera::Settings::behind();
    glm::vec3 target(camera.idealTarget(kFeet, 0.0f, settings));
    glm::vec3 location(camera.idealLocation(target, 0.0f, 0.0f, settings));
    float dt = 1.0f / 60.0f;

    camera.update(dt, kFeet, 0.0f, 0.0f, settings, clear());

    expectNear(target - left(0.5f, dt) * (target - aim), camera.aim());
    expectNear(location - left(0.3f, dt) * (location - eye), camera.eye());
}

TEST(JaCamera, should_follow_more_tightly_the_steeper_the_view) {
    // given: looking 60 degrees down, the camera damps by 0.7 (60/115)^2 + 0.3
    JaCamera camera(kEyes);
    glm::vec3 eye(1.0f, -3.0f, 2.0f);
    camera.start(eye, glm::vec3(1.0f, 2.0f, 1.68f), 0.0f);
    auto settings = JaCamera::Settings::behind();
    float pitch = glm::radians(60.0f);
    glm::vec3 location(camera.idealLocation(camera.idealTarget(kFeet, 0.0f, settings), 0.0f, pitch, settings));
    float steep = 60.0f / 115.0f;
    float damp = 0.7f * steep * steep + 0.3f;

    // when
    camera.update(0.05f, kFeet, 0.0f, pitch, settings, clear());

    // then
    expectNear(location - (1.0f - damp) * (location - eye), camera.eye());
}

TEST(JaCamera, should_stiffen_on_a_fast_turn) {
    auto settings = JaCamera::Settings::behind();
    glm::vec3 eye(1.0f, -3.0f, 2.0f);
    glm::vec3 aim(1.0f, 2.0f, 1.68f);
    // Degrees turned in 50 ms, and the stiffness JA gives a turn that fast:
    // none below 1 degree per millisecond, 0.5 per degree above it, and 0.75
    // above 2.5.
    struct Case {
        float degrees;
        float stiff;
    };
    for (auto c : {Case {40.0f, 0.0f}, Case {75.0f, 0.25f}, Case {150.0f, 0.75f}}) {
        JaCamera camera(kEyes);
        camera.start(eye, aim, 0.0f);
        float yaw = glm::radians(c.degrees);
        glm::vec3 location(camera.idealLocation(camera.idealTarget(kFeet, yaw, settings), yaw, 0.0f, settings));
        float damp = 0.3f + 0.7f * c.stiff;

        camera.update(0.05f, kFeet, yaw, 0.0f, settings, clear());

        expectNear(location - (1.0f - damp) * (location - eye), camera.eye(), 1e-3f);
    }
}

TEST(JaCamera, should_measure_a_turn_the_short_way_round) {
    // given: from 350 degrees to 10, a 20-degree turn, too slow to stiffen
    auto settings = JaCamera::Settings::behind();
    glm::vec3 eye(1.0f, -3.0f, 2.0f);
    JaCamera camera(kEyes);
    camera.start(eye, glm::vec3(1.0f, 2.0f, 1.68f), glm::radians(350.0f));
    float yaw = glm::radians(10.0f);
    glm::vec3 location(camera.idealLocation(camera.idealTarget(kFeet, yaw, settings), yaw, 0.0f, settings));

    // when
    camera.update(0.05f, kFeet, yaw, 0.0f, settings, clear());

    // then
    expectNear(location - 0.7f * (location - eye), camera.eye(), 1e-3f);
}

TEST(JaCamera, should_stop_short_of_a_wall_behind) {
    // given: a wall 1 m behind the eyes
    JaCamera camera(kEyes);

    // when
    camera.update(0.0f, kFeet, 0.0f, 0.0f, JaCamera::Settings::behind(), wall(1, 1.0f));

    // then: the camera stops the camera box's half-size, 4 units, in front of it
    EXPECT_NEAR(1.0f + 4.0f * kUnit, camera.eye().y, kTolerance);
    EXPECT_NEAR(kEyes + 16.0f * kUnit, camera.eye().z, kTolerance);
}

TEST(JaCamera, should_keep_the_aim_point_under_a_low_ceiling) {
    // given: a ceiling 0.2 m above the eyes
    JaCamera camera(kEyes);
    float ceiling = kEyes + 0.2f;

    // when
    camera.update(0.0f, kFeet, 0.0f, 0.0f, JaCamera::Settings::behind(), wall(2, ceiling));

    // then
    EXPECT_NEAR(ceiling - 4.0f * kUnit, camera.aim().z, kTolerance);
}

TEST(JaCamera, should_look_along_the_view_when_too_close_to_aim) {
    // given: a wall right behind the aim point
    JaCamera camera(kEyes);

    // when
    camera.update(0.0f, kFeet, 0.0f, 0.0f, JaCamera::Settings::behind(), wall(1, 2.0f - kUnit));

    // then: it stands at the aim point and looks ahead
    glm::vec3 look(camera.aim() - camera.eye());
    EXPECT_NEAR(0.0f, look.x, kTolerance);
    EXPECT_GT(look.y, 0.5f);
    EXPECT_NEAR(0.0f, look.z, kTolerance);
}

TEST(JaCamera, should_move_aim_point_and_camera_to_the_right_over_the_shoulder) {
    // given
    JaCamera camera(kEyes);
    auto settings = JaCamera::Settings::shoulder();

    // when
    camera.update(0.0f, kFeet, 0.0f, 0.0f, settings, clear());

    // then: 16 units to the right, 4 above the eyes, the camera 56 behind,
    // looking the way the character does
    glm::vec3 target(1.0f + 16.0f * kUnit, 2.0f, kEyes + 4.0f * kUnit);
    expectNear(target, camera.aim());
    expectNear(target - glm::vec3(0.0f, 56.0f * kUnit, 0.0f), camera.eye());
}
