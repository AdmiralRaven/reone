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

#include "reone/game/jaretarget.h"

#include "reone/graphics/animation.h"
#include "reone/graphics/model.h"
#include "reone/graphics/modelnode.h"
#include "reone/graphics/types.h"

using namespace reone::graphics;

namespace reone {

namespace game {

// In the bind pose the forearm sets the weapon's twist about the blade only
// when it lies at least 15 degrees off the blade's line.
static constexpr float kMinForearmSine = 0.258819045f;

// A saber tag's +Y and +Z are perpendicular; nearer than 30 degrees to
// parallel is broken data.
static constexpr float kMinTagSine = 0.5f;

static bool isFinite(const glm::vec3 &v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

static bool isFinite(const glm::quat &q) {
    return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z);
}

static bool isFinite(const glm::mat3 &m) {
    return isFinite(m[0]) && isFinite(m[1]) && isFinite(m[2]);
}

// A half turn about Z: JA's model axes to KOTOR's. It is its own inverse.
static glm::vec3 toKotorAxes(const glm::vec3 &v) {
    return glm::vec3(-v.x, -v.y, v.z);
}

static glm::mat3 toKotorAxes(const glm::mat3 &m) {
    glm::mat3 a(1.0f);
    a[0][0] = -1.0f;
    a[1][1] = -1.0f;
    return a * m * a;
}

// The rotation nearest to a matrix: its orthogonal polar factor, by Newton
// iteration. Ghoul2's matrices carry the file's scale and quantization noise.
static glm::mat3 nearestRotation(glm::mat3 m) {
    for (int i = 0; i < 32; ++i) {
        glm::mat3 next(0.5f * (m + glm::transpose(glm::inverse(m))));
        float change = 0.0f;
        for (int c = 0; c < 3; ++c) {
            for (int r = 0; r < 3; ++r) {
                change = std::max(change, std::abs(next[c][r] - m[c][r]));
            }
        }
        m = next;
        if (change < 1e-7f) {
            break;
        }
    }
    return m;
}

// The smallest rotation taking direction a onto direction b.
static glm::mat3 rotationBetween(glm::vec3 a, glm::vec3 b) {
    a = glm::normalize(a);
    b = glm::normalize(b);
    glm::vec3 v(glm::cross(a, b));
    float c = glm::dot(a, b);
    if (glm::length(v) < 1e-6f) {
        if (c > 0.0f) {
            return glm::mat3(1.0f);
        }
        // Opposite directions: a half turn about any axis across them.
        glm::vec3 axis(glm::cross(a, std::abs(a.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f)));
        return glm::mat3_cast(glm::angleAxis(glm::pi<float>(), glm::normalize(axis)));
    }
    glm::mat3 skew(0.0f);
    skew[1][0] = -v.z;
    skew[2][0] = v.y;
    skew[0][1] = v.z;
    skew[2][1] = -v.x;
    skew[0][2] = -v.y;
    skew[1][2] = v.x;
    return glm::mat3(1.0f) + skew + skew * skew * (1.0f / (1.0f + c));
}

// The frame with z exactly along z, and y as near as possible to y; nothing
// when the two are nearer to parallel than asin(minSine), which leaves the
// turn about z undecided, or when either is zero or not finite.
static std::optional<glm::mat3> frameOf(const glm::vec3 &z, const glm::vec3 &y, float minSine) {
    glm::vec3 x(glm::cross(y, z));
    float sine = glm::length(x) / (glm::length(y) * glm::length(z));
    if (!(sine >= minSine)) {
        return std::nullopt;
    }
    glm::vec3 zAxis(glm::normalize(z));
    glm::vec3 xAxis(glm::normalize(x));
    return glm::mat3(xAxis, glm::cross(zAxis, xAxis), zAxis);
}

const std::vector<JaRetargeter::BoneMapping> &JaRetargeter::boneMappings() {
    static const std::vector<BoneMapping> mappings {
        {"pelvis_g", "pelvis", "", ""},
        {"torso_g", "upper_lumbar", "torsoupr_g", "thoracic"},
        {"torsoupr_g", "thoracic", "headhook", "cervical"},
        {"headhook", "cranium", "", ""},
        {"lthigh_g", "lfemuryz", "lshin_g", "ltibia"},
        {"lshin_g", "ltibia", "lfoot_g", "ltalus"},
        {"lfoot_g", "ltalus", "", ""},
        {"rthigh_g", "rfemuryz", "rshin_g", "rtibia"},
        {"rshin_g", "rtibia", "rfoot_g", "rtalus"},
        {"rfoot_g", "rtalus", "", ""},
        {"lcollar_g", "lclavical", "", ""},
        {"lbicep_g", "lhumerus", "lforearm_g", "lradius"},
        {"lforearm_g", "lradius", "lhand_g", "lhand"},
        {"lhand_g", "lhand", "", ""},
        {"rcollar_g", "rclavical", "", ""},
        {"rbicep_g", "rhumerus", "rforearm_g", "rradius"},
        {"rforearm_g", "rradius", "rhand_g", "rhand"},
        {"rhand_g", "rhand", "", ""}};
    return mappings;
}

JaRetargeter::JaRetargeter(const GhoulAnimation &ja, const Model &body) :
    _ja(ja) {

    // KOTOR nodes, depth first, parents before children
    std::function<void(const ModelNode &, int, bool)> collect = [&](const ModelNode &modelNode, int parent, bool underRoot) {
        Node node;
        node.name = modelNode.name();
        node.number = modelNode.number();
        node.parent = parent;
        node.restPosition = modelNode.restPosition();
        node.restOrientation = modelNode.restOrientation();
        node.bindTransform = modelNode.absoluteTransform();
        underRoot = underRoot || node.name == kRootNode;
        node.posed = underRoot && !modelNode.isSkinMesh();
        int index = static_cast<int>(_nodes.size());
        _nodeByName.insert({node.name, index});
        _nodes.push_back(std::move(node));
        for (auto &child : modelNode.children()) {
            collect(*child, index, underRoot);
        }
    };
    if (!body.rootNode()) {
        throw std::runtime_error("Body model has no nodes: " + body.name());
    }
    collect(*body.rootNode(), -1, false);

    std::vector<std::string> missing;
    auto requireNode = [&](const std::string &name) {
        int index = nodeIndex(name);
        if (index < 0) {
            missing.push_back(name);
        }
        return index;
    };
    auto requireBone = [&](const std::string &name) {
        int index = _ja.boneIndex(name);
        if (index < 0) {
            missing.push_back("JA " + name);
        }
        return index;
    };
    _rootNode = requireNode(kRootNode);
    _weaponNode = requireNode(kWeaponNode);
    _weaponHandNode = requireNode(kWeaponHandNode);
    int forearmNode = requireNode("rforearm_g");
    _jaPelvis = requireBone("pelvis");
    _jaSaberTag = requireBone(kSaberTagBone);
    int jaRadius = requireBone("rradius");
    int jaHand = requireBone("rhand");
    for (auto &mapping : boneMappings()) {
        requireNode(mapping.kotor);
        requireBone(mapping.ja);
        if (*mapping.kotorChild) {
            requireNode(mapping.kotorChild);
            requireBone(mapping.jaChild);
        }
    }
    if (!missing.empty()) {
        throw std::runtime_error(str(boost::format("Body model %s cannot take JA animations; missing: %s") %
                                     body.name() % boost::join(missing, ", ")));
    }
    if (_nodes[_weaponNode].parent != _weaponHandNode) {
        throw std::runtime_error(str(boost::format("Body model %s: %s is not a child of %s") %
                                     body.name() % kWeaponNode % kWeaponHandNode));
    }
    for (int bone = 0; bone < _ja.numBones(); ++bone) {
        if (_ja.bones()[bone].parent < 0) {
            _jaRootBone = bone;
            break;
        }
    }

    // KOTOR units per JA unit, from the height of the pelvis
    float jaPelvisHeight = _ja.bones()[_jaPelvis].basePose[3].z;
    float kotorPelvisHeight = _nodes[_rootNode].bindTransform[3].z;
    if (!(std::abs(jaPelvisHeight) >= 1e-4f) || !(kotorPelvisHeight > 0.0f) || !std::isfinite(kotorPelvisHeight)) {
        throw std::runtime_error("Cannot scale JA animations: a pelvis height is zero or not a number");
    }
    _scale = kotorPelvisHeight / jaPelvisHeight;

    // Bind poses, then each mapped bone's offset from its JA counterpart
    std::vector<glm::mat4> jaBind;
    jaBind.reserve(_ja.numBones());
    for (auto &bone : _ja.bones()) {
        jaBind.push_back(bone.basePose);
    }
    glm::vec3 noRoot(0.0f);
    for (auto &mapping : boneMappings()) {
        Mapped mapped;
        mapped.node = nodeIndex(mapping.kotor);
        mapped.bone = _ja.boneIndex(mapping.ja);
        glm::mat3 kotorBind(_nodes[mapped.node].bindTransform);
        if (*mapping.kotorChild) {
            glm::vec3 kotorDirection(_nodes[nodeIndex(mapping.kotorChild)].bindTransform[3] - _nodes[mapped.node].bindTransform[3]);
            glm::vec3 jaDirection(jaPosition(jaBind, noRoot, _ja.boneIndex(mapping.jaChild)) - jaPosition(jaBind, noRoot, mapped.bone));
            kotorBind = rotationBetween(kotorDirection, jaDirection) * kotorBind;
        }
        mapped.offset = glm::transpose(jaRotation(jaBind, mapped.bone)) * kotorBind;
        if (!isFinite(mapped.offset)) {
            throw std::runtime_error(str(boost::format("JA bone %s cannot drive %s: a base pose has no usable rotation") %
                                         _ja.bones()[mapped.bone].name % mapping.kotor));
        }
        _mapped.push_back(mapped);
    }
    _jaPelvisBind = jaPosition(jaBind, noRoot, _jaPelvis);
    if (!isFinite(_jaPelvisBind)) {
        throw std::runtime_error("JA bone pelvis has no usable base pose");
    }

    // The weapon hand turns with the saber tag. Only its twist about the
    // blade needs choosing, once, in the bind pose: there the forearm keeps
    // the angle around the blade it has in KOTOR's bind pose. The forearm is
    // not consulted again, because when a frame points the blade along it, it
    // says nothing about the twist.
    _weaponLocalRotation = glm::mat3_cast(_nodes[_weaponNode].restOrientation);
    glm::mat3 weaponBind(nearestRotation(glm::mat3(_nodes[_weaponNode].bindTransform)));
    glm::mat3 tagBind(saberTagFrame(jaBind, -1));
    glm::vec3 kotorForearm(_nodes[forearmNode].bindTransform[3] - _nodes[_weaponHandNode].bindTransform[3]);
    glm::vec3 jaForearm(jaPosition(jaBind, noRoot, jaRadius) - jaPosition(jaBind, noRoot, jaHand));
    auto weaponAndForearm = frameOf(glm::vec3(0.0f, 0.0f, 1.0f), glm::transpose(weaponBind) * kotorForearm, kMinForearmSine);
    auto bladeAndForearm = frameOf(tagBind[2], jaForearm, kMinForearmSine);
    glm::mat3 weaponAtBind;
    if (weaponAndForearm && bladeAndForearm) {
        weaponAtBind = *bladeAndForearm * glm::transpose(*weaponAndForearm);
    } else {
        // A forearm along the blade sets nothing: turn KOTOR's bind weapon the
        // shortest way onto the blade instead.
        weaponAtBind = rotationBetween(weaponBind[2], tagBind[2]) * weaponBind;
    }
    // Both frames have +Z on the blade, so they differ by a turn about it.
    glm::mat3 twist(glm::transpose(tagBind) * weaponAtBind);
    float angle = std::atan2(twist[0][1], twist[0][0]);
    if (!std::isfinite(angle)) {
        throw std::runtime_error("Cannot place the weapon: the right arm's bind poses are not usable");
    }
    float c = std::cos(angle);
    float s = std::sin(angle);
    _weaponCalibration = glm::mat3(c, s, 0.0f, -s, c, 0.0f, 0.0f, 0.0f, 1.0f);
}

glm::mat3 JaRetargeter::saberTagFrame(const std::vector<glm::mat4> &jaTransforms, int frame) const {
    glm::mat3 tag(toKotorAxes(glm::mat3(jaTransforms[_jaSaberTag])));
    auto result = frameOf(tag[2], tag[1], kMinTagSine);
    if (!result) {
        throw std::runtime_error(frame < 0
                                     ? std::string("JA bone ") + kSaberTagBone + " has no usable rotation in its base pose"
                                     : str(boost::format("JA frame %d: %s has no usable rotation") % frame % kSaberTagBone));
    }
    return *result;
}

int JaRetargeter::nodeIndex(const std::string &name) const {
    auto it = _nodeByName.find(name);
    return it != _nodeByName.end() ? it->second : -1;
}

glm::mat3 JaRetargeter::jaRotation(const std::vector<glm::mat4> &jaTransforms, int bone) const {
    return nearestRotation(toKotorAxes(glm::mat3(jaTransforms[bone])));
}

glm::vec3 JaRetargeter::jaPosition(const std::vector<glm::mat4> &jaTransforms, const glm::vec3 &rootTranslation, int bone) const {
    return toKotorAxes(glm::vec3(jaTransforms[bone][3]) - rootTranslation) * _scale;
}

glm::vec3 JaRetargeter::bladeDirection(int frame) const {
    auto transforms = _ja.boneTransforms(frame);
    return glm::normalize(toKotorAxes(glm::vec3(transforms[_jaSaberTag][2])));
}

JaRetargeter::Pose JaRetargeter::pose(int frame) const {
    auto ja = _ja.boneTransforms(frame);
    glm::vec3 rootTranslation(0.0f);
    if (_jaRootBone >= 0) {
        // The model root carries the player's origin offset, not body motion.
        rootTranslation = glm::vec3(_ja.boneDelta(frame, _jaRootBone)[3]);
    }

    // Model-space rotations wanted for the mapped nodes
    std::vector<std::optional<glm::mat3>> wanted(_nodes.size());
    for (auto &mapped : _mapped) {
        wanted[mapped.node] = jaRotation(ja, mapped.bone) * mapped.offset;
    }
    // The weapon hand turns with the saber tag: weapon +Z exactly along the
    // tag's +Z, the blade, and the twist calibrated in the bind pose.
    glm::mat3 weapon(saberTagFrame(ja, frame) * _weaponCalibration);
    wanted[_weaponHandNode] = weapon * glm::transpose(_weaponLocalRotation);

    Pose pose;
    pose.orientations.resize(_nodes.size());
    std::vector<glm::mat3> rotations(_nodes.size());
    std::vector<glm::vec3> positions(_nodes.size());
    for (size_t i = 0; i < _nodes.size(); ++i) {
        const Node &node = _nodes[i];
        glm::mat3 restRotation(glm::mat3_cast(node.restOrientation));
        if (node.parent < 0) {
            rotations[i] = restRotation;
            positions[i] = node.restPosition;
            pose.orientations[i] = node.restOrientation;
            continue;
        }
        const glm::mat3 &parentRotation = rotations[node.parent];
        const glm::vec3 &parentPosition = positions[node.parent];
        positions[i] = parentRotation * node.restPosition + parentPosition;
        if (static_cast<int>(i) == _rootNode) {
            positions[i] = glm::vec3(node.bindTransform[3]) + (jaPosition(ja, rootTranslation, _jaPelvis) - _jaPelvisBind);
            pose.rootPosition = glm::transpose(parentRotation) * (positions[i] - parentPosition);
        }
        if (wanted[i]) {
            rotations[i] = *wanted[i];
            pose.orientations[i] = glm::normalize(glm::quat_cast(glm::transpose(parentRotation) * rotations[i]));
        } else {
            rotations[i] = parentRotation * restRotation;
            pose.orientations[i] = node.restOrientation;
        }
    }

    // Nothing that is not a number may reach an animation's keys.
    for (size_t i = 0; i < _nodes.size(); ++i) {
        if (!isFinite(pose.orientations[i])) {
            throw std::runtime_error(str(boost::format("JA frame %d gives %s no finite rotation") % frame % _nodes[i].name));
        }
    }
    if (!isFinite(pose.rootPosition)) {
        throw std::runtime_error(str(boost::format("JA frame %d gives %s no finite position") % frame % kRootNode));
    }
    return pose;
}

std::vector<glm::mat4> JaRetargeter::transforms(const Pose &pose) const {
    std::vector<glm::mat4> result(_nodes.size());
    for (size_t i = 0; i < _nodes.size(); ++i) {
        const Node &node = _nodes[i];
        glm::vec3 position(static_cast<int>(i) == _rootNode ? pose.rootPosition : node.restPosition);
        glm::mat4 local(glm::translate(position) * glm::mat4_cast(pose.orientations[i]));
        result[i] = node.parent < 0 ? local : result[node.parent] * local;
    }
    return result;
}

JaRetargeter::Pose JaRetargeter::blend(const Pose &from, const Pose &to, float factor) {
    Pose result;
    result.orientations.resize(from.orientations.size());
    for (size_t i = 0; i < from.orientations.size(); ++i) {
        result.orientations[i] = glm::slerp(from.orientations[i], to.orientations[i], factor);
    }
    result.rootPosition = glm::mix(from.rootPosition, to.rootPosition, factor);
    return result;
}

std::vector<JaSequenceStep> jaNamedMove(const std::string &name) {
    if (name == "swing") {
        return {{kJaFastStance, 0.6f, 0.0f},
                {"BOTH_S1_S1_TL", -1.0f, 0.1f},
                {"BOTH_A1_TL_BR", -1.0f, 0.1f},
                {"BOTH_R1_BR_S1", -1.0f, 0.1f},
                {kJaFastStance, 0.6f, 0.35f}};
    }
    if (name == "block") {
        return {{kJaFastStance, 0.6f, 0.0f},
                {"BOTH_P1_S1_TR", 0.5f, 0.05f},
                {kJaFastStance, 0.6f, 0.35f}};
    }
    if (name == "overhead") {
        return {{kJaMediumStance, 0.6f, 0.0f},
                {"BOTH_S2_S1_T_", -1.0f, 0.1f},
                {"BOTH_A2_T__B_", -1.0f, 0.1f},
                {"BOTH_R2_B__S1", -1.0f, 0.1f},
                {kJaMediumStance, 0.6f, 0.35f}};
    }
    if (name == "guard") {
        return {{kJaFastStance, 0.6f, 0.0f},
                {"BOTH_P1_S1_T_", 0.7f, 0.05f},
                {kJaFastStance, 0.6f, 0.35f}};
    }
    if (name == "stance") {
        return {{kJaFastStance, 2.0f, 0.0f}};
    }
    return {};
}

JaSequencer::JaSequencer(
    const JaRetargeter &retargeter,
    std::map<std::string, GhoulAnimationClip> clips,
    float sampleRate) :
    _retargeter(retargeter),
    _clips(std::move(clips)),
    _sampleRate(sampleRate) {
    if (_sampleRate <= 0.0f) {
        throw std::invalid_argument("Sample rate must be positive");
    }
}

bool JaSequencer::hasClip(const std::string &name) const {
    return _clips.count(boost::to_upper_copy(name)) != 0;
}

const GhoulAnimationClip &JaSequencer::clip(const std::string &name) const {
    auto it = _clips.find(boost::to_upper_copy(name));
    if (it == _clips.end()) {
        throw std::runtime_error("No JA animation named " + name);
    }
    const GhoulAnimationClip &clip = it->second;
    if (clip.firstFrame < 0 || clip.firstFrame + clip.numFrames > _retargeter.source().numFrames()) {
        throw std::runtime_error("JA animation " + name + " lies outside the GLA's frames");
    }
    return clip;
}

static float clipRate(const GhoulAnimationClip &clip) {
    return clip.fps != 0.0f ? std::abs(clip.fps) : 20.0f;
}

float JaSequencer::clipLength(const std::string &name) const {
    const GhoulAnimationClip &c = clip(name);
    return static_cast<float>(c.numFrames - 1) / clipRate(c);
}

const JaRetargeter::Pose &JaSequencer::framePose(int frame) const {
    auto it = _framePoses.find(frame);
    if (it == _framePoses.end()) {
        it = _framePoses.insert({frame, _retargeter.pose(frame)}).first;
    }
    return it->second;
}

JaRetargeter::Pose JaSequencer::sample(const std::string &name, float time) const {
    const GhoulAnimationClip &c = clip(name);
    int count = c.numFrames;
    float position = std::max(0.0f, time) * clipRate(c);
    // Within rounding of a frame, as at a clip's end, take that frame.
    float nearest = std::round(position);
    if (std::abs(position - nearest) < 1e-4f) {
        position = nearest;
    }
    int first, second;
    float factor;
    if (c.loopFrames >= 0 && count > 1) {
        position = std::fmod(position, static_cast<float>(count));
        first = std::min(static_cast<int>(position), count - 1);
        second = (first + 1) % count;
    } else {
        position = std::min(position, static_cast<float>(count - 1));
        first = static_cast<int>(position);
        second = std::min(first + 1, count - 1);
    }
    factor = position - static_cast<float>(first);
    // A negative rate plays the stored frames from last to first.
    auto stored = [&](int frame) {
        return c.firstFrame + (c.fps < 0.0f ? count - 1 - frame : frame);
    };
    const JaRetargeter::Pose &from = framePose(stored(first));
    if (factor < 1e-6f) {
        return from;
    }
    return JaRetargeter::blend(from, framePose(stored(second)), factor);
}

std::vector<JaRetargeter::Pose> JaSequencer::timeline(const std::vector<JaSequenceStep> &steps) const {
    std::vector<JaRetargeter::Pose> poses;
    for (auto &step : steps) {
        bool fullLength = step.seconds < 0.0f;
        float length = fullLength ? clipLength(step.clip) : step.seconds;
        // A whole clip runs through its last frame: enough intervals to reach
        // it (allowing for rounding), the last sample held at the clip's end.
        int intervals = fullLength
                            ? static_cast<int>(std::ceil(length * _sampleRate - 1e-3f))
                            : static_cast<int>(std::lround(length * _sampleRate));
        intervals = std::max(1, intervals);
        int count = fullLength ? intervals + 1 : intervals;
        std::optional<JaRetargeter::Pose> previous;
        if (!poses.empty()) {
            previous = poses.back();
        }
        for (int k = 0; k < count; ++k) {
            float time = static_cast<float>(k) / _sampleRate;
            JaRetargeter::Pose pose(sample(step.clip, fullLength ? std::min(time, length) : time));
            if (previous && step.blendIn > 0.0f && time < step.blendIn) {
                pose = JaRetargeter::blend(*previous, pose, time / step.blendIn);
            }
            poses.push_back(std::move(pose));
        }
    }
    return poses;
}

std::shared_ptr<Animation> JaSequencer::animation(
    std::string name,
    const std::vector<JaSequenceStep> &steps,
    float transitionTime) const {

    auto poses = timeline(steps);
    if (poses.empty()) {
        throw std::invalid_argument("A JA sequence needs at least one step");
    }
    float length = static_cast<float>(poses.size() - 1) / _sampleRate;

    // Animation nodes are found by name; the root's name matches no model node.
    auto root = std::make_shared<ModelNode>(
        0xffff,
        "ja_animation_root",
        glm::vec3(0.0f),
        glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
        false);
    const auto &nodes = _retargeter.nodes();
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto &node = nodes[i];
        if (!node.posed) {
            continue;
        }
        auto animNode = std::make_shared<ModelNode>(
            node.number,
            node.name,
            node.restPosition,
            node.restOrientation,
            true,
            root.get());
        auto &orientations = animNode->quaternionTracks()[ControllerTypes::orientation];
        bool constant = std::all_of(poses.begin(), poses.end(), [&](const auto &pose) {
            return pose.orientations[i] == poses.front().orientations[i];
        });
        glm::quat previous(poses.front().orientations[i]);
        for (size_t k = 0; k < (constant ? 1 : poses.size()); ++k) {
            glm::quat orientation(poses[k].orientations[i]);
            if (glm::dot(orientation, previous) < 0.0f) {
                orientation = -orientation; // keep neighbouring keys on the same hemisphere
            }
            orientations.add(static_cast<float>(k) / _sampleRate, orientation);
            previous = orientation;
        }
        orientations.update();
        if (static_cast<int>(i) == _retargeter.rootNodeIndex()) {
            // Position keys are offsets from the rest position.
            auto &positions = animNode->vectorTracks()[ControllerTypes::position];
            for (size_t k = 0; k < poses.size(); ++k) {
                positions.add(static_cast<float>(k) / _sampleRate, poses[k].rootPosition - node.restPosition);
            }
            positions.update();
        }
        root->addChild(std::move(animNode));
    }

    return std::make_shared<Animation>(
        std::move(name),
        length,
        transitionTime,
        "",
        std::move(root),
        std::vector<Animation::Event>());
}

} // namespace game

} // namespace reone
