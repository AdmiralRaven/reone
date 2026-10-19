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

#include <map>
#include <set>

#include "reone/game/jaanimator.h"
#include "reone/graphics/format/glareader.h"
#include "reone/graphics/format/mdlmdxreader.h"
#include "reone/graphics/options.h"
#include "reone/graphics/statistic.h"
#include "reone/scene/graphs.h"
#include "reone/scene/node/model.h"
#include "reone/scene/node/modelnode.h"
#include "reone/system/stream/fileinput.h"

#include "../fixtures/audio.h"
#include "../fixtures/graphics.h"
#include "../fixtures/jabody.h"
#include "../fixtures/resource.h"
#include "../fixtures/scene.h"

using namespace reone;
using namespace reone::audio;
using namespace reone::game;
using namespace reone::graphics;
using namespace reone::resource;
using namespace reone::scene;
using namespace reone::test;

namespace {

struct Body {
    std::shared_ptr<GhoulAnimation> gla;
    std::shared_ptr<Model> model;
    std::unique_ptr<JaRetargeter> retargeter;
    std::unique_ptr<JaSequencer> sequencer;

    Body(bool withSkin = true) :
        gla(loadGla(syntheticJaSkeleton())),
        model(syntheticKotorBody(withSkin)),
        retargeter(std::make_unique<JaRetargeter>(*gla, *model)),
        sequencer(std::make_unique<JaSequencer>(*retargeter, syntheticClips(), 20.0f)) {
    }
};

// A node's rotation in model space.
glm::quat modelRotation(const JaRetargeter &retargeter, const JaRetargeter::Pose &pose, const std::string &node) {
    return glm::normalize(glm::quat_cast(glm::mat3(retargeter.transforms(pose)[retargeter.nodeIndex(node)])));
}

float angleBetween(const glm::quat &a, const glm::quat &b) {
    glm::quat q(glm::inverse(a) * b);
    return 2.0f * std::atan2(glm::length(glm::vec3(q.x, q.y, q.z)), std::abs(q.w));
}

} // namespace

TEST(JaBodyAnimator, should_blend_from_the_pose_shown_into_a_clip_from_its_first_frame) {
    // given
    Body body;
    JaBodyAnimator animator(*body.sequencer, *body.retargeter);
    animator.play(JaBodyAnimator::kBody, "MOVE", 1.0f, 0.0f);
    animator.advance(0.05f);
    auto shown = body.sequencer->sample("MOVE", 0.05f);
    expectSamePose(shown, animator.pose());

    // when
    animator.play(JaBodyAnimator::kBody, "BACK", 1.0f, 0.1f);

    // then: from the pose shown, toward the new clip as it plays
    EXPECT_TRUE(animator.isBlending(JaBodyAnimator::kLegs));
    expectSamePose(shown, animator.pose());
    animator.advance(0.05f);
    expectSamePose(JaRetargeter::blend(shown, body.sequencer->sample("BACK", 0.05f), 0.5f), animator.pose());
    animator.advance(0.05f);
    EXPECT_FALSE(animator.isBlending(JaBodyAnimator::kLegs));
    expectSamePose(body.sequencer->sample("BACK", 0.1f), animator.pose());
    EXPECT_EQ(std::string("BACK"), animator.clip(JaBodyAnimator::kTorso));
}

TEST(JaBodyAnimator, should_play_the_legs_and_the_torso_apart) {
    // given
    Body body;
    JaBodyAnimator animator(*body.sequencer, *body.retargeter);
    animator.play(JaBodyAnimator::kBody, "BIND", 1.0f, 0.0f);

    // when: the legs play a clip of their own, at twice its speed
    animator.play(JaBodyAnimator::kLegs, "MOVE", 2.0f, 0.0f);
    animator.advance(0.05f);

    // then: the torso and everything under it stay with the first clip; the
    // pelvis, the legs and the root's position take the second
    auto pose = animator.pose();
    auto legs = body.sequencer->sample("MOVE", 0.1f);
    auto torso = body.sequencer->sample("BIND", 0.05f);
    const auto &nodes = body.retargeter->nodes();
    for (auto name : {"torso_g", "torsoupr_g", "headhook", "rbicep_g", "rhand"}) {
        int i = body.retargeter->nodeIndex(name);
        EXPECT_NEAR(1.0f, std::abs(glm::dot(torso.orientations[i], pose.orientations[i])), 1e-6f) << name;
    }
    for (auto name : {"pelvis_g", "lthigh_g", "rshin_g"}) {
        int i = body.retargeter->nodeIndex(name);
        EXPECT_NEAR(1.0f, std::abs(glm::dot(legs.orientations[i], pose.orientations[i])), 1e-6f) << name;
    }
    EXPECT_NEAR(0.0f, glm::distance(legs.rootPosition, pose.rootPosition), 1e-6f);
    EXPECT_FLOAT_EQ(0.1f, animator.clipTime(JaBodyAnimator::kLegs));
    EXPECT_FLOAT_EQ(0.05f, animator.clipTime(JaBodyAnimator::kTorso));
    EXPECT_EQ(nodes.size(), pose.orientations.size());
}

TEST(JaBodyAnimator, should_go_on_with_a_clip_already_playing_unless_restarted) {
    Body body;
    JaBodyAnimator animator(*body.sequencer, *body.retargeter);
    animator.play(JaBodyAnimator::kBody, "LOOP", 1.0f, 0.0f);
    animator.advance(0.15f);

    // The same clip again goes on, at the new speed.
    animator.play(JaBodyAnimator::kBody, "LOOP", 2.0f, 0.1f, false);
    EXPECT_FALSE(animator.isBlending(JaBodyAnimator::kLegs));
    animator.advance(0.05f);
    // 0.25 s into a clip of 2 frames at 10 fps wraps to 0.05 s.
    EXPECT_NEAR(0.05f, animator.clipTime(JaBodyAnimator::kLegs), 1e-6f);
    expectSamePose(body.sequencer->sample("LOOP", 0.05f), animator.pose());

    // Restarted, it starts over and blends.
    animator.play(JaBodyAnimator::kBody, "LOOP", 1.0f, 0.1f);
    EXPECT_TRUE(animator.isBlending(JaBodyAnimator::kTorso));
    EXPECT_FLOAT_EQ(0.0f, animator.clipTime(JaBodyAnimator::kTorso));
}

TEST(JaBodyAnimator, should_join_the_clip_the_other_part_plays_where_it_is) {
    // given: the legs 0.15 s into a loop, the torso on another clip
    Body body;
    JaBodyAnimator animator(*body.sequencer, *body.retargeter);
    animator.play(JaBodyAnimator::kBody, "MOVE", 1.0f, 0.0f);
    animator.play(JaBodyAnimator::kLegs, "LOOP", 1.0f, 0.0f);
    animator.advance(0.15f);

    // when: the torso takes up the loop, not restarted
    animator.play(JaBodyAnimator::kTorso, "LOOP", 1.0f, 0.1f, false);

    // then: it joins the legs where they are, blending
    EXPECT_FLOAT_EQ(0.15f, animator.clipTime(JaBodyAnimator::kTorso));
    EXPECT_TRUE(animator.isBlending(JaBodyAnimator::kTorso));

    // and restarted, it starts over
    animator.play(JaBodyAnimator::kTorso, "MOVE", 1.0f, 0.0f);
    animator.play(JaBodyAnimator::kTorso, "LOOP", 1.0f, 0.0f);
    EXPECT_FLOAT_EQ(0.0f, animator.clipTime(JaBodyAnimator::kTorso));
}

TEST(JaBodyAnimator, should_hold_a_pose_until_a_clip_plays_and_blend_from_it) {
    Body body;
    JaBodyAnimator animator(*body.sequencer, *body.retargeter);
    auto held = body.retargeter->pose(2);

    animator.hold(held);
    animator.advance(1.0f);
    expectSamePose(held, animator.pose());
    EXPECT_TRUE(animator.clip(JaBodyAnimator::kLegs).empty());

    animator.play(JaBodyAnimator::kBody, "BIND", 1.0f, 0.2f);
    animator.advance(0.1f);
    expectSamePose(JaRetargeter::blend(held, body.retargeter->pose(0), 0.5f), animator.pose());
}

TEST(JaBodyAnimator, should_turn_the_torso_and_not_the_legs) {
    // given
    Body body;
    const auto &retargeter = *body.retargeter;
    auto pose = retargeter.pose(1);
    auto turned = pose;

    // when
    jaTurnTorso(retargeter, turned, glm::radians(30.0f));

    // then: about the vertical, 24 degrees low on the spine, the rest higher up
    glm::vec3 up(0.0f, 0.0f, 1.0f);
    auto expectTurned = [&](const std::string &node, float degrees) {
        glm::quat expected(glm::angleAxis(glm::radians(degrees), up) * modelRotation(retargeter, pose, node));
        EXPECT_LT(angleBetween(expected, modelRotation(retargeter, turned, node)), glm::radians(0.01f)) << node;
    };
    expectTurned("torso_g", 24.0f);
    expectTurned("torsoupr_g", 30.0f);
    expectTurned("headhook", 30.0f);
    expectTurned("rhand", 30.0f);
    expectTurned("pelvis_g", 0.0f);
    expectTurned("lfoot_g", 0.0f);
    EXPECT_NEAR(0.0f, glm::distance(pose.rootPosition, turned.rootPosition), 1e-6f);
}

TEST(JaLivePose, should_pose_a_model_scene_node_with_each_pose_it_is_given) {
    // given
    auto graphicsOpt = GraphicsOptions();
    auto pipelineFactory = MockRenderPipelineFactory();
    auto graphicsModule = TestGraphicsModule();
    graphicsModule.init();
    auto audioModule = TestAudioModule();
    audioModule.init();
    auto resourceModule = TestResourceModule();
    resourceModule.init();
    auto scene = std::make_unique<SceneGraph>("test", pipelineFactory, graphicsOpt, graphicsModule.services(), audioModule.services(), resourceModule.services());
    Body body(false);
    JaLivePose live(*body.retargeter, "ja_live");
    auto model = std::make_shared<ModelSceneNode>(
        *body.model,
        ModelUsage::Creature,
        *scene,
        graphicsModule.services(),
        audioModule.services(),
        resourceModule.services());
    model->init();
    auto properties = AnimationProperties::fromFlags(AnimationFlags::loop);
    properties.scale = 1.0f;
    model->playAnimation(live.animation(), nullptr, properties);

    for (int frame : {1, 2, 0}) {
        // when
        auto pose = body.retargeter->pose(frame);
        live.set(pose);
        model->update(1.0f / 60.0f);

        // then: every node sits where the retargeter puts it for that pose
        auto expected = body.retargeter->transforms(pose);
        const auto &nodes = body.retargeter->nodes();
        for (size_t i = 0; i < nodes.size(); ++i) {
            auto sceneNode = model->getNodeByName(nodes[i].name);
            ASSERT_TRUE(sceneNode) << nodes[i].name;
            const glm::mat4 &actual = sceneNode->absoluteTransform();
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 4; ++r) {
                    EXPECT_NEAR(expected[i][c][r], actual[c][r], 1e-4f) << nodes[i].name << " in frame " << frame;
                }
            }
        }
    }

    // and the pose shown reads back
    expectSamePose(body.retargeter->pose(0), jaCurrentPose(*body.retargeter, *model), 1e-5f);
}

namespace {

// Every clip saber mode needs, made of the synthetic frames: the saber moves
// play MOVE's three frames, the walks and runs loop LOOP's two.
std::map<std::string, GhoulAnimationClip> saberClips() {
    auto clips = syntheticClips();
    for (auto &move : JaSaberMoves::fast().moves()) {
        clips[move.clip] = clips.at("MOVE");
    }
    for (auto &clip : JaSaberDriver::locomotionClips()) {
        clips[clip] = clips.at("LOOP");
    }
    return clips;
}

struct SaberBody {
    std::shared_ptr<GhoulAnimation> gla;
    std::shared_ptr<Model> model;
    std::map<std::string, GhoulAnimationClip> clips;
    std::unique_ptr<JaRetargeter> retargeter;
    std::unique_ptr<JaSequencer> sequencer;

    SaberBody(std::map<std::string, GhoulAnimationClip> withClips = saberClips()) :
        gla(loadGla(syntheticJaSkeleton())),
        model(syntheticKotorBody(true)),
        clips(std::move(withClips)),
        retargeter(std::make_unique<JaRetargeter>(*gla, *model)),
        sequencer(std::make_unique<JaSequencer>(*retargeter, clips, 20.0f)) {
    }

    JaRetargeter::Pose rest() const {
        JaRetargeter::Pose pose;
        for (auto &node : retargeter->nodes()) {
            pose.orientations.push_back(node.restOrientation);
        }
        pose.rootPosition = retargeter->nodes()[retargeter->rootNodeIndex()].restPosition;
        return pose;
    }
};

JaSaberInput holding(bool attack, int forward, int right) {
    JaSaberInput input;
    input.attack = attack;
    input.forward = forward;
    input.right = right;
    return input;
}

constexpr float kTick = 1.0f / 60.0f;

} // namespace

TEST(JaSaberDriver, should_attack_on_the_torso_while_the_legs_run_on) {
    // given
    SaberBody body;
    JaSaberDriver driver(*body.sequencer, *body.retargeter, body.clips);
    driver.start(body.rest());
    EXPECT_EQ(std::string("BOTH_SABERFAST_STANCE"), driver.animator().clip(JaBodyAnimator::kLegs));

    // when: forward
    auto step = driver.step(kTick, holding(false, 1, 0), false, 5.0f);

    // then: a run
    EXPECT_TRUE(step.moving);
    EXPECT_TRUE(step.started.empty());
    EXPECT_EQ(std::string("BOTH_RUN2"), driver.animator().clip(JaBodyAnimator::kLegs));
    EXPECT_EQ(std::string("BOTH_RUN2"), driver.animator().clip(JaBodyAnimator::kTorso));

    // when: attacking forward on the run
    step = driver.step(kTick, holding(true, 1, 0), false, 5.0f);

    // then: the start plays on the torso, and the character runs on at full speed
    EXPECT_TRUE(step.moving);
    EXPECT_FLOAT_EQ(1.0f, step.speedScale);
    ASSERT_EQ(1ll, step.started.size());
    EXPECT_EQ(std::string("LS_S_T2B"), JaSaberMoves::fast().move(step.started[0]).name);
    EXPECT_EQ(std::string("BOTH_RUN2"), driver.animator().clip(JaBodyAnimator::kLegs));
    EXPECT_EQ(std::string("BOTH_S1_S1_T_"), driver.animator().clip(JaBodyAnimator::kTorso));

    // when: still running with the button up, until back in the stance
    std::vector<std::string> played;
    bool stood = false;
    bool legsRan = true;
    for (int tick = 0; tick < 120; ++tick) {
        step = driver.step(kTick, holding(false, 1, 0), false, 5.0f);
        for (int move : step.started) {
            played.push_back(JaSaberMoves::fast().move(move).name);
        }
        stood |= !step.moving || step.speedScale != 1.0f;
        legsRan &= driver.animator().clip(JaBodyAnimator::kLegs) == "BOTH_RUN2";
        if (!played.empty() && played.back() == "LS_READY") {
            break;
        }
    }

    // then: the attack and its return play on the torso while the legs run,
    // and in the stance the torso runs with the legs again, in step with them
    std::vector<std::string> expected {"LS_A_T2B", "LS_R_T2B", "LS_READY"};
    EXPECT_EQ(expected, played);
    EXPECT_FALSE(stood);
    EXPECT_TRUE(legsRan);
    EXPECT_EQ(std::string("BOTH_RUN2"), driver.animator().clip(JaBodyAnimator::kTorso));
    EXPECT_FLOAT_EQ(driver.animator().clipTime(JaBodyAnimator::kLegs), driver.animator().clipTime(JaBodyAnimator::kTorso));
}

TEST(JaSaberDriver, should_attack_on_the_whole_body_standing_and_run_on_at_once) {
    // given: standing in the stance
    SaberBody body;
    JaSaberDriver driver(*body.sequencer, *body.retargeter, body.clips);
    driver.start(body.rest());
    driver.step(kTick, JaSaberInput(), false, 5.0f);

    // when: attacking from a standstill
    auto step = driver.step(kTick, holding(true, 0, 0), false, 5.0f);
    ASSERT_EQ(1ll, step.started.size());
    int start = step.started[0];

    // then: the start plays on the whole body
    EXPECT_FALSE(step.moving);
    EXPECT_EQ(JaMoveKind::Start, JaSaberMoves::fast().move(start).kind);
    const auto &clip = JaSaberMoves::fast().move(start).clip;
    EXPECT_EQ(clip, driver.animator().clip(JaBodyAnimator::kLegs));
    EXPECT_EQ(clip, driver.animator().clip(JaBodyAnimator::kTorso));

    // when: running forward while the start plays
    step = driver.step(kTick, holding(false, 1, 0), false, 5.0f);

    // then: moving takes the legs from the start at once, its hold
    // notwithstanding, and they run at full speed while the torso plays the
    // start on
    EXPECT_TRUE(step.moving);
    EXPECT_TRUE(step.started.empty());
    EXPECT_EQ(std::string("BOTH_RUN2"), driver.animator().clip(JaBodyAnimator::kLegs));
    EXPECT_TRUE(driver.animator().isBlending(JaBodyAnimator::kLegs));
    EXPECT_EQ(clip, driver.animator().clip(JaBodyAnimator::kTorso));
    EXPECT_FLOAT_EQ(1.0f, step.speedScale);
}

TEST(JaSaberDriver, should_join_the_legs_to_the_torsos_move_on_standing_still) {
    // given: an attack started on the run
    SaberBody body;
    JaSaberDriver driver(*body.sequencer, *body.retargeter, body.clips);
    driver.start(body.rest());
    driver.step(kTick, holding(false, 1, 0), false, 5.0f);
    driver.step(kTick, holding(true, 1, 0), false, 5.0f);
    driver.step(kTick, holding(false, 1, 0), false, 5.0f);
    std::string torso(driver.animator().clip(JaBodyAnimator::kTorso));
    ASSERT_EQ(std::string("BOTH_S1_S1_T_"), torso);
    ASSERT_EQ(std::string("BOTH_RUN2"), driver.animator().clip(JaBodyAnimator::kLegs));

    // when: letting go of the movement
    auto step = driver.step(kTick, JaSaberInput(), false, 5.0f);

    // then: the legs take up the torso's move where the torso is, blending
    // into it, while the torso plays on
    EXPECT_FALSE(step.moving);
    EXPECT_EQ(torso, driver.animator().clip(JaBodyAnimator::kLegs));
    EXPECT_EQ(torso, driver.animator().clip(JaBodyAnimator::kTorso));
    EXPECT_FLOAT_EQ(driver.animator().clipTime(JaBodyAnimator::kTorso), driver.animator().clipTime(JaBodyAnimator::kLegs));
    EXPECT_TRUE(driver.animator().isBlending(JaBodyAnimator::kLegs));

    // when: standing on until back in the stance
    const auto &moves = JaSaberMoves::fast();
    std::vector<std::string> played;
    for (int tick = 0; tick < 120 && moves.move(driver.machine().move()).kind != JaMoveKind::Ready; ++tick) {
        for (int id : driver.step(kTick, JaSaberInput(), false, 5.0f).started) {
            played.push_back(moves.move(id).name);
        }
        // then: the legs play the torso's moves with it
        EXPECT_EQ(driver.animator().clip(JaBodyAnimator::kTorso), driver.animator().clip(JaBodyAnimator::kLegs));
    }

    // and the attack, its return and the stance follow, on the whole body
    EXPECT_EQ(std::vector<std::string>({"LS_A_T2B", "LS_R_T2B", "LS_READY"}), played);
    EXPECT_EQ(std::string("BOTH_SABERFAST_STANCE"), driver.animator().clip(JaBodyAnimator::kLegs));
    EXPECT_EQ(std::string("BOTH_SABERFAST_STANCE"), driver.animator().clip(JaBodyAnimator::kTorso));
}

TEST(JaSaberDriver, should_move_backwards_at_three_quarters_of_its_speed_during_an_attack) {
    // given: walking backwards, which alone keeps full speed
    SaberBody body;
    JaSaberDriver driver(*body.sequencer, *body.retargeter, body.clips);
    driver.start(body.rest());
    auto step = driver.step(kTick, holding(false, -1, 0), true, 1.5f);
    EXPECT_EQ(std::string("BOTH_WALKBACK2"), driver.animator().clip(JaBodyAnimator::kLegs));
    EXPECT_FLOAT_EQ(1.0f, step.speedScale);

    // when: attacking on the way back, then letting go of the button, until
    // the stance
    std::map<JaMoveKind, std::set<float>> scales;
    driver.step(kTick, holding(true, -1, 0), true, 1.5f);
    for (int tick = 0; tick < 120; ++tick) {
        step = driver.step(kTick, holding(false, -1, 0), true, 1.5f);
        const auto &move = JaSaberMoves::fast().move(driver.machine().move());
        scales[move.kind].insert(step.speedScale);
        if (move.kind == JaMoveKind::Ready) {
            break;
        }
    }

    // then: 3/4 speed in the attack itself, full speed in its start, its
    // return and the stance
    EXPECT_EQ(std::set<float>({JaSaberDriver::kBackwardsAttackSpeed}), scales[JaMoveKind::Attack]);
    EXPECT_EQ(std::set<float>({1.0f}), scales[JaMoveKind::Start]);
    EXPECT_EQ(std::set<float>({1.0f}), scales[JaMoveKind::Return]);
    EXPECT_EQ(std::set<float>({1.0f}), scales[JaMoveKind::Ready]);

    // and forward during an attack, full speed
    JaSaberDriver forward(*body.sequencer, *body.retargeter, body.clips);
    forward.start(body.rest());
    forward.step(kTick, holding(true, 1, 0), false, 5.0f);
    for (int tick = 0; tick < 120; ++tick) {
        step = forward.step(kTick, holding(false, 1, 0), false, 5.0f);
        if (JaSaberMoves::fast().move(forward.machine().move()).kind == JaMoveKind::Attack) {
            break;
        }
    }
    EXPECT_EQ(JaMoveKind::Attack, JaSaberMoves::fast().move(forward.machine().move()).kind);
    EXPECT_FLOAT_EQ(1.0f, step.speedScale);
}

TEST(JaSaberDriver, should_run_backwards_at_three_quarters_of_its_speed_and_slower_again_in_an_attack) {
    // given
    SaberBody body;
    JaSaberDriver driver(*body.sequencer, *body.retargeter, body.clips);
    driver.start(body.rest());

    // when, then: running backwards, straight or to a side, at 3/4 of the
    // speed; running forward or sideways, and walking backwards, at full speed
    EXPECT_FLOAT_EQ(1.0f, driver.step(kTick, holding(false, 1, 0), false, 5.0f).speedScale);
    EXPECT_FLOAT_EQ(1.0f, driver.step(kTick, holding(false, 0, 1), false, 5.0f).speedScale);
    EXPECT_FLOAT_EQ(JaSaberDriver::kBackwardsRunSpeed, driver.step(kTick, holding(false, -1, 0), false, 5.0f).speedScale);
    EXPECT_FLOAT_EQ(JaSaberDriver::kBackwardsRunSpeed, driver.step(kTick, holding(false, -1, 1), false, 5.0f).speedScale);
    EXPECT_FLOAT_EQ(1.0f, driver.step(kTick, holding(false, -1, 0), true, 1.5f).speedScale);

    // when: attacking on the way back, running, then letting go of the
    // button, until the stance
    std::map<JaMoveKind, std::set<float>> scales;
    for (int tick = 0; tick < 120; ++tick) {
        auto step = driver.step(kTick, holding(tick == 0, -1, 0), false, 5.0f);
        const auto &move = JaSaberMoves::fast().move(driver.machine().move());
        scales[move.kind].insert(step.speedScale);
        if (move.kind == JaMoveKind::Ready) {
            break;
        }
    }

    // then: 3/4 of 3/4 in the attack itself, 3/4 in the rest
    EXPECT_EQ(std::set<float>({0.5625f}), scales[JaMoveKind::Attack]);
    EXPECT_EQ(std::set<float>({0.75f}), scales[JaMoveKind::Start]);
    EXPECT_EQ(std::set<float>({0.75f}), scales[JaMoveKind::Return]);
    EXPECT_EQ(std::set<float>({0.75f}), scales[JaMoveKind::Ready]);
}

TEST(JaSaberDriver, should_spin_on_the_whole_body_on_the_move_at_half_speed_facing_the_view) {
    // given: running forward and to the right, the legs turned toward the
    // movement
    SaberBody body;
    const auto &moves = JaSaberMoves::fast();
    JaSaberDriver driver(*body.sequencer, *body.retargeter, body.clips);
    driver.start(body.rest());
    for (int tick = 0; tick < 30; ++tick) {
        driver.step(kTick, holding(false, 1, 1), false, 5.0f);
    }
    ASSERT_NEAR(glm::radians(-45.0f), driver.legsYaw(), 1e-4f);

    // when: holding the attack button too, round twice
    int spin = moves.find("LS_T1_BR_TL");
    ASSERT_NE(JaSaberMoves::kNone, spin);
    const std::string &spinClip = moves.move(spin).clip;
    std::vector<std::string> played;
    std::vector<std::string> wholeBody; /**< the moves the legs took up as they started */
    std::set<float> spinScales;
    std::set<float> runScales;
    int spinTicks = 0;
    float spinYaw = 0.0f;
    for (int tick = 0; tick < 300 && played.size() < 6; ++tick) {
        auto step = driver.step(kTick, holding(true, 1, 1), false, 5.0f);
        const auto &legs = driver.animator().clip(JaBodyAnimator::kLegs);
        for (int id : step.started) {
            played.push_back(moves.move(id).name);
            if (legs == moves.move(id).clip) {
                wholeBody.push_back(moves.move(id).name);
            }
        }
        if (legs == spinClip) {
            ++spinTicks;
            spinScales.insert(step.speedScale);
            spinYaw = std::max(spinYaw, std::abs(driver.legsYaw()));
            // nor is the torso turned back from the legs
            expectSamePose(driver.animator().pose(), driver.pose());
        } else {
            EXPECT_EQ(std::string("BOTH_RUN2"), legs);
            runScales.insert(step.speedScale);
        }
    }

    // then: the attack goes round through the spin, which plays on the
    // whole body and holds the legs for its hold, at half speed, the body
    // facing the view; the rest plays on the torso, the legs running at full
    // speed
    std::vector<std::string> expected {"LS_S_TL2BR", "LS_A_TL2BR", "LS_T1_BR_TL", "LS_A_TL2BR", "LS_T1_BR_TL", "LS_A_TL2BR"};
    EXPECT_EQ(expected, played);
    EXPECT_EQ(std::vector<std::string>({"LS_T1_BR_TL", "LS_T1_BR_TL"}), wholeBody);
    EXPECT_NEAR(2.0f * driver.machine().timing(spin).holdMs / 1000.0f, spinTicks * kTick, 2.0f * kTick);
    EXPECT_EQ(std::set<float>({JaSaberDriver::kSpinSpeed}), spinScales);
    EXPECT_EQ(std::set<float>({1.0f}), runScales);
    EXPECT_FLOAT_EQ(0.0f, spinYaw);
    // and after it the legs turn from the view toward the movement again
    EXPECT_NEAR(glm::radians(-5.0f), driver.legsYaw(), 1e-4f);
}

TEST(JaSaberDriver, should_play_a_transition_that_isnt_a_spin_on_the_torso_on_the_move) {
    // given: running forward
    SaberBody body;
    const auto &moves = JaSaberMoves::fast();
    JaSaberDriver driver(*body.sequencer, *body.retargeter, body.clips);
    driver.start(body.rest());
    driver.step(kTick, holding(false, 1, 0), false, 5.0f);

    // when: holding the attack button too, until the attack goes round
    std::vector<std::string> played;
    std::set<float> scales;
    bool legsRan = true;
    for (int tick = 0; tick < 300 && played.size() < 4; ++tick) {
        auto step = driver.step(kTick, holding(true, 1, 0), false, 5.0f);
        for (int id : step.started) {
            played.push_back(moves.move(id).name);
        }
        scales.insert(step.speedScale);
        legsRan &= driver.animator().clip(JaBodyAnimator::kLegs) == "BOTH_RUN2";
    }

    // then: it goes round from the bottom by the bottom right's transition
    // to the top, which isn't a spin: on the torso, the legs running on at
    // full speed
    std::vector<std::string> expected {"LS_S_T2B", "LS_A_T2B", "LS_T1_BR_T_", "LS_A_T2B"};
    EXPECT_EQ(expected, played);
    EXPECT_FALSE(moves.move(moves.find("LS_T1_BR_T_")).spin);
    EXPECT_TRUE(legsRan);
    EXPECT_EQ(std::set<float>({1.0f}), scales);
}

TEST(JaSaberDriver, should_spin_at_half_speed_and_run_backwards_at_three_quarters_of_that) {
    // Holding back, right and the attack button goes round from the bottom
    // left to the top right through a spin.
    const auto &moves = JaSaberMoves::fast();
    SaberBody body;
    auto scales = [&](bool walk) {
        JaSaberDriver driver(*body.sequencer, *body.retargeter, body.clips);
        driver.start(body.rest());
        std::map<std::string, std::set<float>> byPart;
        std::vector<std::string> played;
        for (int tick = 0; tick < 300 && played.size() < 4; ++tick) {
            auto step = driver.step(kTick, holding(true, -1, 1), walk, walk ? 1.5f : 5.0f);
            for (int id : step.started) {
                played.push_back(moves.move(id).name);
            }
            JaMoveKind kind = moves.move(driver.machine().move()).kind;
            std::string part;
            if (driver.animator().clip(JaBodyAnimator::kLegs) == "BOTH_T1_TR_BL") {
                part = "spin";
            } else {
                part = kind == JaMoveKind::Attack ? "attack" : (kind == JaMoveKind::Start ? "start" : "other");
            }
            byPart[part].insert(step.speedScale);
        }
        std::vector<std::string> expected {"LS_S_BL2TR", "LS_A_BL2TR", "LS_T1_TR_BL", "LS_A_BL2TR"};
        EXPECT_EQ(expected, played) << (walk ? "walking" : "running");
        return byPart;
    };

    // running: 3/4 backwards; of that, 3/4 in the attack and half in the spin
    auto running = scales(false);
    EXPECT_EQ(std::set<float>({0.75f}), running["start"]);
    EXPECT_EQ(std::set<float>({0.5625f}), running["attack"]);
    EXPECT_EQ(std::set<float>({0.375f}), running["spin"]);
    EXPECT_TRUE(running["other"].empty());

    // walking: full speed backwards, 3/4 in the attack, half in the spin
    auto walking = scales(true);
    EXPECT_EQ(std::set<float>({1.0f}), walking["start"]);
    EXPECT_EQ(std::set<float>({0.75f}), walking["attack"]);
    EXPECT_EQ(std::set<float>({0.5f}), walking["spin"]);
    EXPECT_TRUE(walking["other"].empty());
}

TEST(JaSaberDriver, should_turn_the_legs_toward_a_sidestep_and_keep_the_torso_on_the_view) {
    // given
    SaberBody body;
    JaSaberDriver driver(*body.sequencer, *body.retargeter, body.clips);
    driver.start(body.rest());

    // when: a sidestep to the right, for a fifth of a second
    for (int tick = 0; tick < 12; ++tick) {
        driver.step(kTick, holding(false, 0, 1), false, 5.0f);
    }

    // then: the legs have turned 60 degrees to the right, at 300 degrees a second
    EXPECT_NEAR(glm::radians(-60.0f), driver.legsYaw(), 1e-4f);
    // and the torso back by as much, against the pose the clips give
    auto turned = driver.pose();
    auto unturned = driver.animator().pose();
    jaTurnTorso(*body.retargeter, unturned, glm::radians(60.0f));
    expectSamePose(unturned, turned);

    // when: standing again
    driver.step(kTick, JaSaberInput(), false, 5.0f);
    for (int tick = 0; tick < 6; ++tick) {
        driver.step(kTick, JaSaberInput(), false, 5.0f);
    }

    // then: the legs are straight again, at 600 degrees a second, and the
    // stance blends back in
    EXPECT_NEAR(0.0f, driver.legsYaw(), 1e-4f);
    EXPECT_EQ(std::string("BOTH_SABERFAST_STANCE"), driver.animator().clip(JaBodyAnimator::kLegs));
}

TEST(JaSaberDriver, should_blend_the_torso_into_a_walk_more_slowly_than_into_a_run) {
    SaberBody body;
    JaSaberDriver walking(*body.sequencer, *body.retargeter, body.clips);
    JaSaberDriver running(*body.sequencer, *body.retargeter, body.clips);
    walking.start(body.rest());
    running.start(body.rest());
    // Let the stance finish blending in.
    for (int tick = 0; tick < 30; ++tick) {
        walking.step(kTick, JaSaberInput(), false, 0.0f);
        running.step(kTick, JaSaberInput(), false, 0.0f);
    }

    for (int tick = 0; tick < 12; ++tick) {
        walking.step(kTick, holding(false, 1, 0), true, 1.5f);
        running.step(kTick, holding(false, 1, 0), false, 5.0f);
    }

    // 0.2 s in: the legs have finished their 100 ms blends; the walking
    // torso is still in its 350 ms one
    EXPECT_EQ(std::string("BOTH_WALK2"), walking.animator().clip(JaBodyAnimator::kLegs));
    EXPECT_FALSE(walking.animator().isBlending(JaBodyAnimator::kLegs));
    EXPECT_TRUE(walking.animator().isBlending(JaBodyAnimator::kTorso));
    EXPECT_FALSE(running.animator().isBlending(JaBodyAnimator::kTorso));
}

TEST(JaSaberDriver, should_name_missing_walks_and_runs) {
    auto clips = saberClips();
    clips.erase("BOTH_RUNBACK2");
    SaberBody body(clips);
    try {
        JaSaberDriver driver(*body.sequencer, *body.retargeter, body.clips);
        FAIL() << "expected an error";
    } catch (const std::runtime_error &e) {
        EXPECT_NE(std::string::npos, std::string(e.what()).find("BOTH_RUNBACK2")) << e.what();
    }
}

// Runs only with the player's own files:
//   REONE_TEST_JA_HUMANOID: folder with Jedi Academy's _humanoid.gla and animation.cfg
//   REONE_TEST_KOTOR_BODY: path of a KOTOR body model without extension (for example .../pmbbl)
//   REONE_TEST_JA_DUMP (optional): folder to write each scenario's poses and moves to
TEST(JaSaberDriverOnGameFiles, should_play_saber_mode_on_a_kotor_body) {
    const char *humanoidDir = std::getenv("REONE_TEST_JA_HUMANOID");
    const char *bodyPath = std::getenv("REONE_TEST_KOTOR_BODY");
    if (!humanoidDir || !bodyPath) {
        GTEST_SKIP() << "Set REONE_TEST_JA_HUMANOID and REONE_TEST_KOTOR_BODY to run against game files";
    }
    std::filesystem::path humanoid(humanoidDir);
    auto glaStream = FileInputStream(humanoid / "_humanoid.gla");
    auto glaReader = GlaReader(glaStream);
    glaReader.load();
    auto gla = glaReader.animation();
    std::ifstream cfgFile(humanoid / "animation.cfg");
    std::stringstream cfgText;
    cfgText << cfgFile.rdbuf();
    auto clips = parseGhoulAnimationConfig(cfgText.str());
    auto mdl = FileInputStream(std::string(bodyPath) + ".mdl");
    auto mdx = FileInputStream(std::string(bodyPath) + ".mdx");
    Statistic statistic;
    auto mdlReader = MdlMdxReader(mdl, mdx, statistic);
    mdlReader.load();
    auto body = mdlReader.model();
    auto retargeter = JaRetargeter(*gla, *body);
    auto sequencer = JaSequencer(retargeter, clips, 60.0f);
    const auto &moves = JaSaberMoves::fast();

    // Every move's clip and timing, as Jedi Academy's own numbers give them.
    JaSaberStateMachine machine(moves, clips);
    int reversed = 0;
    std::cout << "move clip frames fps hold_ms rate blend_ms" << std::endl;
    for (auto &move : moves.moves()) {
        const auto &clip = clips.at(move.clip);
        const auto &timing = machine.timing(move.id);
        reversed += clip.fps < 0.0f ? 1 : 0;
        std::cout << move.name << " " << move.clip << " " << clip.numFrames << " " << clip.fps << " " << timing.holdMs
                  << " " << timing.rate << " " << timing.blendMs << std::endl;
    }
    EXPECT_EQ(15, reversed) << "transitions played in reverse";
    EXPECT_EQ(169, machine.timing(moves.find("LS_A_TL2BR")).holdMs);
    EXPECT_EQ(349, machine.timing(moves.find("LS_S_TL2BR")).holdMs);
    EXPECT_EQ(170, machine.timing(moves.find("LS_R_TL2BR")).holdMs);
    EXPECT_EQ(400, machine.timing(moves.find("LS_T1_BR_TL")).holdMs);
    EXPECT_EQ(366, machine.timing(moves.find("LS_T1_TR_BL")).holdMs);
    EXPECT_EQ(0, machine.timing(moves.find("LS_T1__R_BR")).blendMs);
    EXPECT_EQ(50, machine.timing(moves.find("LS_A_BL2TR")).blendMs);
    for (auto &clip : JaSaberDriver::locomotionClips()) {
        std::cout << clip << " " << clips.at(clip).numFrames << " frames at " << clips.at(clip).fps << " fps, loops "
                  << (clips.at(clip).loopFrames >= 0) << std::endl;
    }
    std::cout << retargeter.scale() << " KOTOR units per JA unit" << std::endl;

    // Scenarios, as a player would press the keys, 60 ticks a second. Each
    // starts from KOTOR's bind pose and settles in the stance first.
    struct Scenario {
        std::string name;
        float seconds;
        std::function<JaSaberInput(float, const std::string &)> input; /**< time and the move playing */
        bool walk {false};
        std::vector<std::string> expected; /**< the moves that start, or nothing to leave unchecked */
    };
    auto tap = [](int forward, int right) {
        return [forward, right](float t, const std::string &) {
            JaSaberInput input;
            if (t >= 0.5f && t < 0.5f + 1.5f / 60.0f) {
                input.attack = true;
                input.forward = forward;
                input.right = right;
            }
            return input;
        };
    };
    auto holding = [](bool attack, int forward, int right) {
        JaSaberInput input;
        input.attack = attack;
        input.forward = forward;
        input.right = right;
        return input;
    };
    std::vector<Scenario> scenarios {
        {"tap_forward_right", 1.6f, tap(1, 1), false, {"LS_S_TL2BR", "LS_A_TL2BR", "LS_R_TL2BR", "LS_READY"}},
        {"tap_right", 1.6f, tap(0, 1), false, {"LS_S_L2R", "LS_A_L2R", "LS_R_L2R", "LS_READY"}},
        {"tap_back_right", 1.6f, tap(-1, 1), false, {"LS_S_BL2TR", "LS_A_BL2TR", "LS_R_BL2TR", "LS_READY"}},
        {"tap_forward_left", 1.6f, tap(1, -1), false, {"LS_S_TR2BL", "LS_A_TR2BL", "LS_R_TR2BL", "LS_READY"}},
        {"tap_left", 1.6f, tap(0, -1), false, {"LS_S_R2L", "LS_A_R2L", "LS_R_R2L", "LS_READY"}},
        {"tap_back_left", 1.6f, tap(-1, -1), false, {"LS_S_BR2TL", "LS_A_BR2TL", "LS_R_BR2TL", "LS_READY"}},
        {"tap_forward", 1.6f, tap(1, 0), false, {"LS_S_T2B", "LS_A_T2B", "LS_R_T2B", "LS_READY"}},
        {"tap_back", 1.6f, tap(-1, 0), false, {"LS_S_T2B", "LS_A_T2B", "LS_R_T2B", "LS_READY"}},
        {"tap", 1.6f, tap(0, 0), false, {"LS_S_R2L", "LS_A_R2L", "LS_R_R2L", "LS_READY"}},
        {"hold_forward_right", 2.0f, [&](float t, const std::string &) { return t >= 0.5f ? holding(true, 1, 1) : JaSaberInput(); }, false, {"LS_S_TL2BR", "LS_A_TL2BR", "LS_T1_BR_TL", "LS_A_TL2BR", "LS_T1_BR_TL"}},
        {"alternate", 2.0f, [&](float t, const std::string &move) {
             if (t < 0.5f) {
                 return JaSaberInput();
             }
             // The direction held as each attack ends picks the next.
             return move == "LS_A_TL2BR" ? holding(true, -1, -1) : holding(true, 1, 1);
         },
         false,
         {"LS_S_TL2BR", "LS_A_TL2BR", "LS_A_BR2TL", "LS_A_TL2BR", "LS_A_BR2TL", "LS_A_TL2BR", "LS_A_BR2TL", "LS_A_TL2BR"}},
        {"hold", 2.4f, [&](float t, const std::string &) { return t >= 0.5f ? holding(true, 0, 0) : JaSaberInput(); }, false, {"LS_S_R2L", "LS_A_R2L", "LS_R_R2L", "LS_READY", "LS_S_R2L", "LS_A_R2L", "LS_R_R2L", "LS_READY", "LS_S_R2L"}},
        {"sidesteps", 4.0f, [&](float t, const std::string &) {
             if (t < 0.5f || t >= 3.5f) {
                 return JaSaberInput();
             }
             return t < 1.5f ? holding(false, 0, 1) : (t < 2.5f ? holding(false, -1, 0) : holding(false, 1, -1));
         },
         false,
         {}},
        {"walk", 3.0f, [&](float t, const std::string &) {
             if (t < 0.5f || t >= 2.5f) {
                 return JaSaberInput();
             }
             return t < 1.5f ? holding(false, 1, 0) : holding(false, -1, 0);
         },
         true,
         {}},
        {"run_then_attack", 3.0f, [&](float t, const std::string &) {
             if (t < 0.5f) {
                 return JaSaberInput();
             }
             return holding(t >= 1.5f && t < 2.0f, 1, 0);
         },
         false,
         {}},
    };

    const char *dumpDir = std::getenv("REONE_TEST_JA_DUMP");
    int weapon = retargeter.nodeIndex("rhand");
    for (auto &scenario : scenarios) {
        JaSaberDriver driver(sequencer, retargeter, clips);
        JaRetargeter::Pose bind;
        for (auto &node : retargeter.nodes()) {
            bind.orientations.push_back(node.restOrientation);
        }
        bind.rootPosition = retargeter.nodes()[retargeter.rootNodeIndex()].restPosition;
        driver.start(bind);

        std::ofstream poses;
        std::ofstream events;
        std::ofstream states;
        if (dumpDir) {
            poses.open(std::filesystem::path(dumpDir) / (scenario.name + ".txt"));
            poses << std::setprecision(9);
            events.open(std::filesystem::path(dumpDir) / (scenario.name + "_moves.txt"));
            states.open(std::filesystem::path(dumpDir) / (scenario.name + "_ticks.txt"));
            states << std::setprecision(6);
        }
        std::vector<std::string> started;
        float worstStep = 0.0f;
        std::string worstAt;
        int ticks = static_cast<int>(std::lround(scenario.seconds * 60.0f));
        glm::vec3 previousBlade(0.0f);
        for (int tick = 0; tick <= ticks; ++tick) {
            float t = tick / 60.0f;
            const auto &playing = moves.move(driver.machine().move()).name;
            // KOTOR's player walks and runs at appearance.2da's walkdist and
            // rundist, in metres a second, as reone moves creatures.
            auto step = driver.step(tick == 0 ? 0.0f : 1.0f / 60.0f, scenario.input(t, playing), scenario.walk, scenario.walk ? 1.813f : 3.96f);
            for (int move : step.started) {
                started.push_back(moves.move(move).name);
                if (events.is_open()) {
                    events << t << " " << moves.move(move).name << " " << moves.move(move).clip << "\n";
                }
            }
            if (states.is_open()) {
                const auto &animator = driver.animator();
                states << t << " " << moves.move(driver.machine().move()).name << " " << step.moving << " "
                       << glm::degrees(driver.legsYaw()) << " " << animator.clip(JaBodyAnimator::kLegs) << " "
                       << animator.clip(JaBodyAnimator::kTorso) << "\n";
            }
            auto pose = driver.pose();
            for (auto &q : pose.orientations) {
                ASSERT_TRUE(std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z))
                    << scenario.name << " at " << t;
            }
            // The whole body turns with the legs.
            glm::mat4 facing(glm::rotate(driver.legsYaw(), glm::vec3(0.0f, 0.0f, 1.0f)));
            auto transforms = retargeter.transforms(pose);
            glm::vec3 blade(glm::normalize(glm::mat3(facing) * glm::vec3(transforms[weapon][2])));
            if (tick > 0) {
                float turned = std::atan2(glm::length(glm::cross(previousBlade, blade)), glm::dot(previousBlade, blade));
                if (turned > worstStep) {
                    worstStep = turned;
                    worstAt = str(boost::format("%.3f s in %s") % t % playing);
                }
            }
            previousBlade = blade;
            if (poses.is_open()) {
                for (size_t i = 0; i < transforms.size(); ++i) {
                    glm::mat4 m(facing * transforms[i]);
                    poses << t << " " << retargeter.nodes()[i].name;
                    for (int c = 0; c < 4; ++c) {
                        for (int r = 0; r < 4; ++r) {
                            poses << " " << m[c][r];
                        }
                    }
                    poses << "\n";
                }
            }
        }
        std::cout << scenario.name << ": " << boost::join(started, " ") << "; the blade turns at most "
                  << glm::degrees(worstStep) << " degrees in a tick, at " << worstAt << std::endl;
        if (!scenario.expected.empty()) {
            EXPECT_EQ(scenario.expected, started) << scenario.name;
        }
    }
}
