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

#include "reone/game/jasabermoves.h"
#include "reone/graphics/aabb.h"

namespace reone {

namespace graphics {

class Model;

} // namespace graphics

namespace scene {

class ModelSceneNode;
class SceneNode;

} // namespace scene

namespace game {

class Creature;
class Item;

/*
 * Saber mode's blade contacts, after Jedi Academy single-player's
 * (WP_SaberDamageTrace, code/game/wp_saber.cpp at OpenJK 260c59c, as #34's
 * specification traces it). Where this differs from JA's sampled traces:
 *
 * - JA traces a few points of the blade every 50 ms of game time against
 *   each body's posed mesh; this sweeps the whole blade every tick, in steps
 *   too short for a part to pass between two, against capsules standing for
 *   the body's parts;
 * - JA adds up damage for every pass a blade spends in a body; here a move
 *   hits a creature once, and the hit's damage follows the user's rule
 *   (jaCanBlockSaber, jaTooBigToCut).
 */

/** A blade at one moment, in world space. */
struct JaBlade {
    glm::vec3 base {0.0f};
    glm::vec3 tip {0.0f};
};

/** Which part of a body a capsule stands for. */
enum class JaBodyRegion {
    Body, /**< the whole body, for a creature without a humanoid skeleton */
    Head,
    Torso,
    LeftArm,
    RightArm,
    LeftLeg,
    RightLeg
};

const char *jaBodyRegionName(JaBodyRegion region);

/** A part of a body that a blade can cut: a capsule from \p a to \p b, in world space. */
struct JaBodyPart {
    JaBodyRegion region {JaBodyRegion::Body};
    glm::vec3 a {0.0f};
    glm::vec3 b {0.0f};
    float radius {0.0f};
};

/** Where a moving blade first touches a body. */
struct JaBladeContact {
    size_t part {0};        /**< which of the parts */
    float time {0.0f};      /**< from 0 at the earlier blade to 1 at the later one */
    glm::vec3 point {0.0f}; /**< the point of the blade nearest that part */
};

/**
 * How thick the blade cuts, in JA units: the player's saber traces carry a box
 * 2 units either way (wp_saber.cpp:2487-2506), which Ghoul2's collision with a
 * body takes as a radius (sv_world.cpp:764).
 */
constexpr float kJaBladeTraceUnits = 2.0f;

/** kJaBladeTraceUnits in metres at a typical retargeting scale, for tests and defaults. */
constexpr float kJaBladeRadius = 0.05f;

/**
 * The first contact of a blade moving from \p from to \p to with any of
 * \p parts, or none. On the way the base moves straight, the blade turns at
 * an even rate and its length changes evenly, as a swing between two ticks
 * would. The blade is tested at both ends and at steps in which no point of
 * it moves farther than the thinnest part's reach, the part's radius and the
 * blade's: so no part can be cut through between two steps, and only a graze
 * in the outer 13% of a part's reach can fall between them.
 */
std::optional<JaBladeContact> jaSweepBlade(
    const JaBlade &from,
    const JaBlade &to,
    const std::vector<JaBodyPart> &parts,
    float bladeRadius = kJaBladeRadius);

/**
 * A KOTOR humanoid's body as capsules, from where its skeleton's nodes are:
 * the torso, from the pelvis to between the collars; the head, on its hook;
 * the shoulders and the hips, across; and on each side the upper arm, the
 * forearm, the hand, the thigh, the shin and, with a toe node, the foot.
 * Their radii are shares of the length from the pelvis to the head's hook,
 * fitted to the player's three KOTOR body types. \p nodePosition gives a
 * node's world position, or none for a node the model lacks; without one of
 * the skeleton's main nodes there is no humanoid body, and the result is
 * empty.
 */
std::vector<JaBodyPart> jaHumanoidParts(const std::function<std::optional<glm::vec3>(const std::string &)> &nodePosition);

/**
 * One upright capsule inside a model's bounding box \p box, as wide as its
 * narrower side, carried to world space by \p toWorld.
 */
JaBodyPart jaUprightBody(const graphics::AABB &box, const glm::mat4 &toWorld);

/** A creature's body as its model was last posed: humanoid parts, or one upright capsule. */
std::vector<JaBodyPart> jaBodyParts(scene::ModelSceneNode &model);

/**
 * The blades of a lightsaber model, in the model's own space, as its saber
 * meshes stand at rest: each from one end of the mesh's bounding box along
 * the blade, the weapon's +Z, to the other. Meshes that lie on one blade, as
 * the two crossed planes of KOTOR's single saber do, give one blade; a blade
 * under 1 cm long is left out.
 */
std::vector<JaBlade> jaSaberBlades(const graphics::Model &weapon);

/**
 * The blades of a lightsaber as its model shows them now, in the space of
 * \p hand, the node the weapon hangs from: as jaSaberBlades(model), with the
 * meshes where the model's animation puts them, so a blade powering down,
 * shrunk under 1 cm, is left out.
 */
std::vector<JaBlade> jaSaberBlades(scene::ModelSceneNode &weapon, const scene::SceneNode &hand);

/** \p blade carried by \p transform. */
JaBlade jaTransformBlade(const glm::mat4 &transform, const JaBlade &blade);

/**
 * Whether a move's blade hits, as in Jedi Academy's fast style at its default
 * settings: an attack or a spin does; the ready stance, starts, returns and
 * other transitions pass through, as their damage is none there
 * (wp_saber.cpp:4645-4787, 1448-1451).
 */
bool jaMoveHits(const JaSaberMove &move);

/**
 * Whether \p item stands up to a lightsaber, by the user's rule (2026-10-19:
 * "a vibroblade, a lightsaber, or the glowy shield things"; 2026-10-20:
 * stun batons and staffs are cut through): KOTOR's vibro weapons, rows 3, 5
 * and 7 of baseitems.2da (the vibrosword, the vibroblade and the vibro
 * double-blade), and its lightsabers, rows 8 to 10, as Item::isLightsaber has
 * them. A lightsaber cuts through every other weapon: the quarterstaff, the
 * stun baton, the plain swords, the gaffi stick, the Wookiee warblade and the
 * Gamorrean battleaxe.
 */
bool jaWeaponBlocksSaber(const Item &item);

/**
 * Whether \p creature could block a lightsaber, by the user's rule: it holds
 * a weapon that stands up to one (jaWeaponBlocksSaber) in either hand, or has
 * an energy shield up. One that can't dies to a saber's hit; one that can
 * takes KOTOR's damage for the hit, its hit points standing in for stamina.
 */
bool jaCanBlockSaber(const Creature &creature);

/**
 * Whether the blade passes through \p creature though KOTOR's factions make
 * it hostile to the party leader: it is of one of KOTOR's two friendly
 * standard factions, friendly_1 or friendly_2. KOTOR's own repute.2da makes
 * those two enemies of each other, for staged scenes such as the Endar
 * Spire's, where Sith in friendly_1 fight Republic soldiers in friendly_2.
 * By the user's rule (2026-10-20), the blade must not kill a Republic
 * soldier, nor anyone outside a fight; those KOTOR's factions don't make
 * hostile it never hits. KOTOR also puts in friendly_2 the Sith of two
 * staged fights on the Endar Spire, its Sith apprentice and Darth Bandon,
 * whom the blade would pass through too.
 */
bool jaSparedBySaber(const Creature &creature);

/**
 * Whether \p creature is too big for a lightsaber to cut down at one hit, by
 * the user's answer of 2026-10-20: KOTOR's large and huge creatures, by
 * appearance.2da's size category, such as the bantha, the rancor, the
 * terentatek, the krayt dragon and the sea beast. A hit takes the weapon's
 * damage off their hit points, a health bar, as for those that can block:
 * not capped as Jedi Academy caps its rancor's (his choice).
 */
bool jaTooBigToCut(const Creature &creature);

/**
 * What a saber's hit on \p target adds to the weapon's damage, by the same
 * rule: the target's hit points when it can't block and isn't too big to
 * cut, so the hit kills unless its resistances or a story's protection stop
 * it; none otherwise.
 */
int jaSaberHitExtraDamage(const Creature &target);

/** A creature a blade can hit: its object ID, and its body. */
struct JaContactTarget {
    uint32_t id {0};
    std::vector<JaBodyPart> parts;
};

/** A blade's hit on a creature. */
struct JaSaberHit {
    uint32_t target {0};
    JaBodyRegion region {JaBodyRegion::Body};
    glm::vec3 point {0.0f};
};

/**
 * Which creatures a character's blades hit, tick by tick:
 *
 * - each blade sweeps from where it was on the previous tick to where it is
 *   now; after a reset, a change in the number of blades or a base that
 *   jumped farther than kMaxBladeJump, as when the character is moved
 *   somewhere else at once, it starts over without sweeping;
 * - only a move that hits (jaMoveHits) hits anything;
 * - one move hits a creature at most once, with all its blades; the next
 *   move can hit it again.
 */
class JaSaberContacts {
public:
    /** How far, in metres, a blade's base may move in one tick and still sweep. */
    static constexpr float kMaxBladeJump = 2.5f;

    /** How thick the blades cut, in metres. */
    float bladeRadius() const { return _bladeRadius; }
    void setBladeRadius(float radius) { _bladeRadius = radius; }

    /** Forgets the blades and the creatures hit. */
    void reset();

    /**
     * \param moveSerial the move playing: a new number for every move started
     * \param hits whether that move hits (jaMoveHits)
     * \param blades the blades now, in world space
     * \param targets the creatures the blades may hit
     * \return the creatures hit this tick
     */
    std::vector<JaSaberHit> update(
        int moveSerial,
        bool hits,
        const std::vector<JaBlade> &blades,
        const std::vector<JaContactTarget> &targets);

private:
    float _bladeRadius {kJaBladeRadius};
    std::vector<JaBlade> _blades;
    int _moveSerial {-1};
    std::set<uint32_t> _hitThisMove;
};

} // namespace game

} // namespace reone
