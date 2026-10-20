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

#include "reone/game/jacontact.h"

#include "reone/game/object/creature.h"
#include "reone/game/object/item.h"
#include "reone/graphics/model.h"
#include "reone/graphics/modelnode.h"
#include "reone/scene/node/model.h"
#include "reone/scene/node/modelnode.h"

using namespace reone::graphics;
using namespace reone::scene;

namespace reone {

namespace game {

namespace {

constexpr float kEpsilon = 1e-6f;

// The most steps one sweep takes, however far the blade moves.
constexpr int kMaxSweepSteps = 128;

/**
 * The squared distance between segments p1-q1 and p2-q2, with the closest
 * points' shares of each, \p s and \p t (Ericson, Real-Time Collision
 * Detection, 5.1.9).
 */
float closestPointsOfSegments(
    const glm::vec3 &p1, const glm::vec3 &q1,
    const glm::vec3 &p2, const glm::vec3 &q2,
    float &s, float &t) {

    glm::vec3 d1(q1 - p1);
    glm::vec3 d2(q2 - p2);
    glm::vec3 r(p1 - p2);
    float a = glm::dot(d1, d1);
    float e = glm::dot(d2, d2);
    float f = glm::dot(d2, r);
    if (a <= kEpsilon && e <= kEpsilon) {
        s = t = 0.0f;
        return glm::dot(r, r);
    }
    if (a <= kEpsilon) {
        s = 0.0f;
        t = glm::clamp(f / e, 0.0f, 1.0f);
    } else {
        float c = glm::dot(d1, r);
        if (e <= kEpsilon) {
            t = 0.0f;
            s = glm::clamp(-c / a, 0.0f, 1.0f);
        } else {
            float b = glm::dot(d1, d2);
            float denominator = a * e - b * b;
            s = denominator > kEpsilon ? glm::clamp((b * f - c * e) / denominator, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = glm::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = glm::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    glm::vec3 between((p1 + d1 * s) - (p2 + d2 * t));
    return glm::dot(between, between);
}

struct BladeState {
    glm::vec3 base {0.0f};
    glm::vec3 direction {0.0f, 0.0f, 1.0f};
    float length {0.0f};
};

BladeState bladeState(const JaBlade &blade) {
    BladeState state;
    state.base = blade.base;
    glm::vec3 along(blade.tip - blade.base);
    state.length = glm::length(along);
    if (state.length > kEpsilon) {
        state.direction = along / state.length;
    }
    return state;
}

// A turn from one direction to another, about one axis.
struct Turn {
    glm::vec3 axis {0.0f, 0.0f, 1.0f};
    float angle {0.0f};
};

Turn turnBetween(const glm::vec3 &from, const glm::vec3 &to) {
    Turn turn;
    float cosine = glm::clamp(glm::dot(from, to), -1.0f, 1.0f);
    turn.angle = std::acos(cosine);
    glm::vec3 axis(glm::cross(from, to));
    float axisLength = glm::length(axis);
    if (axisLength > kEpsilon) {
        turn.axis = axis / axisLength;
    } else if (cosine < 0.0f) {
        // A half turn: about any axis square to the blade.
        glm::vec3 other(std::abs(from.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f));
        turn.axis = glm::normalize(glm::cross(from, other));
    } else {
        turn.angle = 0.0f;
    }
    return turn;
}

glm::vec3 position(const glm::mat4 &transform) {
    return glm::vec3(transform[3]);
}

} // namespace

const char *jaBodyRegionName(JaBodyRegion region) {
    switch (region) {
    case JaBodyRegion::Head:
        return "head";
    case JaBodyRegion::Torso:
        return "torso";
    case JaBodyRegion::LeftArm:
        return "left arm";
    case JaBodyRegion::RightArm:
        return "right arm";
    case JaBodyRegion::LeftLeg:
        return "left leg";
    case JaBodyRegion::RightLeg:
        return "right leg";
    default:
        return "body";
    }
}

std::optional<JaBladeContact> jaSweepBlade(
    const JaBlade &from,
    const JaBlade &to,
    const std::vector<JaBodyPart> &parts,
    float bladeRadius) {

    if (parts.empty()) {
        return std::nullopt;
    }
    BladeState start(bladeState(from));
    BladeState end(bladeState(to));
    Turn turn(turnBetween(start.direction, end.direction));

    // No point of the blade moves farther in a step than the thinnest part's
    // reach: a path through a part's reach then has a step inside it unless
    // it passes farther out than sqrt(3)/2 of the reach, a graze.
    float reach = std::numeric_limits<float>::max();
    for (auto &part : parts) {
        reach = std::min(reach, part.radius + bladeRadius);
    }
    reach = std::max(reach, 0.01f);
    float farthest = glm::length(end.base - start.base) +
                     std::max(start.length, end.length) * turn.angle +
                     std::abs(end.length - start.length);
    int steps = glm::clamp(static_cast<int>(std::ceil(farthest / reach)), 1, kMaxSweepSteps);

    for (int step = 0; step <= steps; ++step) {
        float time = static_cast<float>(step) / static_cast<float>(steps);
        glm::vec3 base(glm::mix(start.base, end.base, time));
        glm::vec3 direction(glm::angleAxis(turn.angle * time, turn.axis) * start.direction);
        glm::vec3 tip(base + direction * glm::mix(start.length, end.length, time));

        std::optional<JaBladeContact> contact;
        float deepest = std::numeric_limits<float>::max();
        for (size_t i = 0; i < parts.size(); ++i) {
            const JaBodyPart &part = parts[i];
            float alongBlade, alongPart;
            float distance2 = closestPointsOfSegments(base, tip, part.a, part.b, alongBlade, alongPart);
            float partReach = part.radius + bladeRadius;
            if (distance2 > partReach * partReach) {
                continue;
            }
            // Of the parts touched at once, the one the blade is deepest in.
            float depth = std::sqrt(distance2) / std::max(partReach, kEpsilon);
            if (depth < deepest) {
                deepest = depth;
                JaBladeContact found;
                found.part = i;
                found.time = time;
                found.point = base + (tip - base) * alongBlade;
                contact = found;
            }
        }
        if (contact) {
            return contact;
        }
    }
    return std::nullopt;
}

namespace {

// A KOTOR humanoid's build: each capsule's radius as a share of the length
// from the pelvis to the head's hook. Fitted to the player's three body types
// at rest (pmbbl, pmbam and pfbam): for each capsule, the share at which it
// holds 55% of the skin nearest it, averaged over the three bodies and both
// sides, and rounded to two places.
constexpr float kTorsoShare = 0.34f;
constexpr float kHeadShare = 0.20f;
constexpr float kShouldersShare = 0.23f;
constexpr float kHipsShare = 0.28f;
constexpr float kUpperArmShare = 0.15f;
constexpr float kForearmShare = 0.09f;
constexpr float kHandShare = 0.12f;
constexpr float kThighShare = 0.23f;
constexpr float kShinShare = 0.19f;
constexpr float kFootShare = 0.21f;

// How far the head reaches above its hook, and a hand without fingertip
// nodes past its wrist, as shares of the same length.
constexpr float kHeadLengthShare = 0.45f;
constexpr float kHandLengthShare = 0.30f;

} // namespace

std::vector<JaBodyPart> jaHumanoidParts(const std::function<std::optional<glm::vec3>(const std::string &)> &nodePosition) {
    static const std::vector<std::string> required {
        "pelvis_g", "torsoupr_g", "headhook", "lcollar_g", "rcollar_g",
        "lbicep_g", "lforearm_g", "lhand_g", "rbicep_g", "rforearm_g", "rhand_g",
        "lthigh_g", "lshin_g", "lfoot_g", "rthigh_g", "rshin_g", "rfoot_g"};
    std::map<std::string, glm::vec3> nodes;
    for (auto &name : required) {
        auto found = nodePosition(name);
        if (!found) {
            return {};
        }
        nodes[name] = *found;
    }
    float build = glm::length(nodes["headhook"] - nodes["pelvis_g"]);
    if (build < 0.01f) {
        return {};
    }
    auto along = [](const glm::vec3 &from, const glm::vec3 &to) {
        glm::vec3 between(to - from);
        float length = glm::length(between);
        return length > kEpsilon ? between / length : glm::vec3(0.0f, 0.0f, 1.0f);
    };

    std::vector<JaBodyPart> parts;
    auto add = [&](JaBodyRegion region, const glm::vec3 &a, const glm::vec3 &b, float share) {
        JaBodyPart part;
        part.region = region;
        part.a = a;
        part.b = b;
        part.radius = share * build;
        parts.push_back(part);
    };
    glm::vec3 shoulders(0.5f * (nodes["lcollar_g"] + nodes["rcollar_g"]));
    add(JaBodyRegion::Torso, nodes["pelvis_g"], shoulders, kTorsoShare);
    add(JaBodyRegion::Head, nodes["headhook"], nodes["headhook"] + along(nodes["torsoupr_g"], nodes["headhook"]) * (kHeadLengthShare * build), kHeadShare);
    add(JaBodyRegion::Torso, nodes["lbicep_g"], nodes["rbicep_g"], kShouldersShare);
    add(JaBodyRegion::Torso, nodes["lthigh_g"], nodes["rthigh_g"], kHipsShare);
    for (const char *side : {"l", "r"}) {
        std::string s(side);
        JaBodyRegion arm = s == "l" ? JaBodyRegion::LeftArm : JaBodyRegion::RightArm;
        JaBodyRegion leg = s == "l" ? JaBodyRegion::LeftLeg : JaBodyRegion::RightLeg;
        const glm::vec3 &wrist = nodes[s + "hand_g"];
        add(arm, nodes[s + "bicep_g"], nodes[s + "forearm_g"], kUpperArmShare);
        add(arm, nodes[s + "forearm_g"], wrist, kForearmShare);
        // The hand to its middle fingertip, or on past the wrist without one.
        auto fingertip = nodePosition(s + "cfngrt_g");
        add(arm, wrist, fingertip ? *fingertip : wrist + along(nodes[s + "forearm_g"], wrist) * (kHandLengthShare * build), kHandShare);
        add(leg, nodes[s + "thigh_g"], nodes[s + "shin_g"], kThighShare);
        add(leg, nodes[s + "shin_g"], nodes[s + "foot_g"], kShinShare);
        // The foot to its toe; without a toe node, the foot is left out.
        if (auto toe = nodePosition(s + "foott_g")) {
            add(leg, nodes[s + "foot_g"], *toe, kFootShare);
        }
    }
    return parts;
}

JaBodyPart jaUprightBody(const AABB &box, const glm::mat4 &toWorld) {
    glm::vec3 min(box.min());
    glm::vec3 max(box.max());
    glm::vec3 center(0.5f * (min + max));
    float radius = 0.5f * std::min(max.x - min.x, max.y - min.y);
    float bottom = min.z + radius;
    float top = max.z - radius;
    if (top < bottom) {
        bottom = top = center.z;
    }
    JaBodyPart part;
    part.region = JaBodyRegion::Body;
    part.a = glm::vec3(toWorld * glm::vec4(center.x, center.y, bottom, 1.0f));
    part.b = glm::vec3(toWorld * glm::vec4(center.x, center.y, top, 1.0f));
    part.radius = radius * glm::length(glm::vec3(toWorld[0]));
    return part;
}

std::vector<JaBodyPart> jaBodyParts(ModelSceneNode &model) {
    auto parts = jaHumanoidParts([&model](const std::string &name) -> std::optional<glm::vec3> {
        auto node = model.getNodeByName(name);
        if (!node) {
            return std::nullopt;
        }
        return position(node->absoluteTransform());
    });
    if (!parts.empty()) {
        return parts;
    }
    const AABB &box = model.model().aabb();
    if (box.isDegenerate()) {
        return {};
    }
    return {jaUprightBody(box, model.absoluteTransform())};
}

namespace {

// The blade of a saber mesh whose box \p meshToWeapon carries into the
// weapon's space, where the blade runs along +Z; none under 1 cm long.
std::optional<JaBlade> bladeOfMesh(const AABB &box, const glm::mat4 &meshToWeapon) {
    if (box.isDegenerate()) {
        return std::nullopt;
    }
    glm::vec3 min(std::numeric_limits<float>::max());
    glm::vec3 max(std::numeric_limits<float>::lowest());
    for (auto &corner : box.corners()) {
        glm::vec3 point(meshToWeapon * glm::vec4(corner, 1.0f));
        min = glm::min(min, point);
        max = glm::max(max, point);
    }
    glm::vec2 center(0.5f * (min.x + max.x), 0.5f * (min.y + max.y));
    JaBlade blade;
    blade.base = glm::vec3(center, min.z);
    blade.tip = glm::vec3(center, max.z);
    if (glm::length(blade.tip - blade.base) < 0.01f) {
        return std::nullopt;
    }
    return blade;
}

// Adds \p blade unless one already there lies on it: KOTOR's single saber
// draws its blade as two crossed planes, two saber meshes along one line.
void addBlade(std::vector<JaBlade> &blades, const JaBlade &blade) {
    constexpr float kSameBlade = 0.02f;
    for (auto &other : blades) {
        if (glm::distance(other.base, blade.base) < kSameBlade && glm::distance(other.tip, blade.tip) < kSameBlade) {
            return;
        }
    }
    blades.push_back(blade);
}

void forEachSaberMesh(const ModelNode &node, const std::function<void(const ModelNode &)> &visit) {
    if (node.isSaberMesh() && node.mesh()->mesh) {
        visit(node);
    }
    for (auto &child : node.children()) {
        forEachSaberMesh(*child, visit);
    }
}

} // namespace

std::vector<JaBlade> jaSaberBlades(const Model &weapon) {
    std::vector<JaBlade> blades;
    forEachSaberMesh(*weapon.rootNode(), [&](const ModelNode &mesh) {
        auto blade = bladeOfMesh(mesh.mesh()->mesh->aabb(), mesh.absoluteTransform());
        if (blade) {
            addBlade(blades, *blade);
        }
    });
    return blades;
}

std::vector<JaBlade> jaSaberBlades(ModelSceneNode &weapon, const SceneNode &hand) {
    std::vector<JaBlade> blades;
    glm::mat4 toWeapon(weapon.absoluteTransformInverse());
    glm::mat4 weaponToHand(hand.absoluteTransformInverse() * weapon.absoluteTransform());
    forEachSaberMesh(*weapon.model().rootNode(), [&](const ModelNode &mesh) {
        // As the weapon's model shows the mesh now: a blade powering up
        // grows, one powering down shrinks.
        auto sceneNode = weapon.getNodeByNumber(mesh.number());
        if (!sceneNode) {
            return;
        }
        auto blade = bladeOfMesh(mesh.mesh()->mesh->aabb(), toWeapon * sceneNode->absoluteTransform());
        if (blade) {
            addBlade(blades, jaTransformBlade(weaponToHand, *blade));
        }
    });
    return blades;
}

JaBlade jaTransformBlade(const glm::mat4 &transform, const JaBlade &blade) {
    JaBlade result;
    result.base = glm::vec3(transform * glm::vec4(blade.base, 1.0f));
    result.tip = glm::vec3(transform * glm::vec4(blade.tip, 1.0f));
    return result;
}

bool jaMoveHits(const JaSaberMove &move) {
    switch (move.kind) {
    case JaMoveKind::Attack:
        return true;
    case JaMoveKind::Transition:
        return move.spin;
    default:
        return false;
    }
}

bool jaWeaponBlocksSaber(const Item &item) {
    switch (item.baseItemType()) {
    case 3: // vibrosword
    case 5: // vibroblade
    case 7: // vibro double-blade
        return true;
    default:
        return item.isLightsaber();
    }
}

bool jaCanBlockSaber(const Creature &creature) {
    for (int slot : {InventorySlots::rightWeapon, InventorySlots::leftWeapon}) {
        auto item = creature.getEquippedItem(slot);
        if (item && jaWeaponBlocksSaber(*item)) {
            return true;
        }
    }
    return creature.hasEffect(EffectType::ForceShield);
}

bool jaSparedBySaber(const Creature &creature) {
    return creature.faction() == Faction::Friendly1 || creature.faction() == Faction::Friendly2;
}

bool jaTooBigToCut(const Creature &creature) {
    return creature.size() >= CreatureSize::Large;
}

int jaSaberHitExtraDamage(const Creature &target) {
    return jaCanBlockSaber(target) || jaTooBigToCut(target) ? 0 : std::max(0, target.currentHitPoints());
}

void JaSaberContacts::reset() {
    _blades.clear();
    _moveSerial = -1;
    _hitThisMove.clear();
}

std::vector<JaSaberHit> JaSaberContacts::update(
    int moveSerial,
    bool hits,
    const std::vector<JaBlade> &blades,
    const std::vector<JaContactTarget> &targets) {

    if (moveSerial != _moveSerial) {
        _moveSerial = moveSerial;
        _hitThisMove.clear();
    }
    bool sweep = !_blades.empty() && _blades.size() == blades.size();
    for (size_t i = 0; sweep && i < blades.size(); ++i) {
        sweep = glm::distance(_blades[i].base, blades[i].base) <= kMaxBladeJump;
    }

    std::vector<JaSaberHit> result;
    if (hits && sweep) {
        for (auto &target : targets) {
            if (_hitThisMove.count(target.id)) {
                continue;
            }
            std::optional<JaBladeContact> first;
            for (size_t i = 0; i < blades.size(); ++i) {
                auto contact = jaSweepBlade(_blades[i], blades[i], target.parts, _bladeRadius);
                if (contact && (!first || contact->time < first->time)) {
                    first = contact;
                }
            }
            if (!first) {
                continue;
            }
            JaSaberHit hit;
            hit.target = target.id;
            hit.region = target.parts[first->part].region;
            hit.point = first->point;
            result.push_back(hit);
            _hitThisMove.insert(target.id);
        }
    }
    _blades = blades;
    return result;
}

} // namespace game

} // namespace reone
