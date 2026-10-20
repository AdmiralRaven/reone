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

#pragma once

#include <gtest/gtest.h>

#include "reone/game/jaretarget.h"
#include "reone/graphics/animation.h"
#include "reone/graphics/format/glareader.h"
#include "reone/graphics/ghoulanimation.h"
#include "reone/graphics/mesh.h"
#include "reone/graphics/model.h"
#include "reone/graphics/modelnode.h"
#include "reone/resource/2da.h"
#include "reone/system/stream/memoryinput.h"

#include "gla.h"

namespace reone {

namespace test {

using namespace reone::game;
using namespace reone::graphics;

// Synthetic bodies for the Jedi Academy animation tests: a small Ghoul2
// humanoid in JA's axes and a KOTOR-like body in KOTOR's.

inline glm::quat turn(float degrees, const glm::vec3 &axis) {
    return glm::angleAxis(glm::radians(degrees), glm::normalize(axis));
}

// A Ghoul2 humanoid in JA's axes (left +X, forward -Y, up +Z), T-posed, with
// the base pose scale Jedi Academy uses and a few arbitrary bone frames.
// Frame 0 is the bind pose; frames 1 and 2 move the body. Each of handTurns
// adds a frame that turns the right hand, and the saber tag with it, about
// the wrist; the forearm points from the wrist along +X.
inline GlaBuilder syntheticJaSkeleton(const std::vector<glm::quat> &handTurns = {}) {
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
    for (auto &handTurn : handTurns) {
        pose({{"rhand", GlaBuilder::turnAbout(position("rhand"), handTurn)}});
    }
    return gla;
}

inline std::shared_ptr<GhoulAnimation> loadGla(const GlaBuilder &builder) {
    auto bytes = builder.build();
    auto stream = MemoryInputStream(bytes);
    auto reader = GlaReader(stream);
    reader.load();
    return reader.animation();
}

// A KOTOR-like body in KOTOR's axes (right +X, forward +Y, up +Z), arms
// hanging at 45 degrees, with arbitrary node frames where KOTOR has them.
// weapon replaces the weapon node's model-space rotation.
inline std::shared_ptr<Model> syntheticKotorBody(bool withSkin, const std::string &omit = "", std::optional<glm::quat> weapon = std::nullopt) {
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
        glm::quat rotation(weapon && std::string(spec.name) == "rhand" ? *weapon : spec.rotation);
        glm::mat4 global(glm::translate(spec.position) * glm::mat4_cast(rotation));
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

// A mesh that is a box from \p min to \p max: only its corners, as the
// blade and body tests need only its bounds.
inline std::shared_ptr<ModelNode::TriangleMesh> boxMesh(const glm::vec3 &min, const glm::vec3 &max) {
    std::vector<Mesh::Vertex> vertices;
    for (float x : {min.x, max.x}) {
        for (float y : {min.y, max.y}) {
            for (float z : {min.z, max.z}) {
                Mesh::Vertex vertex;
                vertex.position = glm::vec3(x, y, z);
                vertices.push_back(vertex);
            }
        }
    }
    Mesh::VertexLayout layout;
    layout.stride = 3 * sizeof(float);
    layout.offPosition = 0;
    auto mesh = std::make_shared<ModelNode::TriangleMesh>();
    mesh->mesh = std::make_shared<Mesh>(std::move(vertices), layout, std::vector<Mesh::Face>());
    return mesh;
}

// A lightsaber model with one blade, a mesh 2 cm square that runs along +Z
// for 1 m, from 5 cm past the hilt's origin; with \p planes meshes on it, as
// KOTOR's single saber has two crossed planes.
inline std::shared_ptr<Model> syntheticSaber(int planes = 1) {
    const glm::quat none(1.0f, 0.0f, 0.0f, 0.0f);
    auto root = std::make_shared<ModelNode>(0, "w_test_saber", glm::vec3(0.0f), none, true);
    for (int plane = 0; plane < planes; ++plane) {
        auto blade = std::make_shared<ModelNode>(1 + plane, "blade" + std::to_string(plane), glm::vec3(0.0f, 0.0f, 0.05f), none, true, root.get());
        auto mesh = boxMesh(glm::vec3(-0.01f, -0.01f, 0.0f), glm::vec3(0.01f, 0.01f, 1.0f));
        mesh->saber = true;
        blade->setMesh(mesh);
        root->addChild(blade);
    }
    return std::make_shared<Model>("w_test_saber", MdlClassification::lightsaber, root, std::vector<std::shared_ptr<Animation>>(), "", 1.0f);
}

// A creature's model that is one box, \p width square and \p height tall,
// standing on its origin: a body with no humanoid skeleton.
// KOTOR's appearance.2da as far as Creature::loadAppearance reads it: one row
// for each size category, small, medium, large and huge (rows 0 to 3), none
// with a model to build.
inline std::shared_ptr<resource::TwoDA> sizedAppearances() {
    resource::TwoDA::Builder builder;
    builder.columns({"modeltype", "walkdist", "rundist", "perspace", "creperspace",
                     "sizecategory", "footsteptype", "envmap", "race"});
    for (const char *size : {"2", "3", "4", "5"}) {
        builder.row({"S", "1", "1", "0.6", "0.6", size, "-1", "", ""});
    }
    return builder.build();
}

inline std::shared_ptr<Model> syntheticBoxBody(float width, float height) {
    const glm::quat none(1.0f, 0.0f, 0.0f, 0.0f);
    auto root = std::make_shared<ModelNode>(0, "box_body", glm::vec3(0.0f), none, true);
    auto box = std::make_shared<ModelNode>(1, "box", glm::vec3(0.0f), none, true, root.get());
    box->setMesh(boxMesh(glm::vec3(-0.5f * width, -0.5f * width, 0.0f), glm::vec3(0.5f * width, 0.5f * width, height)));
    root->addChild(box);
    return std::make_shared<Model>("box_body", 0, root, std::vector<std::shared_ptr<Animation>>(), "", 1.0f);
}

inline std::map<std::string, GhoulAnimationClip> syntheticClips() {
    return {{"BIND", {0, 1, -1, 20.0f}},
            {"MOVE", {0, 3, -1, 10.0f}},
            {"BACK", {0, 3, -1, -10.0f}},
            {"LOOP", {1, 2, 0, 10.0f}}};
}

inline void expectSamePose(const JaRetargeter::Pose &expected, const JaRetargeter::Pose &actual, float tolerance = 1e-5f) {
    ASSERT_EQ(expected.orientations.size(), actual.orientations.size());
    for (size_t i = 0; i < expected.orientations.size(); ++i) {
        EXPECT_NEAR(1.0f, std::abs(glm::dot(expected.orientations[i], actual.orientations[i])), tolerance) << "node " << i;
    }
    EXPECT_NEAR(0.0f, glm::distance(expected.rootPosition, actual.rootPosition), tolerance);
}

} // namespace test

} // namespace reone
