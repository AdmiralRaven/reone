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

namespace reone {

namespace game {

/**
 * Jedi Academy's single-player third-person camera, in KOTOR's metres and
 * axes. It follows the rules of OpenJK's code/cgame/cg_view.cpp
 * (CG_OffsetThirdPersonView and the functions it calls) without its code:
 *
 * - The camera aims at a point above the eyes and stands a fixed range behind
 *   it, along the view: looking down raises it behind the character, looking
 *   up lowers it.
 * - Each frame the aim point and the camera close part of the gap to where
 *   they belong, as much per 50 ms as JA's damping says. The camera follows
 *   more tightly the steeper the view and the faster it turns.
 * - Something between the eyes and the aim point, or between the aim point
 *   and the camera, stops it short by JA's camera box.
 *
 * JA's sizes are in its own units; they scale to the character by its eye
 * height. Yaw is reone's facing: 0 looks along +Y and positive turns to the
 * left. Positive pitch looks down, as in JA.
 */
class JaCamera {
public:
    /** Settings in JA units, as JA's cvars name them. */
    struct Settings {
        float range {80.0f};      /**< cg_thirdPersonRange: the camera behind the aim point */
        float vertOffset {16.0f}; /**< cg_thirdPersonVertOffset: the aim point above the eyes */
        float horzOffset {0.0f};  /**< aim point and camera to the right of the eyes; not JA's (see shoulder()) */
        float cameraDamp {0.3f};  /**< cg_thirdPersonCameraDamp */
        float targetDamp {0.5f};  /**< cg_thirdPersonTargetDamp */

        /** JA's own view, with its cvars' defaults: straight behind. */
        static Settings behind();

        /**
         * A view over the right shoulder, closer and lower: a proposal to try,
         * not JA's. JA's own sideways cvar, cg_thirdPersonHorzOffset (0 by
         * default), shifts the camera after aiming it and feeds the shift back
         * into the damping; this shifts the aim point and the camera together.
         */
        static Settings shoulder();

        /**
         * The same view with the camera \p range units behind the aim point.
         * The sideways offset keeps its share of the range, so the character
         * stays where it was on the screen, only nearer or farther.
         */
        Settings atRange(float range) const;
    };

    /**
     * JA's standing eye height above the feet: the player's origin is 24
     * units up (DEFAULT_MINS_2) and the view 36 above that (DEFAULT_MAXS_2
     * plus STANDARD_VIEWHEIGHT_OFFSET).
     */
    static constexpr float kJaEyeHeight = 60.0f;

    /** Half the size of JA's camera box, CAMERA_SIZE. */
    static constexpr float kJaCameraSize = 4.0f;

    /** The camera code's own pitch cap, 89 degrees. */
    static constexpr float kMaxCameraPitch = 1.553343f;

    /** How far the player's view tilts up or down in JA: 75 degrees (PM_UpdateViewAngles). */
    static constexpr float kMaxViewPitch = 1.308997f;

    /** The distance from \p from toward \p to at which something first meets the line between them; none when the way is clear. */
    using Obstruction = std::function<std::optional<float>(const glm::vec3 &from, const glm::vec3 &to)>;

    /** \param eyeHeight the character's eyes above its feet, in metres */
    explicit JaCamera(float eyeHeight);

    float eyeHeight() const { return _eyeHeight; }

    /** Metres per JA unit. */
    float scale() const { return _scale; }

    /** Starts from another camera's view, which the first updates leave behind as damping allows. */
    void start(const glm::vec3 &eye, const glm::vec3 &aim, float yaw);

    /**
     * Moves the camera on by \p dt seconds toward its view of a character at
     * \p feet looking at \p yaw and \p pitch. The first update without
     * start() goes straight there, as JA's first frame does.
     */
    void update(float dt, const glm::vec3 &feet, float yaw, float pitch, const Settings &settings, const Obstruction &obstruction);

    /** Where the camera stands. */
    const glm::vec3 &eye() const { return _location; }

    /** A point the camera looks at: its aim point, or along the view when it stands too close to aim from. */
    glm::vec3 aim() const;

    /** The aim point before damping and walls, for a character at \p feet: CG_CalcIdealThirdPersonViewTarget. */
    glm::vec3 idealTarget(const glm::vec3 &feet, float yaw, const Settings &settings) const;

    /** The camera before damping and walls, behind \p target: CG_CalcIdealThirdPersonViewLocation. */
    glm::vec3 idealLocation(const glm::vec3 &target, float yaw, float pitch, const Settings &settings) const;

    /** The direction the view looks, at a pitch within the camera's cap. */
    static glm::vec3 viewForward(float yaw, float pitch);

    /** The view's horizontal right. */
    static glm::vec3 viewRight(float yaw);

private:
    float _eyeHeight;
    float _scale;
    bool _started {false};
    glm::vec3 _target {0.0f};
    glm::vec3 _location {0.0f};
    glm::vec3 _forward {0.0f, 1.0f, 0.0f};
    float _lastYaw {0.0f};

    void reset(const glm::vec3 &feet, float yaw, float pitch, const Settings &settings, const Obstruction &obstruction);
    glm::vec3 stopShort(const glm::vec3 &from, const glm::vec3 &to, const Obstruction &obstruction) const;
};

} // namespace game

} // namespace reone
