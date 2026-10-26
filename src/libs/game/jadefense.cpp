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

#include "reone/game/jadefense.h"

#include "reone/system/randomutil.h"

namespace reone {

namespace game {

namespace {

constexpr float kEpsilon = 1e-6f;

// How many halvings find a ball's first contact: to a millionth of its way.
constexpr int kBisections = 20;

float distanceToSegment(const glm::vec3 &point, const glm::vec3 &a, const glm::vec3 &b) {
    glm::vec3 ab(b - a);
    float length2 = glm::dot(ab, ab);
    float share = length2 > kEpsilon ? glm::clamp(glm::dot(point - a, ab) / length2, 0.0f, 1.0f) : 0.0f;
    return glm::distance(point, a + ab * share);
}

int clampDefense(int defense) {
    return glm::clamp(defense, 0, kJaMaxDefense);
}

} // namespace

JaRandom jaGameRandom() {
    return [](int low, int high) { return randomInt(low, high); };
}

JaRandomFloat jaGameRandomFloat() {
    return [](float low, float high) { return randomFloat(low, high); };
}

glm::vec2 jaAhead(float facing) {
    return glm::vec2(-std::sin(facing), std::cos(facing));
}

glm::vec2 jaRight(float facing) {
    return glm::vec2(std::cos(facing), std::sin(facing));
}

bool jaInFrontLevel(const glm::vec3 &point, const glm::vec3 &from, float facing, float minDot) {
    glm::vec2 toward(glm::vec2(point) - glm::vec2(from));
    float length = glm::length(toward);
    if (length < kEpsilon) {
        return false;
    }
    return glm::dot(toward / length, jaAhead(facing)) > minDot;
}

bool jaInFront(const glm::vec3 &point, const glm::vec3 &from, float facing, float minDot) {
    glm::vec3 toward(point - from);
    float length = glm::length(toward);
    if (length < kEpsilon) {
        return false;
    }
    glm::vec2 ahead(jaAhead(facing));
    return glm::dot(toward / length, glm::vec3(ahead, 0.0f)) >= minDot;
}

JaBlock jaBlockAt(const glm::vec3 &impact, const glm::vec3 &eye, float facing, float metresPerUnit) {
    // WP_SaberBlockNonRandom: the direction to the impact, level, against the
    // view's right; its height against the eye's, in JA units.
    glm::vec2 level(glm::vec2(impact) - glm::vec2(eye));
    float length = glm::length(level);
    float rightDot = length > kEpsilon ? glm::dot(level / length, jaRight(facing)) : 0.0f;
    float scale = metresPerUnit > kEpsilon ? metresPerUnit : 1.0f;
    return jaBlockFor((impact.z - eye.z) / scale, rightDot);
}

JaGuard jaGuardOf(const JaSaberMove &move) {
    switch (move.kind) {
    case JaMoveKind::Ready:
    case JaMoveKind::Parry:
        return JaGuard::Wide;
    case JaMoveKind::Start:
    case JaMoveKind::Attack:
    case JaMoveKind::Return:
        return JaGuard::Tight;
    default:
        return JaGuard::None;
    }
}

std::vector<JaBodyPart> jaWideGuard(const std::vector<JaBodyPart> &body, float facing, float metresPerUnit) {
    glm::vec3 forward(jaAhead(facing) * kJaWideGuardForward * metresPerUnit, 0.0f);
    std::vector<JaBodyPart> guard;
    guard.reserve(body.size());
    for (auto part : body) {
        part.a += forward;
        part.b += forward;
        part.radius += kJaWideGuardPadding * metresPerUnit;
        guard.push_back(part);
    }
    return guard;
}

std::vector<JaBodyPart> jaTightGuard(const std::vector<JaBlade> &blades, float metresPerUnit) {
    std::vector<JaBodyPart> guard;
    for (auto &blade : blades) {
        JaBodyPart part;
        part.a = blade.base;
        part.b = blade.tip;
        part.radius = kJaTightGuardRadius * metresPerUnit;
        guard.push_back(part);
    }
    return guard;
}

std::optional<JaPathContact> jaSweepBall(
    const glm::vec3 &from,
    const glm::vec3 &to,
    float radius,
    const std::vector<JaBodyPart> &parts) {

    std::optional<JaPathContact> first;
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto &part = parts[i];
        float reach = part.radius + radius;
        auto gap = [&](float time) {
            return distanceToSegment(from + (to - from) * time, part.a, part.b) - reach;
        };
        // The distance from a point moving straight to a segment changes
        // convexly along the way, so the way's nearest point to the capsule,
        // found by a golden-section search, decides whether it touches, and
        // the first touch lies before it.
        float low = 0.0f;
        float high = 1.0f;
        constexpr float kRatio = 0.618034f;
        float x1 = high - kRatio * (high - low);
        float x2 = low + kRatio * (high - low);
        float g1 = gap(x1);
        float g2 = gap(x2);
        for (int k = 0; k < 40; ++k) {
            if (g1 < g2) {
                high = x2;
                x2 = x1;
                g2 = g1;
                x1 = high - kRatio * (high - low);
                g1 = gap(x1);
            } else {
                low = x1;
                x1 = x2;
                g1 = g2;
                x2 = low + kRatio * (high - low);
                g2 = gap(x2);
            }
        }
        float nearestTime = 0.5f * (low + high);
        if (gap(1.0f) < gap(nearestTime)) {
            nearestTime = 1.0f;
        }
        if (gap(0.0f) <= gap(nearestTime)) {
            nearestTime = 0.0f;
        }
        if (gap(nearestTime) > 0.0f) {
            continue;
        }
        float time;
        if (gap(0.0f) <= 0.0f) {
            time = 0.0f;
        } else {
            float lowTime = 0.0f;
            float highTime = nearestTime;
            for (int k = 0; k < kBisections; ++k) {
                float mid = 0.5f * (lowTime + highTime);
                if (gap(mid) <= 0.0f) {
                    highTime = mid;
                } else {
                    lowTime = mid;
                }
            }
            time = highTime;
        }
        if (!first || time < first->time) {
            JaPathContact contact;
            contact.part = i;
            contact.time = time;
            contact.point = from + (to - from) * time;
            first = contact;
        }
    }
    return first;
}

bool jaBoltPassesGuard(int defense, bool inFront, const JaRandom &random) {
    if (!inFront) {
        return true;
    }
    int d = clampDefense(defense);
    return random(0, d * d) == 0;
}

bool jaBoltSentBack(int defense, const JaRandom &random) {
    int chance;
    switch (clampDefense(defense)) {
    case 3:
        chance = 10;
        break;
    case 2:
        chance = 3;
        break;
    case 1:
        chance = 1;
        break;
    default:
        return false;
    }
    return random(0, chance) != 0;
}

int jaBoltParryDebounceMs(int defense) {
    static const int kDebounce[] {500, 300, 150, 50};
    return kDebounce[clampDefense(defense)];
}

int jaParryWeaponTimeMs(int defense) {
    int d = clampDefense(defense);
    return d <= 1 ? jaBoltParryDebounceMs(d) : 0;
}

JaReflectSteadiness jaReflectSteadiness(const JaSaberMove &move) {
    switch (move.kind) {
    case JaMoveKind::Ready:
    case JaMoveKind::Parry:
        return JaReflectSteadiness::Steady;
    case JaMoveKind::Attack:
    case JaMoveKind::Transition:
        return JaReflectSteadiness::Swinging;
    default:
        return JaReflectSteadiness::Other;
    }
}

glm::vec3 jaReflectDirection(
    const glm::vec3 &bolt,
    const JaReflectTargets &targets,
    int defense,
    JaReflectSteadiness steadiness,
    float metresPerUnit,
    const JaRandom &random,
    const JaRandomFloat &randomFloat) {

    int d = clampDefense(defense);
    auto jitter = [&randomFloat](glm::vec3 &direction, float amount) {
        for (int i = 0; i < 3; ++i) {
            direction[i] += randomFloat(-amount, amount);
        }
    };
    auto normalised = [](const glm::vec3 &direction, const glm::vec3 &fallback) {
        float length = glm::length(direction);
        return length > kEpsilon ? direction / length : fallback;
    };
    glm::vec3 back(normalised(targets.shooter - bolt, glm::vec3(0.0f, 0.0f, 1.0f)));

    // At rank 3, and one time in four at rank 2, at an enemy (g_missile.cpp:161-198).
    if (d > 2 || (d > 1 && random(0, 3) == 0)) {
        std::optional<glm::vec3> head;
        if (targets.currentEnemyHead && random(0, 3) != 0) {
            head = targets.currentEnemyHead;
        } else if (targets.otherEnemyHead) {
            head = targets.otherEnemyHead;
        } else {
            // Jedi_FindEnemyInCone falls back to the current enemy.
            head = targets.currentEnemyHead;
        }
        if (head) {
            glm::vec3 bullseye(*head);
            bullseye.x += static_cast<float>(random(-4, 4)) * metresPerUnit;
            bullseye.y += static_cast<float>(random(-4, 4)) * metresPerUnit;
            bullseye.z += static_cast<float>(random(-16, 4)) * metresPerUnit;
            glm::vec3 direction(normalised(bullseye - bolt, back));
            if (steadiness == JaReflectSteadiness::Swinging) {
                jitter(direction, 0.2f);
            } else if (steadiness == JaReflectSteadiness::Other) {
                jitter(direction, 0.1f);
            }
            return normalised(direction, back);
        }
    }

    // Back at the shooter, off by the rank and the move (g_missile.cpp:209-262).
    glm::vec3 direction(back);
    jitter(direction, d <= 1 ? 0.4f : 0.2f);
    if (steadiness == JaReflectSteadiness::Swinging) {
        jitter(direction, 0.3f);
    } else if (steadiness == JaReflectSteadiness::Other) {
        jitter(direction, 0.1f);
    }
    return normalised(direction, back);
}

} // namespace game

} // namespace reone
