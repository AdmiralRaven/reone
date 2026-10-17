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

#include "reone/game/jacamera.h"

namespace reone {

namespace game {

// JA damps per CAMERA_DAMP_INTERVAL, 50 ms.
static constexpr float kDampInterval = 0.05f;

// Closer than one JA unit, the camera looks along the view instead of at its
// aim point.
static constexpr float kMinAimDistance = 1.0f;

// The camera damps less the steeper the view: the pitch, in degrees, over 115.
static constexpr float kPitchDampDivisor = 115.0f;

JaCamera::Settings JaCamera::Settings::behind() {
    return Settings();
}

JaCamera::Settings JaCamera::Settings::shoulder() {
    // The user asked for "not sooo over the shoulder", 3/4 of the way there.
    return behind().towards(overShoulder(), 0.75f);
}

JaCamera::Settings JaCamera::Settings::overShoulder() {
    Settings settings;
    settings.range = 56.0f;
    settings.vertOffset = 4.0f;
    settings.horzOffset = 16.0f;
    return settings;
}

JaCamera::Settings JaCamera::Settings::towards(const Settings &other, float t) const {
    Settings settings(*this);
    settings.range = glm::mix(range, other.range, t);
    settings.vertOffset = glm::mix(vertOffset, other.vertOffset, t);
    settings.horzOffset = glm::mix(horzOffset, other.horzOffset, t);
    return settings;
}

JaCamera::Settings JaCamera::Settings::atRange(float newRange) const {
    Settings settings(*this);
    if (range > 0.0f) {
        settings.horzOffset = horzOffset * newRange / range;
    }
    settings.range = newRange;
    return settings;
}

JaCamera::JaCamera(float eyeHeight) :
    _eyeHeight(eyeHeight),
    _scale(eyeHeight / kJaEyeHeight) {
}

glm::vec3 JaCamera::viewForward(float yaw, float pitch) {
    float capped = glm::clamp(pitch, -kMaxCameraPitch, kMaxCameraPitch);
    return glm::vec3(
        -std::sin(yaw) * std::cos(capped),
        std::cos(yaw) * std::cos(capped),
        -std::sin(capped));
}

glm::vec3 JaCamera::viewRight(float yaw) {
    return glm::vec3(std::cos(yaw), std::sin(yaw), 0.0f);
}

glm::vec3 JaCamera::idealTarget(const glm::vec3 &feet, float yaw, const Settings &settings) const {
    glm::vec3 target(feet.x, feet.y, feet.z + _eyeHeight + settings.vertOffset * _scale);
    return target + viewRight(yaw) * (settings.horzOffset * _scale);
}

glm::vec3 JaCamera::idealLocation(const glm::vec3 &target, float yaw, float pitch, const Settings &settings) const {
    return target - viewForward(yaw, pitch) * (settings.range * _scale);
}

glm::vec3 JaCamera::stopShort(const glm::vec3 &from, const glm::vec3 &to, const Obstruction &obstruction) const {
    glm::vec3 delta(to - from);
    float length = glm::length(delta);
    if (!obstruction || length < 1e-4f) {
        return to;
    }
    std::optional<float> hit = obstruction(from, to);
    if (!hit || *hit >= length) {
        return to;
    }
    // JA traces a box; this stops a point the box's half-size short of the hit.
    float kept = std::max(0.0f, *hit - kJaCameraSize * _scale);
    return from + delta * (kept / length);
}

void JaCamera::start(const glm::vec3 &eye, const glm::vec3 &aim, float yaw) {
    _location = eye;
    _target = aim;
    glm::vec3 look(aim - eye);
    if (glm::length(look) > 1e-4f) {
        _forward = glm::normalize(look);
    }
    _lastYaw = yaw;
    _started = true;
}

void JaCamera::reset(const glm::vec3 &feet, float yaw, float pitch, const Settings &settings, const Obstruction &obstruction) {
    // CG_ResetThirdPersonViewDamp: straight to the ideal view, then the walls.
    glm::vec3 eyes(feet.x, feet.y, feet.z + _eyeHeight);
    glm::vec3 target(idealTarget(feet, yaw, settings));
    _forward = viewForward(yaw, pitch);
    _location = idealLocation(target, yaw, pitch, settings);
    _target = stopShort(eyes, target, obstruction);
    _location = stopShort(_target, _location, obstruction);
    _lastYaw = yaw;
    _started = true;
}

void JaCamera::update(float dt, const glm::vec3 &feet, float yaw, float pitch, const Settings &settings, const Obstruction &obstruction) {
    if (!_started) {
        reset(feet, yaw, pitch, settings, obstruction);
        return;
    }
    if (dt <= 0.0f) {
        return;
    }
    float capped = glm::clamp(pitch, -kMaxCameraPitch, kMaxCameraPitch);
    _forward = viewForward(yaw, capped);

    // A fast turn stiffens the camera: JA's cameraStiffFactor, from the turn
    // in degrees per millisecond.
    float turned = std::abs(yaw - _lastYaw);
    if (turned > glm::pi<float>()) {
        turned = std::abs(turned - glm::two_pi<float>());
    }
    float stiff = glm::degrees(turned) / (dt * 1000.0f);
    if (stiff < 1.0f) {
        stiff = 0.0f;
    } else if (stiff > 2.5f) {
        stiff = 0.75f;
    } else {
        stiff = (stiff - 1.0f) * 0.5f;
    }
    _lastYaw = yaw;

    // Damping leaves (1 - damp) of the gap per 50 ms.
    float intervals = dt / kDampInterval;

    // The aim point: CG_UpdateThirdPersonTargetDamp.
    glm::vec3 eyes(feet.x, feet.y, feet.z + _eyeHeight);
    glm::vec3 target(idealTarget(feet, yaw, settings));
    if (settings.targetDamp >= 1.0f) {
        _target = target;
    } else if (settings.targetDamp >= 0.0f) {
        float left = std::pow(1.0f - settings.targetDamp, intervals);
        _target = target - left * (target - _target);
    }
    _target = stopShort(eyes, _target, obstruction);

    // The camera: CG_UpdateThirdPersonCameraDamp, behind the ideal aim point.
    glm::vec3 location(idealLocation(target, yaw, capped, settings));
    float damp = 0.0f;
    if (settings.cameraDamp != 0.0f) {
        float steep = glm::degrees(std::abs(capped)) / kPitchDampDivisor;
        damp = (1.0f - settings.cameraDamp) * steep * steep + settings.cameraDamp;
        if (stiff > 0.0f) {
            damp += (1.0f - damp) * stiff;
        }
    }
    if (damp >= 1.0f) {
        _location = location;
    } else if (damp >= 0.0f) {
        float left = std::pow(1.0f - damp, intervals);
        _location = location - left * (location - _location);
    }
    _location = stopShort(_target, _location, obstruction);
}

glm::vec3 JaCamera::aim() const {
    glm::vec3 look(_target - _location);
    if (glm::length(look) < kMinAimDistance * _scale) {
        return _location + _forward;
    }
    return _target;
}

} // namespace game

} // namespace reone
