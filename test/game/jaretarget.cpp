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

#include "reone/game/jaretarget.h"
#include "reone/graphics/animation.h"
#include "reone/graphics/format/glareader.h"
#include "reone/graphics/format/mdlmdxreader.h"
#include "reone/graphics/ghoulanimation.h"
#include "reone/graphics/model.h"
#include "reone/graphics/modelnode.h"
#include "reone/graphics/options.h"
#include "reone/graphics/statistic.h"
#include "reone/graphics/types.h"
#include "reone/scene/graphs.h"
#include "reone/scene/node/model.h"
#include "reone/scene/node/modelnode.h"
#include "reone/system/stream/fileinput.h"
#include "reone/system/stream/memoryinput.h"

#include "../fixtures/audio.h"
#include "../fixtures/gla.h"
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

constexpr float kDegree = glm::pi<float>() / 180.0f;

// Precise near zero, unlike the arccosine of a dot product.
float angleBetween(const glm::vec3 &a, const glm::vec3 &b) {
    return std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
}

glm::vec3 toKotorAxes(const glm::vec3 &v) {
    return glm::vec3(-v.x, -v.y, v.z);
}

// The angle of the turn from one rotation to another, precise near zero.
float rotationAngle(const glm::mat3 &from, const glm::mat3 &to) {
    glm::quat q(glm::quat_cast(glm::transpose(from) * to));
    return 2.0f * std::atan2(glm::length(glm::vec3(q.x, q.y, q.z)), std::abs(q.w));
}

bool isFinite(const glm::quat &q) {
    return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z);
}

// JA's saber tag in a frame, without Ghoul2's scale: +Z kept, +Y squared to it.
glm::mat3 tagRotation(const GhoulAnimation &gla, int frame) {
    glm::mat3 tag(gla.boneTransforms(frame)[gla.boneIndex("rhang_tag_bone")]);
    glm::vec3 z(glm::normalize(tag[2]));
    glm::vec3 x(glm::normalize(glm::cross(tag[1], z)));
    return glm::mat3(x, glm::cross(z, x), z);
}

} // namespace

TEST(JaRetargeter, should_point_kotor_bones_along_their_ja_bones) {
    // given
    auto gla = loadGla(syntheticJaSkeleton());
    auto body = syntheticKotorBody(true);
    auto retargeter = JaRetargeter(*gla, *body);

    for (int frame = 0; frame < gla->numFrames(); ++frame) {
        // when
        auto kotor = retargeter.transforms(retargeter.pose(frame));

        // then
        auto ja = gla->boneTransforms(frame);
        for (auto &mapping : JaRetargeter::boneMappings()) {
            if (!*mapping.kotorChild) {
                continue;
            }
            glm::vec3 kotorDirection(kotor[retargeter.nodeIndex(mapping.kotorChild)][3] - kotor[retargeter.nodeIndex(mapping.kotor)][3]);
            glm::vec3 jaDirection(toKotorAxes(glm::vec3(ja[gla->boneIndex(mapping.jaChild)][3] - ja[gla->boneIndex(mapping.ja)][3])));
            EXPECT_LT(angleBetween(kotorDirection, jaDirection), 0.5f * kDegree)
                << mapping.kotor << " in frame " << frame;
        }
    }
}

TEST(JaRetargeter, should_turn_the_weapon_along_the_saber_tag) {
    // given
    auto gla = loadGla(syntheticJaSkeleton());
    auto body = syntheticKotorBody(true);
    auto retargeter = JaRetargeter(*gla, *body);
    int weapon = retargeter.nodeIndex("rhand");

    for (int frame = 0; frame < gla->numFrames(); ++frame) {
        // when
        auto kotor = retargeter.transforms(retargeter.pose(frame));

        // then: weapon +Z is the tag's +Z, in KOTOR's axes
        auto ja = gla->boneTransforms(frame);
        glm::vec3 tagZ(toKotorAxes(glm::vec3(ja[gla->boneIndex("rhang_tag_bone")][2])));
        glm::vec3 weaponZ(kotor[weapon][2]);
        EXPECT_LT(angleBetween(weaponZ, tagZ), 0.05f * kDegree) << "frame " << frame;
        EXPECT_LT(angleBetween(retargeter.bladeDirection(frame), tagZ), 0.01f * kDegree);
        EXPECT_NEAR(1.0f, glm::length(weaponZ), 1e-4f);
    }
    // In the bind pose the tag points forward: KOTOR's +Y.
    auto bind = retargeter.transforms(retargeter.pose(0));
    EXPECT_LT(angleBetween(glm::vec3(bind[weapon][2]), glm::vec3(0.0f, 1.0f, 0.0f)), 0.05f * kDegree);
}

TEST(JaRetargeter, should_move_the_root_with_the_pelvis_and_scale_by_pelvis_height) {
    // given
    auto gla = loadGla(syntheticJaSkeleton());
    auto body = syntheticKotorBody(true);
    auto retargeter = JaRetargeter(*gla, *body);
    int root = retargeter.nodeIndex("rootdummy");

    // when
    auto bind = retargeter.transforms(retargeter.pose(0));
    auto crouched = retargeter.transforms(retargeter.pose(1));

    // then
    EXPECT_NEAR(1.0f / 36.0f, retargeter.scale(), 1e-6f);
    EXPECT_NEAR(1.0f, bind[root][3].z, 1e-5f);
    // The pelvis drops 3 units; the model root's -24 is the player origin and is ignored.
    EXPECT_NEAR(0.0f, crouched[root][3].x, 1e-3f);
    EXPECT_NEAR(0.0f, crouched[root][3].y, 1e-3f);
    EXPECT_NEAR(1.0f - 3.0f / 36.0f, crouched[root][3].z, 1e-3f);
}

TEST(JaRetargeter, should_keep_unmapped_nodes_at_rest_and_leave_skins_alone) {
    // given
    auto gla = loadGla(syntheticJaSkeleton());
    auto body = syntheticKotorBody(true);
    auto retargeter = JaRetargeter(*gla, *body);

    // when
    auto pose = retargeter.pose(1);

    // then
    const auto &nodes = retargeter.nodes();
    int finger = retargeter.nodeIndex("rfinger1");
    EXPECT_EQ(nodes[finger].restOrientation, pose.orientations[finger]);
    EXPECT_TRUE(nodes[finger].posed);
    EXPECT_FALSE(nodes[retargeter.nodeIndex("torso_skin")].posed);
    EXPECT_FALSE(nodes[retargeter.nodeIndex("synthetic_body")].posed);
    EXPECT_TRUE(nodes[retargeter.nodeIndex("rootdummy")].posed);
}

TEST(JaRetargeter, should_name_what_a_body_is_missing) {
    auto gla = loadGla(syntheticJaSkeleton());
    auto body = syntheticKotorBody(false, "lshin_g");
    try {
        JaRetargeter retargeter(*gla, *body);
        FAIL() << "expected a missing node error";
    } catch (const std::runtime_error &e) {
        EXPECT_NE(std::string::npos, std::string(e.what()).find("lshin_g"));
    }
}

TEST(JaRetargeter, should_turn_the_hand_with_the_saber_tag_when_the_blade_lies_along_the_forearm) {
    // given: the hand turned so that the blade points along the forearm, against
    // it, and 2 degrees to either side of it, where the forearm's angle around
    // the blade jumps by half a turn
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    const glm::vec3 across(0.0f, 1.0f, 0.0f);
    auto gla = loadGla(syntheticJaSkeleton({turn(90.0f, up),
                                            turn(-90.0f, up),
                                            turn(2.0f, across) * turn(90.0f, up),
                                            turn(-2.0f, across) * turn(90.0f, up)}));
    auto body = syntheticKotorBody(true);
    auto retargeter = JaRetargeter(*gla, *body);
    int weapon = retargeter.nodeIndex("rhand");
    ASSERT_EQ(7, gla->numFrames());
    for (int frame : {3, 4}) {
        auto ja = gla->boneTransforms(frame);
        glm::vec3 forearm(ja[gla->boneIndex("rradius")][3] - ja[gla->boneIndex("rhand")][3]);
        glm::vec3 blade(ja[gla->boneIndex("rhang_tag_bone")][2]);
        ASSERT_GT(std::abs(glm::dot(glm::normalize(forearm), glm::normalize(blade))), std::cos(0.1f * kDegree)) << "frame " << frame;
    }

    // when
    std::vector<glm::mat3> weapons;
    for (int frame = 0; frame < gla->numFrames(); ++frame) {
        auto pose = retargeter.pose(frame);
        for (size_t i = 0; i < pose.orientations.size(); ++i) {
            ASSERT_TRUE(isFinite(pose.orientations[i])) << retargeter.nodes()[i].name << " in frame " << frame;
        }
        weapons.push_back(glm::mat3(retargeter.transforms(pose)[weapon]));
    }

    // then: the blade follows the tag, and from any frame to any other the hand
    // turns exactly as far as the tag does
    for (int a = 0; a < gla->numFrames(); ++a) {
        EXPECT_LT(angleBetween(weapons[a][2], retargeter.bladeDirection(a)), 0.01f * kDegree) << "frame " << a;
        for (int b = a + 1; b < gla->numFrames(); ++b) {
            EXPECT_NEAR(rotationAngle(tagRotation(*gla, a), tagRotation(*gla, b)), rotationAngle(weapons[a], weapons[b]), 0.01f * kDegree)
                << "frames " << a << " and " << b;
        }
    }
    // Across the forearm's line that is 4 degrees, not half a turn.
    EXPECT_NEAR(4.0f * kDegree, rotationAngle(weapons[5], weapons[6]), 0.05f * kDegree);
}

TEST(JaRetargeter, should_turn_the_bind_weapon_onto_the_blade_when_the_bind_forearm_lies_along_it) {
    // given: a body whose bind pose holds the blade along the forearm, so the
    // forearm cannot set the twist
    auto gla = loadGla(syntheticJaSkeleton());
    auto body = syntheticKotorBody(true, "", turn(-45.0f, {0, 1, 0}));
    auto retargeter = JaRetargeter(*gla, *body);
    const auto &nodes = retargeter.nodes();
    int weapon = retargeter.nodeIndex("rhand");
    glm::vec3 forearm(nodes[retargeter.nodeIndex("rforearm_g")].bindTransform[3] - nodes[retargeter.nodeIndex("rhand_g")].bindTransform[3]);
    glm::mat3 weaponBind(nodes[weapon].bindTransform);
    ASSERT_LT(angleBetween(weaponBind[2], forearm), 0.1f * kDegree);

    // when
    auto bind = retargeter.transforms(retargeter.pose(0));

    // then: KOTOR's bind weapon is turned the shortest way onto the blade,
    // about the axis across both
    glm::mat3 weaponAtBind(bind[weapon]);
    EXPECT_LT(angleBetween(weaponAtBind[2], retargeter.bladeDirection(0)), 0.01f * kDegree);
    glm::vec3 axis(glm::normalize(glm::cross(weaponBind[2], weaponAtBind[2])));
    EXPECT_LT(glm::distance(axis, weaponAtBind * glm::transpose(weaponBind) * axis), 1e-4f);
    for (int frame = 1; frame < gla->numFrames(); ++frame) {
        auto kotor = retargeter.transforms(retargeter.pose(frame));
        EXPECT_LT(angleBetween(glm::vec3(kotor[weapon][2]), retargeter.bladeDirection(frame)), 0.01f * kDegree);
        EXPECT_NEAR(rotationAngle(tagRotation(*gla, 0), tagRotation(*gla, frame)), rotationAngle(weaponAtBind, glm::mat3(kotor[weapon])), 0.01f * kDegree);
    }
}

TEST(JaRetargeter, should_refuse_ja_bones_without_a_usable_base_rotation) {
    auto body = syntheticKotorBody(true);
    for (std::string name : {"rhang_tag_bone", "lfemurYZ"}) {
        auto builder = syntheticJaSkeleton();
        for (auto &bone : builder.bones) {
            if (bone.name == name) {
                bone.basePose[0] = bone.basePose[1] = bone.basePose[2] = glm::vec4(0.0f);
            }
        }
        auto gla = loadGla(builder);
        try {
            JaRetargeter retargeter(*gla, *body);
            ADD_FAILURE() << "expected an error for " << name;
        } catch (const std::runtime_error &e) {
            EXPECT_NE(std::string::npos, std::string(e.what()).find(name)) << e.what();
        }
    }
}

TEST(JaSequencer, should_sample_clips_between_frames_backwards_and_looping) {
    // given
    auto gla = loadGla(syntheticJaSkeleton());
    auto body = syntheticKotorBody(true);
    auto retargeter = JaRetargeter(*gla, *body);
    auto sequencer = JaSequencer(retargeter, syntheticClips(), 20.0f);
    auto frame0 = retargeter.pose(0);
    auto frame1 = retargeter.pose(1);
    auto frame2 = retargeter.pose(2);

    // then
    EXPECT_FLOAT_EQ(0.2f, sequencer.clipLength("MOVE"));
    EXPECT_TRUE(sequencer.hasClip("move"));
    EXPECT_FALSE(sequencer.hasClip("JUMP"));
    expectSamePose(frame0, sequencer.sample("MOVE", 0.0f));
    expectSamePose(frame1, sequencer.sample("MOVE", 0.1f));
    expectSamePose(JaRetargeter::blend(frame0, frame1, 0.5f), sequencer.sample("MOVE", 0.05f));
    expectSamePose(frame2, sequencer.sample("MOVE", 5.0f));
    // A negative rate plays the frames last to first.
    expectSamePose(frame2, sequencer.sample("BACK", 0.0f));
    expectSamePose(frame0, sequencer.sample("BACK", 0.2f));
    // Looping clips wrap from their last frame to their first.
    expectSamePose(frame1, sequencer.sample("LOOP", 0.2f));
    expectSamePose(JaRetargeter::blend(frame2, frame1, 0.5f), sequencer.sample("LOOP", 0.15f));
    EXPECT_THROW(sequencer.sample("JUMP", 0.0f), std::runtime_error);
}

TEST(JaSequencer, should_build_a_kotor_animation_of_a_sequence) {
    // given
    auto gla = loadGla(syntheticJaSkeleton());
    auto body = syntheticKotorBody(true);
    auto retargeter = JaRetargeter(*gla, *body);
    auto sequencer = JaSequencer(retargeter, syntheticClips(), 20.0f);
    std::vector<JaSequenceStep> steps {{"BIND", 0.1f, 0.0f}, {"MOVE", -1.0f, 0.1f}};

    // when
    auto timeline = sequencer.timeline(steps);
    auto animation = sequencer.animation("ja_test", steps, 0.25f);

    // then: 2 samples of BIND, then MOVE's 5, the first 2 blended in from BIND
    ASSERT_EQ(7ll, timeline.size());
    expectSamePose(retargeter.pose(0), timeline[1]);
    expectSamePose(JaRetargeter::blend(retargeter.pose(0), sequencer.sample("MOVE", 0.05f), 0.5f), timeline[3]);
    expectSamePose(retargeter.pose(1), timeline[4]);
    expectSamePose(retargeter.pose(2), timeline[6]);

    EXPECT_EQ(std::string("ja_test"), animation->name());
    EXPECT_FLOAT_EQ(0.3f, animation->length());
    EXPECT_FLOAT_EQ(0.25f, animation->transitionTime());
    EXPECT_FALSE(static_cast<bool>(animation->getNodeByName("torso_skin")));
    EXPECT_FALSE(static_cast<bool>(animation->getNodeByName("synthetic_body")));
    auto root = animation->getNodeByName("rootdummy");
    ASSERT_TRUE(static_cast<bool>(root));
    const auto &rootNode = retargeter.nodes()[retargeter.nodeIndex("rootdummy")];
    glm::vec3 offset;
    ASSERT_TRUE(root->positionAtTime(0.2f, offset));
    EXPECT_NEAR(0.0f, glm::distance(timeline[4].rootPosition - rootNode.restPosition, offset), 1e-5f);
    auto finger = animation->getNodeByName("rfinger1");
    ASSERT_TRUE(static_cast<bool>(finger));
    EXPECT_EQ(1ll, finger->quaternionTracks().at(ControllerTypes::orientation).keyframes().size());
    auto bicep = animation->getNodeByName("rbicep_g");
    ASSERT_TRUE(static_cast<bool>(bicep));
    EXPECT_EQ(7ll, bicep->quaternionTracks().at(ControllerTypes::orientation).keyframes().size());
}

TEST(JaSequencer, should_end_a_whole_clip_on_its_last_frame_between_samples) {
    // given: 3 frames at 50 fps take 0.04 s, 2.4 intervals at 60 Hz
    auto gla = loadGla(syntheticJaSkeleton());
    auto body = syntheticKotorBody(true);
    auto retargeter = JaRetargeter(*gla, *body);
    auto clips = syntheticClips();
    clips["ODD"] = {0, 3, -1, 50.0f};
    clips["ODDLOOP"] = {0, 3, 0, 50.0f};
    auto sequencer = JaSequencer(retargeter, clips, 60.0f);
    auto frame0 = retargeter.pose(0);
    auto frame1 = retargeter.pose(1);
    auto frame2 = retargeter.pose(2);

    for (std::string name : {"ODD", "ODDLOOP"}) {
        // when
        auto timeline = sequencer.timeline({{name, -1.0f, 0.0f}});

        // then: samples at 0, 1/60 and 2/60 s, then the last frame at 0.04 s,
        // not wrapped toward the first when the clip loops
        ASSERT_EQ(4ll, timeline.size()) << name;
        expectSamePose(frame0, timeline[0]);
        expectSamePose(JaRetargeter::blend(frame0, frame1, 50.0f / 60.0f), timeline[1]);
        expectSamePose(JaRetargeter::blend(frame1, frame2, 40.0f / 60.0f), timeline[2]);
        expectSamePose(frame2, timeline[3]);
    }

    // and the next step blends from that last frame
    auto timeline = sequencer.timeline({{"ODD", -1.0f, 0.0f}, {"BIND", 0.05f, 0.05f}});
    ASSERT_EQ(7ll, timeline.size());
    expectSamePose(JaRetargeter::blend(frame2, frame0, 1.0f / 3.0f), timeline[5]);
}

TEST(JaSequencer, should_pose_a_model_scene_node_through_the_engine_animation_path) {
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

    auto gla = loadGla(syntheticJaSkeleton());
    auto body = syntheticKotorBody(false);
    auto retargeter = JaRetargeter(*gla, *body);
    auto sequencer = JaSequencer(retargeter, syntheticClips(), 20.0f);
    std::vector<JaSequenceStep> steps {{"MOVE", -1.0f, 0.0f}};
    auto timeline = sequencer.timeline(steps);
    auto animation = sequencer.animation("ja_move", steps, 0.25f);
    auto modelSceneNode = std::make_shared<ModelSceneNode>(
        *body,
        ModelUsage::Creature,
        *scene,
        graphicsModule.services(),
        audioModule.services(),
        resourceModule.services());
    modelSceneNode->init();

    // when: half way, on the key for JA frame 1
    auto properties = AnimationProperties::fromFlags(AnimationFlags::fireForget);
    properties.scale = 1.0f;
    modelSceneNode->playAnimation(*animation, nullptr, properties);
    modelSceneNode->update(0.1f);

    // then: every node sits where the retargeter put it
    auto expected = retargeter.transforms(timeline[2]);
    const auto &nodes = retargeter.nodes();
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto sceneNode = modelSceneNode->getNodeByName(nodes[i].name);
        ASSERT_TRUE(sceneNode) << nodes[i].name;
        const glm::mat4 &actual = sceneNode->absoluteTransform();
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                EXPECT_NEAR(expected[i][c][r], actual[c][r], 1e-4f) << nodes[i].name << " [" << c << "][" << r << "]";
            }
        }
    }
}

TEST(JaNamedMove, should_spell_out_the_named_swing_and_parry) {
    auto swing = jaNamedMove("swing");
    ASSERT_EQ(5ll, swing.size());
    EXPECT_EQ(std::string("BOTH_SABERFAST_STANCE"), swing[0].clip);
    EXPECT_EQ(std::string("BOTH_S1_S1_TL"), swing[1].clip);
    EXPECT_EQ(std::string("BOTH_A1_TL_BR"), swing[2].clip);
    EXPECT_EQ(std::string("BOTH_R1_BR_S1"), swing[3].clip);
    EXPECT_EQ(std::string("BOTH_SABERFAST_STANCE"), swing[4].clip);
    for (int i = 1; i <= 3; ++i) {
        EXPECT_LT(swing[i].seconds, 0.0f) << "start, attack and return play in full";
        EXPECT_FLOAT_EQ(0.1f, swing[i].blendIn);
    }
    EXPECT_FLOAT_EQ(0.35f, swing[4].blendIn) << "back into the ready stance";
    auto block = jaNamedMove("block");
    ASSERT_EQ(3ll, block.size());
    EXPECT_EQ(std::string("BOTH_P1_S1_TR"), block[1].clip);
    EXPECT_FLOAT_EQ(0.05f, block[1].blendIn);
    EXPECT_FLOAT_EQ(0.35f, block[2].blendIn);
    EXPECT_TRUE(jaNamedMove("dance").empty());
}

// Runs only with the player's own files:
//   REONE_TEST_JA_HUMANOID: folder with Jedi Academy's _humanoid.gla and animation.cfg
//   REONE_TEST_KOTOR_BODY: path of a KOTOR body model without extension (for example .../pmbam)
//   REONE_TEST_JA_DUMP (optional): folder to write the posed node transforms to
TEST(JaRetargeterOnGameFiles, should_play_a_swing_and_a_block_on_a_kotor_body) {
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
    std::cout << "GLA " << gla->name() << ": " << gla->numBones() << " bones, " << gla->numFrames()
              << " frames, " << clips.size() << " clips; body " << body->name()
              << ", " << retargeter.scale() << " KOTOR units per JA unit" << std::endl;

    // Every posed node must be one the engine animates.
    std::function<void(const ModelNode &)> checkAnimated = [&](const ModelNode &node) {
        int index = retargeter.nodeIndex(node.name());
        if (index >= 0 && retargeter.nodes()[index].posed) {
            EXPECT_TRUE(node.isAnimated()) << node.name();
        }
        for (auto &child : node.children()) {
            checkAnimated(*child);
        }
    };
    checkAnimated(*body->rootNode());

    // Over every frame of the moves: the blade follows the saber tag, and the
    // bones follow their JA counterparts.
    float worstBlade = 0.0f;
    std::map<std::string, float> worstBone;
    int weapon = retargeter.nodeIndex("rhand");
    std::set<std::string> moveClips;
    for (auto move : {"swing", "block", "overhead", "guard"}) {
        for (auto &step : jaNamedMove(move)) {
            moveClips.insert(step.clip);
        }
    }
    // Each error is checked before it is kept: std::max would drop a NaN.
    for (auto &name : moveClips) {
        const auto &clip = sequencer.clip(name);
        for (int frame = clip.firstFrame; frame < clip.firstFrame + clip.numFrames; ++frame) {
            auto kotor = retargeter.transforms(retargeter.pose(frame));
            auto ja = gla->boneTransforms(frame);
            float bladeError = angleBetween(glm::vec3(kotor[weapon][2]), retargeter.bladeDirection(frame));
            ASSERT_TRUE(std::isfinite(bladeError)) << name << " frame " << frame;
            worstBlade = std::max(worstBlade, bladeError);
            for (auto &mapping : JaRetargeter::boneMappings()) {
                if (!*mapping.kotorChild) {
                    continue;
                }
                glm::vec3 kotorDirection(kotor[retargeter.nodeIndex(mapping.kotorChild)][3] - kotor[retargeter.nodeIndex(mapping.kotor)][3]);
                glm::vec3 jaDirection(toKotorAxes(glm::vec3(ja[gla->boneIndex(mapping.jaChild)][3] - ja[gla->boneIndex(mapping.ja)][3])));
                float boneError = angleBetween(kotorDirection, jaDirection);
                ASSERT_TRUE(std::isfinite(boneError)) << mapping.kotor << " in " << name << " frame " << frame;
                float &worst = worstBone[mapping.kotor];
                worst = std::max(worst, boneError);
            }
        }
    }
    std::cout << "worst blade error " << worstBlade / kDegree << " degrees" << std::endl;
    EXPECT_LT(worstBlade, 0.05f * kDegree);
    for (auto &[name, worst] : worstBone) {
        std::cout << "worst " << name << " direction error " << worst / kDegree << " degrees" << std::endl;
        EXPECT_LT(worst, 1.0f * kDegree) << name;
    }

    // Every frame of the GLA, not only the moves': a finite pose (pose()
    // throws otherwise), the blade on the tag, and the hand turning from frame
    // to frame exactly as the tag turns, even where the blade points along the
    // forearm.
    float worstAnyBlade = 0.0f;
    float worstTurn = 0.0f;
    std::set<int> alongForearm;
    float nearestToForearm = glm::pi<float>();
    glm::mat3 previousWeapon(1.0f);
    glm::mat3 previousTag(1.0f);
    for (int frame = 0; frame < gla->numFrames(); ++frame) {
        glm::mat3 weaponRotation(retargeter.transforms(retargeter.pose(frame))[weapon]);
        glm::mat3 tag(tagRotation(*gla, frame));
        float bladeError = angleBetween(weaponRotation[2], retargeter.bladeDirection(frame));
        ASSERT_TRUE(std::isfinite(bladeError)) << "frame " << frame;
        worstAnyBlade = std::max(worstAnyBlade, bladeError);
        if (frame > 0) {
            float turnError = std::abs(rotationAngle(previousWeapon, weaponRotation) - rotationAngle(previousTag, tag));
            ASSERT_TRUE(std::isfinite(turnError)) << "frame " << frame;
            worstTurn = std::max(worstTurn, turnError);
        }
        previousWeapon = weaponRotation;
        previousTag = tag;

        auto ja = gla->boneTransforms(frame);
        glm::vec3 forearm(ja[gla->boneIndex("rradius")][3] - ja[gla->boneIndex("rhand")][3]);
        float offLine = angleBetween(forearm, glm::vec3(ja[gla->boneIndex(JaRetargeter::kSaberTagBone)][2]));
        offLine = std::min(offLine, glm::pi<float>() - offLine);
        nearestToForearm = std::min(nearestToForearm, offLine);
        if (offLine < 15.0f * kDegree) {
            alongForearm.insert(frame);
        }
    }
    int clipsAlongForearm = 0;
    for (auto &[_, clip] : clips) {
        auto first = alongForearm.lower_bound(clip.firstFrame);
        if (first != alongForearm.end() && *first < clip.firstFrame + clip.numFrames) {
            ++clipsAlongForearm;
        }
    }
    std::cout << "all " << gla->numFrames() << " frames: worst blade error " << worstAnyBlade / kDegree
              << " degrees; worst difference between hand and tag turns from frame to frame " << worstTurn / kDegree
              << " degrees; blade within 15 degrees of the forearm's line in " << alongForearm.size() << " frames of "
              << clipsAlongForearm << " clips, nearest " << nearestToForearm / kDegree << " degrees" << std::endl;
    EXPECT_LT(worstAnyBlade, 0.05f * kDegree);
    EXPECT_LT(worstTurn, 0.01f * kDegree);

    // Through the engine: play each move on a model scene node of the body's
    // skeleton and record every node 100 times a second.
    std::function<std::shared_ptr<ModelNode>(const ModelNode &, ModelNode *)> copySkeleton =
        [&](const ModelNode &node, ModelNode *parent) {
            auto copy = std::make_shared<ModelNode>(node.number(), node.name(), node.restPosition(), node.restOrientation(), node.isAnimated(), parent);
            for (auto &child : node.children()) {
                copy->addChild(copySkeleton(*child, copy.get()));
            }
            return copy;
        };
    auto skeleton = std::make_shared<Model>(body->name(), 0, copySkeleton(*body->rootNode(), nullptr), std::vector<std::shared_ptr<Animation>>(), "", 1.0f);
    auto graphicsOpt = GraphicsOptions();
    auto pipelineFactory = MockRenderPipelineFactory();
    auto graphicsModule = TestGraphicsModule();
    graphicsModule.init();
    auto audioModule = TestAudioModule();
    audioModule.init();
    auto resourceModule = TestResourceModule();
    resourceModule.init();
    auto scene = std::make_unique<SceneGraph>("test", pipelineFactory, graphicsOpt, graphicsModule.services(), audioModule.services(), resourceModule.services());

    const char *dumpDir = std::getenv("REONE_TEST_JA_DUMP");
    for (std::string move : {"swing", "block", "overhead", "guard"}) {
        auto steps = jaNamedMove(move);
        auto animation = sequencer.animation("ja_" + move, steps, 0.25f);
        auto timeline = sequencer.timeline(steps);
        auto modelSceneNode = std::make_shared<ModelSceneNode>(
            *skeleton,
            ModelUsage::Creature,
            *scene,
            graphicsModule.services(),
            audioModule.services(),
            resourceModule.services());
        modelSceneNode->init();
        auto properties = AnimationProperties::fromFlags(AnimationFlags::fireForget);
        properties.scale = 1.0f;
        modelSceneNode->playAnimation(*animation, nullptr, properties);

        std::ofstream dump;
        if (dumpDir) {
            dump.open(std::filesystem::path(dumpDir) / (move + ".txt"));
            dump << std::setprecision(9);
        }
        const float step = 1.0f / 100.0f;
        int frames = static_cast<int>(std::floor(animation->length() / step + 1e-4f));
        float worstEngine = 0.0f;
        for (int k = 0; k <= frames; ++k) {
            if (k > 0) {
                modelSceneNode->update(step);
            } else {
                modelSceneNode->update(0.0f);
            }
            float time = std::min(k * step, animation->length());
            // Compare with the retargeter at sample times the engine hits exactly.
            float sample = time * sequencer.sampleRate();
            if (std::abs(sample - std::round(sample)) < 1e-3f) {
                auto expected = retargeter.transforms(timeline[static_cast<size_t>(std::lround(sample))]);
                for (size_t i = 0; i < retargeter.nodes().size(); ++i) {
                    auto sceneNode = modelSceneNode->getNodeByName(retargeter.nodes()[i].name);
                    float error = glm::distance(glm::vec3(expected[i][3]), glm::vec3(sceneNode->absoluteTransform()[3]));
                    ASSERT_TRUE(std::isfinite(error)) << move << " " << retargeter.nodes()[i].name << " at " << time << " s";
                    worstEngine = std::max(worstEngine, error);
                }
            }
            if (dump.is_open()) {
                for (auto &node : retargeter.nodes()) {
                    const glm::mat4 &m = modelSceneNode->getNodeByName(node.name)->absoluteTransform();
                    dump << time << " " << node.name;
                    for (int c = 0; c < 4; ++c) {
                        for (int r = 0; r < 4; ++r) {
                            dump << " " << m[c][r];
                        }
                    }
                    dump << "\n";
                }
            }
        }
        std::cout << move << ": " << animation->length() << " s, " << timeline.size() << " samples, worst engine position error "
                  << worstEngine << std::endl;
        EXPECT_LT(worstEngine, 1e-3f) << move;
    }

    // The retargeter's own pose for every frame of the swing, for comparison with other implementations.
    if (dumpDir) {
        std::ofstream frames(std::filesystem::path(dumpDir) / "frames.txt");
        frames << std::setprecision(9);
        for (auto &name : moveClips) {
            const auto &clip = sequencer.clip(name);
            for (int frame = clip.firstFrame; frame < clip.firstFrame + clip.numFrames; ++frame) {
                auto kotor = retargeter.transforms(retargeter.pose(frame));
                for (size_t i = 0; i < kotor.size(); ++i) {
                    frames << frame << " " << retargeter.nodes()[i].name;
                    for (int c = 0; c < 4; ++c) {
                        for (int r = 0; r < 4; ++r) {
                            frames << " " << kotor[i][c][r];
                        }
                    }
                    frames << "\n";
                }
            }
        }
    }
}
