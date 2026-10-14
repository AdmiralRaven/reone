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

float angleBetween(const glm::vec3 &a, const glm::vec3 &b) {
    float c = glm::dot(glm::normalize(a), glm::normalize(b));
    return std::acos(glm::clamp(c, -1.0f, 1.0f));
}

glm::vec3 toKotorAxes(const glm::vec3 &v) {
    return glm::vec3(-v.x, -v.y, v.z);
}

glm::quat turn(float degrees, const glm::vec3 &axis) {
    return glm::angleAxis(degrees * kDegree, glm::normalize(axis));
}

// A Ghoul2 humanoid in JA's axes (left +X, forward -Y, up +Z), T-posed, with
// the base pose scale Jedi Academy uses and a few arbitrary bone frames.
// Frame 0 is the bind pose; frames 1 and 2 move the body.
GlaBuilder syntheticJaSkeleton() {
    struct Spec {
        const char *name;
        const char *parent;
        glm::vec3 position;
        glm::quat frame;
    };
    const glm::quat none(1.0f, 0.0f, 0.0f, 0.0f);
    const std::vector<Spec> specs {
        {"model_root", "", {0.0f, 0.0f, 0.0f}, none},
        {"pelvis", "model_root", {0.0f, 0.0f, 36.0f}, turn(90.0f, {0, 0, 1})},
        {"lfemurYZ", "pelvis", {4.0f, 0.0f, 34.0f}, turn(180.0f, {1, 0, 0})},
        {"ltibia", "lfemurYZ", {4.0f, 0.0f, 18.0f}, none},
        {"ltalus", "ltibia", {4.0f, 0.0f, 2.0f}, none},
        {"rfemurYZ", "pelvis", {-4.0f, 0.0f, 34.0f}, turn(180.0f, {1, 0, 0})},
        {"rtibia", "rfemurYZ", {-4.0f, 0.0f, 18.0f}, none},
        {"rtalus", "rtibia", {-4.0f, 0.0f, 2.0f}, none},
        {"upper_lumbar", "pelvis", {0.0f, 0.0f, 40.0f}, none},
        {"thoracic", "upper_lumbar", {0.0f, 0.0f, 50.0f}, turn(-20.0f, {1, 0, 0})},
        {"cervical", "thoracic", {0.0f, 0.0f, 58.0f}, none},
        {"cranium", "cervical", {0.0f, 0.0f, 62.0f}, none},
        {"lclavical", "thoracic", {2.0f, 0.0f, 56.0f}, none},
        {"lhumerus", "lclavical", {7.0f, 0.0f, 56.0f}, turn(-90.0f, {0, 1, 0})},
        {"lradius", "lhumerus", {17.0f, 0.0f, 56.0f}, none},
        {"lhand", "lradius", {26.0f, 0.0f, 56.0f}, none},
        {"rclavical", "thoracic", {-2.0f, 0.0f, 56.0f}, none},
        {"rhumerus", "rclavical", {-7.0f, 0.0f, 56.0f}, turn(90.0f, {0, 1, 0})},
        {"rradius", "rhumerus", {-17.0f, 0.0f, 56.0f}, turn(30.0f, {1, 0, 0})},
        {"rhand", "rradius", {-26.0f, 0.0f, 56.0f}, none},
        {"rhang_tag_bone", "rhand", {-28.0f, 0.0f, 56.0f}, turn(90.0f, {1, 0, 0})}, // +Z points forward, -Y
    };
    GlaBuilder gla;
    gla.scale = 0.64f;
    std::map<std::string, int> index;
    for (auto &spec : specs) {
        glm::mat4 base(glm::translate(spec.position) * glm::mat4_cast(spec.frame) * glm::scale(glm::vec3(0.64f)));
        int parent = *spec.parent ? index.at(spec.parent) : -1;
        index[spec.name] = static_cast<int>(gla.bones.size());
        gla.bones.push_back({spec.name, parent, base});
    }
    auto position = [&](const char *name) {
        for (auto &spec : specs) {
            if (std::string(spec.name) == name) {
                return spec.position;
            }
        }
        throw std::logic_error(name);
    };
    gla.pool.push_back(GlaBuilder::entry(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.0f)));
    auto pose = [&](const std::map<std::string, GlaBuilder::Entry> &moves) {
        std::vector<uint32_t> frame(specs.size(), 0);
        for (auto &[name, entry] : moves) {
            frame[index.at(name)] = static_cast<uint32_t>(gla.pool.size());
            gla.pool.push_back(entry);
        }
        gla.frames.push_back(frame);
    };
    pose({});
    pose({{"model_root", GlaBuilder::entry(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), {0.0f, 0.0f, -24.0f})},
          {"pelvis", GlaBuilder::turnAbout(position("pelvis"), turn(10.0f, {0, 0, 1}), {0.0f, 0.0f, -3.0f})},
          {"rhumerus", GlaBuilder::turnAbout(position("rhumerus"), turn(70.0f, {0, 1, 0.3f}))},
          {"rradius", GlaBuilder::turnAbout(position("rradius"), turn(40.0f, {0, 0, 1}))},
          {"rhand", GlaBuilder::turnAbout(position("rhand"), turn(25.0f, {1, 0, 0}))},
          {"lfemurYZ", GlaBuilder::turnAbout(position("lfemurYZ"), turn(30.0f, {1, 0, 0}))},
          {"cervical", GlaBuilder::turnAbout(position("cervical"), turn(15.0f, {0, 1, 0}))}});
    pose({{"rhumerus", GlaBuilder::turnAbout(position("rhumerus"), turn(-50.0f, {0, 0, 1}))},
          {"rradius", GlaBuilder::turnAbout(position("rradius"), turn(-60.0f, {0, 1, 0}))},
          {"rhang_tag_bone", GlaBuilder::turnAbout(position("rhang_tag_bone"), turn(15.0f, {0, 1, 0}))},
          {"upper_lumbar", GlaBuilder::turnAbout(position("upper_lumbar"), turn(20.0f, {1, 0, 0}))},
          {"rtibia", GlaBuilder::turnAbout(position("rtibia"), turn(-35.0f, {1, 0, 0}))}});
    return gla;
}

std::shared_ptr<GhoulAnimation> loadGla(const GlaBuilder &builder) {
    auto bytes = builder.build();
    auto stream = MemoryInputStream(bytes);
    auto reader = GlaReader(stream);
    reader.load();
    return reader.animation();
}

// A KOTOR-like body in KOTOR's axes (right +X, forward +Y, up +Z), arms
// hanging at 45 degrees, with arbitrary node frames where KOTOR has them.
std::shared_ptr<Model> syntheticKotorBody(bool withSkin, const std::string &omit = "") {
    struct Spec {
        const char *name;
        const char *parent;
        glm::vec3 position;
        glm::quat rotation;
        bool skin;
    };
    const glm::quat none(1.0f, 0.0f, 0.0f, 0.0f);
    const std::vector<Spec> specs {
        {"synthetic_body", "", {0.0f, 0.0f, 0.0f}, none, false},
        {"rootdummy", "synthetic_body", {0.0f, 0.0f, 1.0f}, none, false},
        {"pelvis_g", "rootdummy", {0.0f, 0.0f, 1.0f}, turn(5.0f, {1, 0, 0}), false},
        {"lthigh_g", "pelvis_g", {-0.1f, 0.0f, 0.95f}, turn(180.0f, {0, 1, 0}), false},
        {"lshin_g", "lthigh_g", {-0.1f, 0.02f, 0.5f}, none, false},
        {"lfoot_g", "lshin_g", {-0.1f, 0.0f, 0.06f}, turn(-30.0f, {1, 0, 0}), false},
        {"rthigh_g", "pelvis_g", {0.1f, 0.0f, 0.95f}, turn(180.0f, {0, 1, 0}), false},
        {"rshin_g", "rthigh_g", {0.1f, 0.02f, 0.5f}, none, false},
        {"rfoot_g", "rshin_g", {0.1f, 0.0f, 0.06f}, turn(-30.0f, {1, 0, 0}), false},
        {"torso_g", "rootdummy", {0.0f, 0.0f, 1.1f}, none, false},
        {"torso_skin", "torso_g", {0.0f, 0.0f, 1.2f}, none, true},
        {"torsoupr_g", "torso_g", {0.0f, 0.02f, 1.4f}, turn(10.0f, {0, 0, 1}), false},
        {"headhook", "torsoupr_g", {0.0f, 0.0f, 1.62f}, none, false},
        {"lcollar_g", "torsoupr_g", {-0.05f, 0.0f, 1.55f}, none, false},
        {"lbicep_g", "lcollar_g", {-0.2f, 0.0f, 1.55f}, turn(-60.0f, {0, 1, 0}), false},
        {"lforearm_g", "lbicep_g", {-0.4f, 0.0f, 1.35f}, none, false},
        {"lhand_g", "lforearm_g", {-0.58f, 0.0f, 1.17f}, none, false},
        {"rcollar_g", "torsoupr_g", {0.05f, 0.0f, 1.55f}, none, false},
        {"rbicep_g", "rcollar_g", {0.2f, 0.0f, 1.55f}, turn(30.0f, {0, 1, 0}), false},
        {"rforearm_g", "rbicep_g", {0.4f, 0.0f, 1.35f}, turn(-15.0f, {0, 0, 1}), false},
        {"rhand_g", "rforearm_g", {0.58f, 0.0f, 1.17f}, none, false},
        {"rhand", "rhand_g", {0.62f, 0.0f, 1.13f}, turn(-90.0f, {0, 1, 0}) * turn(20.0f, {1, 0, 0}), false},
        {"rfinger1", "rhand_g", {0.65f, 0.0f, 1.12f}, turn(12.0f, {0, 1, 0}), false},
    };
    std::map<std::string, std::shared_ptr<ModelNode>> nodes;
    std::map<std::string, glm::mat4> globals;
    std::map<std::string, std::string> parentOf; // an omitted node's children go to its parent
    std::shared_ptr<ModelNode> root;
    uint16_t number = 0;
    for (auto &spec : specs) {
        std::string parentName(spec.parent);
        while (parentOf.count(parentName)) {
            parentName = parentOf.at(parentName);
        }
        if ((spec.skin && !withSkin) || omit == spec.name) {
            parentOf[spec.name] = parentName;
            continue;
        }
        glm::mat4 global(glm::translate(spec.position) * glm::mat4_cast(spec.rotation));
        glm::mat4 local(global);
        ModelNode *parent = nullptr;
        if (!parentName.empty()) {
            local = glm::inverse(globals.at(parentName)) * global;
            parent = nodes.at(parentName).get();
        }
        auto node = std::make_shared<ModelNode>(
            number++,
            spec.name,
            glm::vec3(local[3]),
            glm::normalize(glm::quat_cast(glm::mat3(local))),
            true,
            parent);
        if (spec.skin) {
            auto mesh = std::make_shared<ModelNode::TriangleMesh>();
            mesh->skin = std::make_shared<ModelNode::Skin>();
            node->setMesh(mesh);
        }
        if (parent) {
            nodes.at(parentName)->addChild(node);
        } else {
            root = node;
        }
        nodes[spec.name] = node;
        globals[spec.name] = global;
    }
    return std::make_shared<Model>("synthetic_body", 0, root, std::vector<std::shared_ptr<Animation>>(), "", 1.0f);
}

std::map<std::string, GhoulAnimationClip> syntheticClips() {
    return {{"BIND", {0, 1, -1, 20.0f}},
            {"MOVE", {0, 3, -1, 10.0f}},
            {"BACK", {0, 3, -1, -10.0f}},
            {"LOOP", {1, 2, 0, 10.0f}}};
}

void expectSamePose(const JaRetargeter::Pose &expected, const JaRetargeter::Pose &actual, float tolerance = 1e-5f) {
    ASSERT_EQ(expected.orientations.size(), actual.orientations.size());
    for (size_t i = 0; i < expected.orientations.size(); ++i) {
        EXPECT_NEAR(1.0f, std::abs(glm::dot(expected.orientations[i], actual.orientations[i])), tolerance) << "node " << i;
    }
    EXPECT_NEAR(0.0f, glm::distance(expected.rootPosition, actual.rootPosition), tolerance);
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
    for (auto &name : moveClips) {
        const auto &clip = sequencer.clip(name);
        for (int frame = clip.firstFrame; frame < clip.firstFrame + clip.numFrames; ++frame) {
            auto kotor = retargeter.transforms(retargeter.pose(frame));
            auto ja = gla->boneTransforms(frame);
            worstBlade = std::max(worstBlade, angleBetween(glm::vec3(kotor[weapon][2]), retargeter.bladeDirection(frame)));
            for (auto &mapping : JaRetargeter::boneMappings()) {
                if (!*mapping.kotorChild) {
                    continue;
                }
                glm::vec3 kotorDirection(kotor[retargeter.nodeIndex(mapping.kotorChild)][3] - kotor[retargeter.nodeIndex(mapping.kotor)][3]);
                glm::vec3 jaDirection(toKotorAxes(glm::vec3(ja[gla->boneIndex(mapping.jaChild)][3] - ja[gla->boneIndex(mapping.ja)][3])));
                float &worst = worstBone[mapping.kotor];
                worst = std::max(worst, angleBetween(kotorDirection, jaDirection));
            }
        }
    }
    std::cout << "worst blade error " << worstBlade / kDegree << " degrees" << std::endl;
    EXPECT_LT(worstBlade, 0.05f * kDegree);
    for (auto &[name, worst] : worstBone) {
        std::cout << "worst " << name << " direction error " << worst / kDegree << " degrees" << std::endl;
        EXPECT_LT(worst, 1.0f * kDegree) << name;
    }

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
                    worstEngine = std::max(worstEngine, glm::distance(glm::vec3(expected[i][3]), glm::vec3(sceneNode->absoluteTransform()[3])));
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
