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

#include "jaretarget.h"
#include "jasabermoves.h"

namespace reone {

namespace graphics {

class ModelNode;

} // namespace graphics

namespace scene {

class ModelSceneNode;

} // namespace scene

namespace game {

/**
 * Plays Jedi Academy clips on a KOTOR body as Ghoul2 plays them: a clip
 * starts on its first frame, and the body blends from the pose it showed at
 * that moment into the clip, evenly over the blend time. The legs and the
 * torso play separately, as Jedi Academy's legs and torso animations do, so
 * each can have its own clip, speed and blend. The torso is KOTOR's torso_g
 * and everything under it; the legs are the rest, the root dummy's position
 * included.
 */
class JaBodyAnimator : boost::noncopyable {
public:
    enum Part {
        kLegs = 1,
        kTorso = 2,
        kBody = kLegs | kTorso
    };

    static constexpr const char *kTorsoNode = "torso_g";

    /** \throws std::runtime_error when the body has no torso node */
    JaBodyAnimator(const JaSequencer &sequencer, const JaRetargeter &retargeter);

    /** Shows \p pose on the whole body, held, with no clip: where JA's clips take over from KOTOR's. */
    void hold(const JaRetargeter::Pose &pose);

    /**
     * Plays \p clip on \p parts from its first frame, at \p rate times its
     * own speed, each part blending from what it shows now over
     * \p blendSeconds. A part already playing the clip goes on, at the new
     * rate, unless \p restart.
     */
    void play(int parts, const std::string &clip, float rate, float blendSeconds, bool restart = true);

    void advance(float dt);

    /** The pose the body shows now. */
    JaRetargeter::Pose pose() const;

    /** The clip a part plays; empty while it holds a pose. */
    const std::string &clip(Part part) const;

    /** How far into its clip a part is, in seconds at the clip's own speed. */
    float clipTime(Part part) const;

    bool isBlending(Part part) const;

private:
    struct Layer {
        std::string clip;
        float time {0.0f};
        float rate {1.0f};
        float period {0.0f}; /**< a looping clip's length; 0 for one that plays once */
        JaRetargeter::Pose held;
        std::optional<JaRetargeter::Pose> from;
        float blendTime {0.0f};
        float blended {0.0f};
    };

    const JaSequencer &_sequencer;
    const JaRetargeter &_retargeter;
    std::vector<bool> _torso; /**< per retargeter node */
    Layer _legs;
    Layer _torsoLayer;

    const Layer &layer(Part part) const;
    JaRetargeter::Pose layerPose(const Layer &layer) const;
    void play(Layer &layer, const std::string &clip, float rate, float blendSeconds, bool restart);
};

/**
 * Turns the torso by \p yawRadians about the body's vertical, positive to the
 * left, as Jedi Academy turns the spine back toward the view while the legs
 * face the movement (CG_G2ClientSpineAngles, cg_players.cpp:2022-2172). It
 * spreads the turn up the spine: KOTOR's torso_g takes 80 per cent, for JA's
 * lower and upper lumbar bones, and torsoupr_g the remaining 20, for its
 * thoracic one.
 */
void jaTurnTorso(const JaRetargeter &retargeter, JaRetargeter::Pose &pose, float yawRadians);

/**
 * The pose a model scene node shows now, as far as the retargeter poses it:
 * where a JA clip blends in from KOTOR's own animation.
 */
JaRetargeter::Pose jaCurrentPose(const JaRetargeter &retargeter, scene::ModelSceneNode &model);

/**
 * A KOTOR animation that shows one body pose at any time: a single key per
 * posed node, which set() replaces. Played looping on the body's model scene
 * node, it shows each pose set from the next scene update on.
 */
class JaLivePose : boost::noncopyable {
public:
    JaLivePose(const JaRetargeter &retargeter, std::string name);

    void set(const JaRetargeter::Pose &pose);

    graphics::Animation &animation() { return *_animation; }
    const std::shared_ptr<graphics::Animation> &sharedAnimation() const { return _animation; }

private:
    const JaRetargeter &_retargeter;
    std::shared_ptr<graphics::Animation> _animation;
    std::vector<graphics::ModelNode *> _nodes; /**< per retargeter node; null for one not posed */
};

/**
 * Saber mode's body, apart from the game around it. Each tick:
 *
 * - the saber moves JaSaberStateMachine starts play on the whole body, as
 *   Jedi Academy plays them on a player standing still, with their blends;
 * - in the ready stance, moving plays Jedi Academy's saber-out walk or run at
 *   the speed g_noFootSlide gives it; the legs turn toward the movement and
 *   the torso back toward the view; standing again blends back into the
 *   stance;
 * - while any other move plays, the character stands: attacking on the move
 *   is not done yet.
 */
class JaSaberDriver : boost::noncopyable {
public:
    struct Step {
        std::vector<int> started; /**< the saber moves that started, in order */
        bool moving {false};      /**< whether the character moves this tick */
    };

    /** The saber-out walks and runs (PM_Footsteps, bg_pmove.cpp:8417-8595). */
    static const std::vector<std::string> &locomotionClips();

    /** \throws std::runtime_error when \p clips lack a saber move's clip, or a walk's or run's */
    JaSaberDriver(
        const JaSequencer &sequencer,
        const JaRetargeter &retargeter,
        const std::map<std::string, graphics::GhoulAnimationClip> &clips);

    /** Starts in the ready stance, blending in from \p pose. */
    void start(const JaRetargeter::Pose &pose);

    /**
     * \param walk whether moving would walk rather than run
     * \param speed how fast the character moves, in KOTOR units a second
     */
    Step step(float dt, const JaSaberInput &input, bool walk, float speed);

    /** The pose the body shows now, its torso turned back from the legs. */
    JaRetargeter::Pose pose() const;

    /** How far the legs face to the left of the view, in radians. */
    float legsYaw() const { return _legsYaw; }

    const JaSaberStateMachine &machine() const { return _machine; }
    const JaBodyAnimator &animator() const { return _animator; }

private:
    const JaSequencer &_sequencer;
    const JaRetargeter &_retargeter;
    JaSaberStateMachine _machine;
    JaBodyAnimator _animator;
    float _legsYaw {0.0f};
};

} // namespace game

} // namespace reone
