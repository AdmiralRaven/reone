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

#include "reone/game/jacontact.h"
#include "reone/game/jasabermoves.h"

namespace reone {

namespace game {

/*
 * Saber mode's automatic defense, after Jedi Academy single-player's
 * (OpenJK 260c59c, as #34's specification traces it): which blows and bolts
 * the saber stops, how it parries, and where a bolt it sends back goes. The
 * rules are written anew from that code, which is GPL-2.0-only.
 *
 * Distances are in metres; JA's own, in its units, are scaled by the body's
 * metres per unit (JaRetargeter::scale). Facings are reone's: facing f looks
 * along (-sin f, cos f), its right is (cos f, sin f).
 */

/** Random whole numbers from \p low to \p high, both included, as Q_irand draws them. */
using JaRandom = std::function<int(int low, int high)>;

/** Random numbers from \p low to \p high, as Q_flrand draws them. */
using JaRandomFloat = std::function<float(float low, float high)>;

/** reone's own random numbers, for play. */
JaRandom jaGameRandom();
JaRandomFloat jaGameRandomFloat();

/** The highest saber defense rank, FORCE_LEVEL_3. */
constexpr int kJaMaxDefense = 3;

/**
 * The defense rank saber mode starts with: Jedi Academy's middle one. In JA
 * it is the Force power Saber Defense, which its player raises from 1 to 3;
 * KOTOR's progression may set it later (#21).
 */
constexpr int kJaDefaultDefense = 2;

/** Facing \p facing, the direction ahead, level. */
glm::vec2 jaAhead(float facing);

/** Facing \p facing, the direction to the right, level. */
glm::vec2 jaRight(float facing);

/**
 * Whether \p point is in front of someone at \p from facing \p facing, as
 * InFront has it for bolts (NPC_senses.cpp:108-123): level, the direction to
 * it more than \p minDot along the facing.
 */
bool jaInFrontLevel(const glm::vec3 &point, const glm::vec3 &from, float facing, float minDot);

/**
 * Whether \p point is in front as the bolt scanner has it
 * (wp_saber.cpp:7551-7779): the direction to it, its height kept, at least
 * \p minDot along the facing.
 */
bool jaInFront(const glm::vec3 &point, const glm::vec3 &from, float facing, float minDot);

/** How far in front a bolt is for the saber to block it, SABER_REFLECT_MISSILE_CONE. */
constexpr float kJaReflectCone = 0.2f;

/**
 * Where a saber blocks an impact at \p impact, for a defender whose eyes are
 * at \p eye, looking along \p facing, at \p metresPerUnit (jaBlockFor).
 */
JaBlock jaBlockAt(const glm::vec3 &impact, const glm::vec3 &eye, float facing, float metresPerUnit);

/**
 * How the saber's box stands in a move (bg_panimate.cpp:127-322, wp_saber.cpp:7895-8091):
 * wide in the ready stance and a parry, around the body; tight in an attack,
 * a start or a return, around the blade; none in a transition, a spin among
 * them.
 */
enum class JaGuard {
    None,
    Tight,
    Wide
};

JaGuard jaGuardOf(const JaSaberMove &move);

/** How far, in JA units, a wide guard's box reaches past the body, and how far forward it stands (wp_saber.cpp:7895-8091). */
constexpr float kJaWideGuardPadding = 8.0f;
constexpr float kJaWideGuardForward = 12.0f;

/** How thick a tight guard's box is around the blade, at least, in JA units: half-extents of 16. */
constexpr float kJaTightGuardRadius = 16.0f;

/** A wide guard's box: the body's parts grown by kJaWideGuardPadding and moved kJaWideGuardForward along \p facing. */
std::vector<JaBodyPart> jaWideGuard(const std::vector<JaBodyPart> &body, float facing, float metresPerUnit);

/** A tight guard's box: each blade as a capsule kJaTightGuardRadius thick. */
std::vector<JaBodyPart> jaTightGuard(const std::vector<JaBlade> &blades, float metresPerUnit);

/** Where a moving ball first touches one of a set of capsules. */
struct JaPathContact {
    size_t part {0};        /**< which of them */
    float time {0.0f};      /**< the share of the way, from 0 at the start to 1 at the end */
    glm::vec3 point {0.0f}; /**< where the ball's centre is then */
};

/**
 * The first contact of a ball of \p radius moving straight from \p from to
 * \p to with any of \p parts, or none.
 */
std::optional<JaPathContact> jaSweepBall(
    const glm::vec3 &from,
    const glm::vec3 &to,
    float radius,
    const std::vector<JaBodyPart> &parts);

/**
 * Whether a bolt that reached the saber's box goes through it, to whatever
 * lies behind (G_RunMissile, g_missile.cpp:1355-1375): when it isn't in front
 * of the defender (kJaReflectCone), and otherwise one time in d²+1 at
 * defense rank d, by a draw from 0 to d² that comes up 0.
 */
bool jaBoltPassesGuard(int defense, bool inFront, const JaRandom &random);

/**
 * Whether a bolt the saber stopped goes back out, rather than being absorbed
 * (G_MissileImpact, g_missile.cpp:818-839): by a draw from 0 to 1, 3 or 10 at
 * defense rank 1, 2 or 3 that doesn't come up 0; never at rank 0.
 */
bool jaBoltSentBack(int defense, const JaRandom &random);

/**
 * How long, in milliseconds, after the saber meets a bolt before it can meet
 * the next with a parry of its own: 300, 150 or 50 at rank 1, 2 or 3
 * (parryDebounce, wp_saber.cpp:323-329). That's the wait for the parry, not
 * for the guard: a bolt still meets the box meanwhile.
 */
int jaBoltParryDebounceMs(int defense);

/**
 * How long, in milliseconds, a plain parry of a blow keeps the player from
 * attacking: 500 or 300 at rank 0 or 1, none higher (PM_SaberBlocking,
 * bg_pmove.cpp:11622-11629).
 */
int jaParryWeaponTimeMs(int defense);

/** Which way a parry or a move the defender is in sends a bolt off its line (G_ReflectMissile, g_missile.cpp:147-292). */
enum class JaReflectSteadiness {
    Steady,   /**< a parry, or the ready stance */
    Swinging, /**< a start, an attack, a transition or a return */
    Other     /**< any other move, as a bounce or a knockaway, none of which saber mode has yet */
};

JaReflectSteadiness jaReflectSteadiness(const JaSaberMove &move);

/** What a bolt sent back aims at. */
struct JaReflectTargets {
    /** The defender's current enemy's head, if it has one: aimed at three times in four. */
    std::optional<glm::vec3> currentEnemyHead;
    /** Another enemy's head, in the defender's view, if any: aimed at otherwise. */
    std::optional<glm::vec3> otherEnemyHead;
    /** Where the bolt's shooter stands, at the height of a JA body's origin: the fallback. */
    glm::vec3 shooter {0.0f};
};

/**
 * Where a bolt the saber sends back from \p bolt goes, a unit direction
 * (G_ReflectMissile): at defense rank 3 always, and at rank 2 one time in
 * four, at an enemy's head, offset by up to 4 units across and 16 down or 4
 * up, and the wilder the less steady the defender's move; else back at its
 * shooter, 0.4 off at random at rank 1 and 0.2 higher, wilder still in an
 * attack.
 */
glm::vec3 jaReflectDirection(
    const glm::vec3 &bolt,
    const JaReflectTargets &targets,
    int defense,
    JaReflectSteadiness steadiness,
    float metresPerUnit,
    const JaRandom &random,
    const JaRandomFloat &randomFloat);

} // namespace game

} // namespace reone
