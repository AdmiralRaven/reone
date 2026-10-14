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

#include "reone/graphics/ghoulanimation.h"

namespace reone {

namespace graphics {

class Animation;
class Model;

} // namespace graphics

namespace game {

/**
 * Poses a KOTOR humanoid body with Jedi Academy's animations, keeping KOTOR's
 * skeleton so heads, equipment and every body model keep working:
 *
 * - each mapped KOTOR bone takes the model-space rotation of its JA
 *   counterpart, after both bind poses are aligned along the bone;
 * - the right hand is turned so that the weapon's +Z, the blade, follows the
 *   +Z of JA's saber tag, with the forearm fixing the twist;
 * - the root dummy follows JA's pelvis, scaled by the ratio of pelvis heights.
 *
 * JA's model axes (left +X, forward -Y, up +Z) map onto KOTOR's (right +X,
 * forward +Y, up +Z) by a half turn about Z.
 */
class JaRetargeter : boost::noncopyable {
public:
    struct Node {
        std::string name;
        uint16_t number {0};
        int parent {-1}; /**< index into nodes(); -1 for the model root */
        glm::vec3 restPosition {0.0f};
        glm::quat restOrientation {1.0f, 0.0f, 0.0f, 0.0f};
        glm::mat4 bindTransform {1.0f}; /**< model space */
        bool posed {false};             /**< carried by retargeted animations */
    };

    /** A body pose: every node's local orientation, and the root dummy's local position. */
    struct Pose {
        std::vector<glm::quat> orientations;
        glm::vec3 rootPosition {0.0f};
    };

    /** Which JA bone drives a KOTOR node, and the children that give both bones their direction. */
    struct BoneMapping {
        const char *kotor;
        const char *ja;
        const char *kotorChild; /**< empty when the bone keeps KOTOR's bind direction */
        const char *jaChild;
    };

    static const std::vector<BoneMapping> &boneMappings();

    static constexpr const char *kRootNode = "rootdummy";
    static constexpr const char *kWeaponNode = "rhand";
    static constexpr const char *kWeaponHandNode = "rhand_g";
    static constexpr const char *kSaberTagBone = "rhang_tag_bone";

    /**
     * \throws std::runtime_error naming the KOTOR nodes or JA bones that are
     *         missing
     */
    JaRetargeter(const graphics::GhoulAnimation &ja, const graphics::Model &body);

    const graphics::GhoulAnimation &source() const { return _ja; }
    const std::vector<Node> &nodes() const { return _nodes; }
    int nodeIndex(const std::string &name) const;
    int rootNodeIndex() const { return _rootNode; }

    /** KOTOR units per JA unit. */
    float scale() const { return _scale; }

    /** The body pose for one JA frame. */
    Pose pose(int frame) const;

    /** Model-space transforms of all nodes in a pose. */
    std::vector<glm::mat4> transforms(const Pose &pose) const;

    /** Model-space rotation of a JA bone in a frame, in KOTOR axes, without Ghoul2's scale. */
    glm::mat3 jaRotation(const std::vector<glm::mat4> &jaTransforms, int bone) const;

    /** Position of a JA bone in a frame, in KOTOR axes and units, relative to JA's model root. */
    glm::vec3 jaPosition(const std::vector<glm::mat4> &jaTransforms, const glm::vec3 &rootTranslation, int bone) const;

    /** JA's blade direction in a frame, in KOTOR axes. */
    glm::vec3 bladeDirection(int frame) const;

    /** A factor of the way from one pose to another: slerped orientations, interpolated root. */
    static Pose blend(const Pose &from, const Pose &to, float factor);

private:
    struct Mapped {
        int node {-1};
        int bone {-1};
        glm::mat3 offset {1.0f}; /**< JA bind rotation to KOTOR bind rotation, after alignment */
    };

    const graphics::GhoulAnimation &_ja;
    std::vector<Node> _nodes;
    std::unordered_map<std::string, int> _nodeByName;
    std::vector<Mapped> _mapped;

    int _rootNode {-1};
    int _weaponNode {-1};
    int _weaponHandNode {-1};
    int _forearmNode {-1};

    int _jaRootBone {-1};
    int _jaPelvis {-1};
    int _jaSaberTag {-1};
    int _jaRadius {-1};
    int _jaHand {-1};

    float _scale {1.0f};
    glm::vec3 _jaPelvisBind {0.0f};
    glm::mat3 _weaponLocalRotation {1.0f};
    glm::vec3 _forearmInWeaponFrame {0.0f};
};

/** One step of a JA move sequence. */
struct JaSequenceStep {
    std::string clip;      /**< animation.cfg name */
    float seconds {-1.0f}; /**< how long to play; below 0 for the clip's own length */
    float blendIn {0.0f};  /**< cross-fade from the previous step's last pose, in seconds */
};

static constexpr const char *kJaFastStance = "BOTH_SABERFAST_STANCE"; /**< the fast style's ready stance */
static constexpr const char *kJaMediumStance = "BOTH_STAND2";         /**< the medium style's */

/**
 * The proof's named moves, as JA clip sequences; empty for any other name.
 *
 * - swing: fast style, forward and right: ready, start, attack from top left
 *   to bottom right, return, ready;
 * - block: fast style parry, upper right, held, then straight back to ready;
 * - overhead: medium style, top to bottom;
 * - guard: the manual guard pose, high;
 * - stance: the fast ready stance.
 *
 * Blends follow JA's saber move table: 100 ms into start, attack and return
 * clips, 50 ms into a parry, 350 ms into the ready stance. Holds, and going
 * from a parry straight back to ready, are the proof's choice, not traced
 * runtime behaviour.
 */
std::vector<JaSequenceStep> jaNamedMove(const std::string &name);

/**
 * Turns sequences of JA clips into KOTOR animations for one body: samples the
 * clips at a fixed rate, cross-fades between steps, and keys every posed node.
 */
class JaSequencer : boost::noncopyable {
public:
    JaSequencer(
        const JaRetargeter &retargeter,
        std::map<std::string, graphics::GhoulAnimationClip> clips,
        float sampleRate = 60.0f);

    bool hasClip(const std::string &name) const;
    const graphics::GhoulAnimationClip &clip(const std::string &name) const;

    /** The clip's own length in seconds: its frames, less one, over its rate. */
    float clipLength(const std::string &name) const;

    /** The body pose a clip shows at a time, between its frames as needed. Looping clips wrap. */
    JaRetargeter::Pose sample(const std::string &name, float time) const;

    /** Poses at the sample rate for a whole sequence. */
    std::vector<JaRetargeter::Pose> timeline(const std::vector<JaSequenceStep> &steps) const;

    /**
     * A KOTOR animation of the sequence, named \p name, with orientation keys
     * for every posed node and position keys for the root dummy.
     */
    std::shared_ptr<graphics::Animation> animation(
        std::string name,
        const std::vector<JaSequenceStep> &steps,
        float transitionTime) const;

    float sampleRate() const { return _sampleRate; }

private:
    const JaRetargeter &_retargeter;
    std::map<std::string, graphics::GhoulAnimationClip> _clips;
    float _sampleRate;

    mutable std::unordered_map<int, JaRetargeter::Pose> _framePoses;

    const JaRetargeter::Pose &framePose(int frame) const;
};

} // namespace game

} // namespace reone
