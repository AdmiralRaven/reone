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

#include "reone/game/jaanimator.h"

#include "reone/graphics/animation.h"
#include "reone/graphics/modelnode.h"
#include "reone/graphics/types.h"
#include "reone/scene/node/model.h"
#include "reone/scene/node/modelnode.h"

using namespace reone::graphics;
using namespace reone::scene;

namespace reone {

namespace game {

static constexpr const char *kUpperTorsoNode = "torsoupr_g";
static constexpr float kLowerSpineShare = 0.8f;

// Ghoul2's default blend, which the legs take into a walk or a run
// (SETANIM_BLEND_DEFAULT, bg_public.h:176).
static constexpr float kLegsBlend = 0.1f;

// The torso's: a run takes the torso at the default blend
// (bg_panimate.cpp:5243-5252); otherwise the torso goes through LS_READY,
// whose blend is 350 ms, also when that resolves to a walk
// (bg_pmove.cpp:9419-9448).
static constexpr float kReadyBlend = 0.35f;

// How fast the legs turn toward the movement: Jedi Academy turns them at most
// 5 degrees a frame while moving and 10 otherwise (cg_players.cpp:1866-1891),
// taken here at 60 frames a second.
static const float kLegsTurnMoving = glm::radians(300.0f);
static const float kLegsTurnStanding = glm::radians(600.0f);

JaBodyAnimator::JaBodyAnimator(const JaSequencer &sequencer, const JaRetargeter &retargeter) :
    _sequencer(sequencer),
    _retargeter(retargeter) {
    int torso = _retargeter.nodeIndex(kTorsoNode);
    if (torso < 0) {
        throw std::runtime_error(std::string("The body has no ") + kTorsoNode);
    }
    const auto &nodes = _retargeter.nodes();
    _torso.resize(nodes.size(), false);
    for (size_t i = 0; i < nodes.size(); ++i) {
        // Parents come before their children.
        _torso[i] = static_cast<int>(i) == torso || (nodes[i].parent >= 0 && _torso[nodes[i].parent]);
    }
    JaRetargeter::Pose rest;
    for (auto &node : nodes) {
        rest.orientations.push_back(node.restOrientation);
    }
    rest.rootPosition = nodes[_retargeter.rootNodeIndex()].restPosition;
    hold(rest);
}

void JaBodyAnimator::hold(const JaRetargeter::Pose &pose) {
    for (Layer *layer : {&_legs, &_torsoLayer}) {
        *layer = Layer();
        layer->held = pose;
    }
}

void JaBodyAnimator::play(int parts, const std::string &clip, float rate, float blendSeconds, bool restart) {
    // Each part blends from what it showed before either changed.
    if (parts & kLegs) {
        play(_legs, clip, rate, blendSeconds, restart);
    }
    if (parts & kTorso) {
        play(_torsoLayer, clip, rate, blendSeconds, restart);
    }
}

void JaBodyAnimator::play(Layer &layer, const std::string &clip, float rate, float blendSeconds, bool restart) {
    std::string name(boost::to_upper_copy(clip));
    if (!restart && layer.clip == name) {
        layer.rate = rate;
        return;
    }
    const GhoulAnimationClip &info = _sequencer.clip(name);
    JaRetargeter::Pose from(layerPose(layer));
    const Layer &other = &layer == &_legs ? _torsoLayer : _legs;
    layer.time = !restart && other.clip == name ? other.time : 0.0f;
    layer.clip = name;
    layer.rate = rate;
    float ownRate = info.fps != 0.0f ? std::abs(info.fps) : 20.0f;
    layer.period = info.loopFrames >= 0 && info.numFrames > 1 ? static_cast<float>(info.numFrames) / ownRate : 0.0f;
    if (blendSeconds > 0.0f) {
        layer.from = std::move(from);
        layer.blendTime = blendSeconds;
        layer.blended = 0.0f;
    } else {
        layer.from.reset();
    }
}

void JaBodyAnimator::advance(float dt) {
    for (Layer *layer : {&_legs, &_torsoLayer}) {
        if (!layer->clip.empty()) {
            layer->time += dt * layer->rate;
            if (layer->period > 0.0f && layer->time >= layer->period) {
                layer->time = std::fmod(layer->time, layer->period);
            }
        }
        if (layer->from) {
            layer->blended += dt;
            if (layer->blended >= layer->blendTime) {
                layer->from.reset();
            }
        }
    }
}

JaRetargeter::Pose JaBodyAnimator::layerPose(const Layer &layer) const {
    JaRetargeter::Pose pose(layer.clip.empty() ? layer.held : _sequencer.sample(layer.clip, layer.time));
    if (layer.from) {
        pose = JaRetargeter::blend(*layer.from, pose, layer.blended / layer.blendTime);
    }
    return pose;
}

JaRetargeter::Pose JaBodyAnimator::pose() const {
    JaRetargeter::Pose legs(layerPose(_legs));
    bool same = _legs.clip == _torsoLayer.clip && _legs.time == _torsoLayer.time &&
                !_legs.from && !_torsoLayer.from && !_legs.clip.empty();
    if (same) {
        return legs;
    }
    JaRetargeter::Pose torso(layerPose(_torsoLayer));
    for (size_t i = 0; i < _torso.size(); ++i) {
        if (_torso[i]) {
            legs.orientations[i] = torso.orientations[i];
        }
    }
    return legs;
}

const JaBodyAnimator::Layer &JaBodyAnimator::layer(Part part) const {
    return part == kTorso ? _torsoLayer : _legs;
}

const std::string &JaBodyAnimator::clip(Part part) const {
    return layer(part).clip;
}

float JaBodyAnimator::clipTime(Part part) const {
    return layer(part).time;
}

bool JaBodyAnimator::isBlending(Part part) const {
    return static_cast<bool>(layer(part).from);
}

void jaTurnTorso(const JaRetargeter &retargeter, JaRetargeter::Pose &pose, float yawRadians) {
    if (yawRadians == 0.0f) {
        return;
    }
    int torso = retargeter.nodeIndex(JaBodyAnimator::kTorsoNode);
    int upper = retargeter.nodeIndex(kUpperTorsoNode);
    if (torso < 0 || upper < 0) {
        return;
    }
    // Turning a node by a model-space rotation, its parent's model-space
    // rotation staying as it was.
    auto transforms = retargeter.transforms(pose);
    auto turn = [&](int node, float angle) {
        int parent = retargeter.nodes()[node].parent;
        glm::quat parentRotation(parent >= 0 ? glm::normalize(glm::quat_cast(glm::mat3(transforms[parent]))) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        glm::quat about(glm::angleAxis(angle, glm::vec3(0.0f, 0.0f, 1.0f)));
        pose.orientations[node] = glm::normalize(glm::inverse(parentRotation) * about * parentRotation * pose.orientations[node]);
    };
    turn(torso, kLowerSpineShare * yawRadians);
    turn(upper, (1.0f - kLowerSpineShare) * yawRadians);
}

JaRetargeter::Pose jaCurrentPose(const JaRetargeter &retargeter, ModelSceneNode &model) {
    JaRetargeter::Pose pose;
    const auto &nodes = retargeter.nodes();
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto &node = nodes[i];
        auto sceneNode = model.getNodeByNumber(node.number);
        if (!sceneNode) {
            pose.orientations.push_back(node.restOrientation);
            if (static_cast<int>(i) == retargeter.rootNodeIndex()) {
                pose.rootPosition = node.restPosition;
            }
            continue;
        }
        const glm::mat4 &local = sceneNode->localTransform();
        glm::mat3 rotation(glm::normalize(glm::vec3(local[0])), glm::normalize(glm::vec3(local[1])), glm::normalize(glm::vec3(local[2])));
        pose.orientations.push_back(glm::normalize(glm::quat_cast(rotation)));
        if (static_cast<int>(i) == retargeter.rootNodeIndex()) {
            pose.rootPosition = glm::vec3(local[3]);
        }
    }
    return pose;
}

const std::vector<std::string> &JaSaberDriver::locomotionClips() {
    static const std::vector<std::string> clips {"BOTH_RUN2", "BOTH_RUNBACK2", "BOTH_WALK2", "BOTH_WALKBACK2"};
    return clips;
}

static JaSaberStateMachine makeMachine(const JaSequencer &sequencer, const std::map<std::string, GhoulAnimationClip> &clips) {
    std::vector<std::string> missing;
    for (auto &clip : JaSaberDriver::locomotionClips()) {
        if (!sequencer.hasClip(clip)) {
            missing.push_back(clip);
        }
    }
    if (!missing.empty()) {
        throw std::runtime_error("The JA animations lack " + boost::join(missing, ", "));
    }
    return JaSaberStateMachine(JaSaberMoves::fast(), clips);
}

JaSaberDriver::JaSaberDriver(
    const JaSequencer &sequencer,
    const JaRetargeter &retargeter,
    const std::map<std::string, GhoulAnimationClip> &clips) :
    _sequencer(sequencer),
    _retargeter(retargeter),
    _machine(makeMachine(sequencer, clips)),
    _animator(sequencer, retargeter) {
}

void JaSaberDriver::start(const JaRetargeter::Pose &pose) {
    _machine.reset();
    _legsYaw = 0.0f;
    _animator.hold(pose);
    const auto &moves = _machine.moves();
    const auto &ready = moves.move(moves.ready());
    const auto &timing = _machine.timing(ready.id);
    _animator.play(JaBodyAnimator::kBody, ready.clip, timing.rate, timing.blendMs / 1000.0f);
}

JaSaberDriver::Step JaSaberDriver::step(float dt, const JaSaberInput &input, bool walk, float speed) {
    Step result;
    _animator.advance(dt);
    _legsHold = std::max(0.0f, _legsHold - std::max(0.0f, dt));
    result.started = _machine.tick(dt, input);
    const auto &moves = _machine.moves();
    result.moving = input.forward != 0 || input.right != 0;
    for (int id : result.started) {
        const auto &timing = _machine.timing(id);
        if (!result.moving) {
            _animator.play(JaBodyAnimator::kBody, moves.move(id).clip, timing.rate, timing.blendMs / 1000.0f);
            _legsHold = timing.holdMs / 1000.0f;
        } else if (id != moves.ready()) {
            // The legs go on walking or running; the ready stance takes up
            // their clip on the torso, below.
            _animator.play(JaBodyAnimator::kTorso, moves.move(id).clip, timing.rate, timing.blendMs / 1000.0f);
        }
    }

    bool ready = _machine.move() == moves.ready();
    if (result.moving && input.forward < 0 && moves.move(_machine.move()).kind == JaMoveKind::Attack) {
        result.speedScale = kBackwardsAttackSpeed;
    }
    const auto &locomotion = locomotionClips();
    bool legsFree = _legsHold <= 0.0f;
    if (result.moving && legsFree) {
        std::string clip(jaLocomotionClip(input.forward, walk));
        float rate = jaFootSlideRate(_sequencer.clip(clip), walk, speed * result.speedScale / _retargeter.scale());
        bool torsoJoins = ready && _animator.clip(JaBodyAnimator::kTorso) != clip;
        if (_animator.clip(JaBodyAnimator::kLegs) != clip) {
            _animator.play(JaBodyAnimator::kLegs, clip, rate, kLegsBlend);
        } else {
            _animator.play(JaBodyAnimator::kLegs, clip, rate, 0.0f, false);
        }
        if (torsoJoins) {
            // In step with the legs, as g_synchSplitAnims plays it.
            _animator.play(JaBodyAnimator::kTorso, clip, rate, walk ? kReadyBlend : kLegsBlend, false);
        } else if (ready) {
            _animator.play(JaBodyAnimator::kTorso, clip, rate, 0.0f, false);
        }
    } else if (!result.moving && legsFree &&
               std::find(locomotion.begin(), locomotion.end(), _animator.clip(JaBodyAnimator::kLegs)) != locomotion.end()) {
        // Stopping: the legs back into the ready stance, and the torso too
        // unless a move plays on it.
        const auto &stance = moves.move(moves.ready());
        const auto &timing = _machine.timing(stance.id);
        _animator.play(JaBodyAnimator::kLegs, stance.clip, timing.rate, kLegsBlend);
        if (ready) {
            _animator.play(JaBodyAnimator::kTorso, stance.clip, timing.rate, timing.blendMs / 1000.0f);
        }
    }

    float target = result.moving ? glm::radians(jaLegsYawTarget(input.forward, input.right)) : 0.0f;
    float turn = (result.moving ? kLegsTurnMoving : kLegsTurnStanding) * std::max(0.0f, dt);
    _legsYaw += glm::clamp(target - _legsYaw, -turn, turn);
    return result;
}

JaRetargeter::Pose JaSaberDriver::pose() const {
    auto pose = _animator.pose();
    jaTurnTorso(_retargeter, pose, -_legsYaw);
    return pose;
}

JaLivePose::JaLivePose(const JaRetargeter &retargeter, std::string name) :
    _retargeter(retargeter) {
    auto root = std::make_shared<ModelNode>(
        0xffff,
        "ja_live_root",
        glm::vec3(0.0f),
        glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
        false);
    const auto &nodes = _retargeter.nodes();
    _nodes.resize(nodes.size(), nullptr);
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
        _nodes[i] = animNode.get();
        root->addChild(std::move(animNode));
    }
    _animation = std::make_shared<Animation>(
        std::move(name),
        1.0f,
        0.0f,
        "",
        std::move(root),
        std::vector<Animation::Event>());
    JaRetargeter::Pose rest;
    for (auto &node : nodes) {
        rest.orientations.push_back(node.restOrientation);
    }
    rest.rootPosition = nodes[_retargeter.rootNodeIndex()].restPosition;
    set(rest);
}

void JaLivePose::set(const JaRetargeter::Pose &pose) {
    for (size_t i = 0; i < _nodes.size(); ++i) {
        if (!_nodes[i]) {
            continue;
        }
        KeyframeTrack<glm::quat> orientation;
        orientation.add(0.0f, pose.orientations[i]);
        _nodes[i]->quaternionTracks()[ControllerTypes::orientation] = std::move(orientation);
        if (static_cast<int>(i) == _retargeter.rootNodeIndex()) {
            // Position keys are offsets from the rest position.
            KeyframeTrack<glm::vec3> position;
            position.add(0.0f, pose.rootPosition - _retargeter.nodes()[i].restPosition);
            _nodes[i]->vectorTracks()[ControllerTypes::position] = std::move(position);
        }
    }
}

} // namespace game

} // namespace reone
