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

#include "reone/graphics/format/glareader.h"
#include "reone/graphics/ghoulanimation.h"
#include "reone/system/exception/validation.h"
#include "reone/system/stream/memoryinput.h"

#include "../../fixtures/gla.h"

using namespace reone;
using namespace reone::graphics;
using namespace reone::test;

namespace {

GlaBuilder threeBoneChain() {
    GlaBuilder gla;
    gla.scale = 0.5f;
    glm::mat4 rootBase(0.5f);
    rootBase[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    glm::mat4 middleBase(0.5f);
    middleBase[3] = glm::vec4(1.0f, 2.0f, 3.0f, 1.0f);
    glm::mat4 tipBase(glm::rotate(glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
    tipBase[3] = glm::vec4(1.0f, 2.0f, 13.0f, 1.0f);
    gla.bones = {{"model_root", -1, rootBase},
                 {"Middle", 0, middleBase},
                 {"tip", 1, tipBase}};
    // Pool entry 0: identity. Entry 1: a quarter turn about Z with a translation.
    gla.pool = {GlaBuilder::entry(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.0f)),
                GlaBuilder::entry(glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f)), glm::vec3(10.0f, -5.0f, 2.5f))};
    // Frame 0: the bind pose. Frame 1: the middle bone turns.
    gla.frames = {{0, 0, 0}, {0, 1, 0}};
    return gla;
}

} // namespace

TEST(GlaReader, should_load_skeleton_and_frames) {
    // given
    auto bytes = threeBoneChain().build();
    auto stream = MemoryInputStream(bytes);
    auto reader = GlaReader(stream);

    // when
    reader.load();

    // then
    auto gla = reader.animation();
    ASSERT_TRUE(static_cast<bool>(gla));
    EXPECT_EQ(std::string("synthetic"), gla->name());
    EXPECT_FLOAT_EQ(0.5f, gla->scale());
    EXPECT_EQ(2, gla->numFrames());
    ASSERT_EQ(3, gla->numBones());
    EXPECT_EQ(std::string("Middle"), gla->bones()[1].name);
    EXPECT_EQ(-1, gla->bones()[0].parent);
    EXPECT_EQ(1, gla->bones()[2].parent);
    EXPECT_EQ(1, gla->boneIndex("middle"));
    EXPECT_EQ(2, gla->boneIndex("TIP"));
    EXPECT_EQ(-1, gla->boneIndex("missing"));
    // Base poses are stored as 3x4 row-major matrices.
    const glm::mat4 &tipBase = gla->bones()[2].basePose;
    EXPECT_FLOAT_EQ(13.0f, tipBase[3].z);
    EXPECT_NEAR(std::sin(glm::radians(30.0f)), tipBase[0].z * -1.0f, 1e-6f);
    EXPECT_NEAR(std::sin(glm::radians(30.0f)), tipBase[2].x, 1e-6f);
}

TEST(GlaReader, should_decode_compressed_bone_transforms) {
    // given
    auto bytes = threeBoneChain().build();
    auto stream = MemoryInputStream(bytes);
    auto reader = GlaReader(stream);
    reader.load();
    auto gla = reader.animation();

    // when
    glm::mat4 still = gla->boneDelta(0, 1);
    glm::mat4 turned = gla->boneDelta(1, 1);

    // then
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            EXPECT_NEAR(c == r ? 1.0f : 0.0f, still[c][r], 1e-4f);
        }
    }
    glm::vec3 x(turned * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
    EXPECT_NEAR(0.0f, x.x, 1e-3f);
    EXPECT_NEAR(1.0f, x.y, 1e-3f);
    EXPECT_NEAR(0.0f, x.z, 1e-3f);
    // Translations are stored in 1/64 steps, so these come back exactly.
    EXPECT_FLOAT_EQ(10.0f, turned[3].x);
    EXPECT_FLOAT_EQ(-5.0f, turned[3].y);
    EXPECT_FLOAT_EQ(2.5f, turned[3].z);
}

TEST(GlaReader, should_compose_bone_transforms_down_the_chain) {
    // given
    auto bytes = threeBoneChain().build();
    auto stream = MemoryInputStream(bytes);
    auto reader = GlaReader(stream);
    reader.load();
    auto gla = reader.animation();

    // when
    auto bind = gla->boneTransforms(0);
    auto turned = gla->boneTransforms(1);

    // then
    for (int bone = 0; bone < 3; ++bone) {
        glm::mat4 expectedBind(gla->boneDelta(0, 0));
        if (bone >= 1) {
            expectedBind = expectedBind * gla->boneDelta(0, 1);
        }
        if (bone >= 2) {
            expectedBind = expectedBind * gla->boneDelta(0, 2);
        }
        expectedBind = expectedBind * gla->bones()[bone].basePose;
        glm::mat4 expectedTurned(gla->boneDelta(1, 0));
        if (bone >= 1) {
            expectedTurned = expectedTurned * gla->boneDelta(1, 1);
        }
        if (bone >= 2) {
            expectedTurned = expectedTurned * gla->boneDelta(1, 2);
        }
        expectedTurned = expectedTurned * gla->bones()[bone].basePose;
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                EXPECT_NEAR(expectedBind[c][r], bind[bone][c][r], 1e-5f);
                EXPECT_NEAR(expectedTurned[c][r], turned[bone][c][r], 1e-5f);
            }
        }
    }
    // The turn carries the tip with the middle bone: (1, 2, 13) turns to (-2, 1, 13), then moves by (10, -5, 2.5).
    EXPECT_NEAR(8.0f, turned[2][3].x, 2e-3f);
    EXPECT_NEAR(-4.0f, turned[2][3].y, 2e-3f);
    EXPECT_NEAR(15.5f, turned[2][3].z, 2e-3f);
}

TEST(GlaReader, should_reject_bad_signature_and_version) {
    auto builder = threeBoneChain();
    auto bytes = builder.build();
    bytes[0] = 'X';
    auto stream = MemoryInputStream(bytes);
    auto reader = GlaReader(stream);
    EXPECT_THROW(reader.load(), ValidationException);

    builder.version = 5;
    auto oldVersion = builder.build();
    auto oldStream = MemoryInputStream(oldVersion);
    auto oldReader = GlaReader(oldStream);
    EXPECT_THROW(oldReader.load(), ValidationException);
}

TEST(GlaReader, should_reject_frame_index_outside_the_pool) {
    auto builder = threeBoneChain();
    builder.frames[1][2] = 2; // the pool has two entries
    auto bytes = builder.build();
    auto stream = MemoryInputStream(bytes);
    auto reader = GlaReader(stream);
    EXPECT_THROW(reader.load(), ValidationException);
}

TEST(GlaReader, should_reject_bones_that_do_not_form_a_tree) {
    auto builder = threeBoneChain();
    builder.bones[0].parent = 2; // root -> tip -> middle -> root
    auto bytes = builder.build();
    auto stream = MemoryInputStream(bytes);
    auto reader = GlaReader(stream);
    EXPECT_THROW(reader.load(), ValidationException);

    auto outside = threeBoneChain();
    outside.bones[1].parent = 7;
    auto outsideBytes = outside.build();
    auto outsideStream = MemoryInputStream(outsideBytes);
    auto outsideReader = GlaReader(outsideStream);
    EXPECT_THROW(outsideReader.load(), ValidationException);
}

TEST(GlaReader, should_reject_truncated_file) {
    auto bytes = threeBoneChain().build();
    bytes.resize(bytes.size() - 5);
    auto stream = MemoryInputStream(bytes);
    auto reader = GlaReader(stream);
    EXPECT_THROW(reader.load(), ValidationException);
}

TEST(GhoulAnimationConfig, should_parse_clips_and_skip_everything_else) {
    // given
    std::string text =
        "// animation.cfg\n"
        "//\n"
        "// Format:  enum, targetFrame, frameCount, loopFrame, frameSpeed\n"
        "\n"
        "BOTH_A2_T__B_       \t284\t11\t-1\t50\n"
        "both_stand2 15068 2 0 20 // trailing comment\n"
        "BOTH_STAND2TO1\t14943\t16\t-1\t-15\r\n"
        "BOTH_BROKEN 12\n"
        "123_NOT_A_NAME 1 2 -1 20\n"
        "BOTH_EMPTY 5 0 -1 20\n";

    // when
    auto clips = parseGhoulAnimationConfig(text);

    // then
    ASSERT_EQ(3ll, clips.size());
    EXPECT_EQ(284, clips.at("BOTH_A2_T__B_").firstFrame);
    EXPECT_EQ(11, clips.at("BOTH_A2_T__B_").numFrames);
    EXPECT_EQ(-1, clips.at("BOTH_A2_T__B_").loopFrames);
    EXPECT_FLOAT_EQ(50.0f, clips.at("BOTH_A2_T__B_").fps);
    EXPECT_EQ(0, clips.at("BOTH_STAND2").loopFrames);
    EXPECT_FLOAT_EQ(-15.0f, clips.at("BOTH_STAND2TO1").fps);
}
