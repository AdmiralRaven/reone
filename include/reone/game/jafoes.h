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

#include "reone/game/attack.h"
#include "reone/game/jadefense.h"
#include "reone/game/projectiles.h"

namespace reone {

namespace scene {

class ModelSceneNode;

} // namespace scene

namespace game {

class Action;
class Creature;
class Game;
class Item;
class JaSaber;
struct ServicesView;

/** What a KOTOR attack action asks of JaFoes. */
struct JaFoeAttack {
    FeatType feat {FeatType::Invalid}; /**< a combat feat, such as Power Attack or Rapid Shot */
    ProjectileAttackType projectiles {ProjectileAttackType::Basic};
    /** KOTOR's animation for one swing or volley, picked afresh for each. */
    std::function<std::string(Creature &attacker, Creature &target)> animation;
};

/**
 * The enemies of the character saber mode drives, fighting it in real time
 * rather than by KOTOR's rounds. KOTOR's AI still picks whom to attack, and
 * with what feat; an attack action aimed at the character then hands its
 * ticks here (AttackObjectAction, UseFeatAction), and:
 *
 * - an enemy with a blaster fires a volley on its own clock: KOTOR's attack
 *   animation, with its shots when weapondischarge.2da has them, each a bolt
 *   that flies at kBoltSpeed and hits whatever body it meets first; then it
 *   waits between kMinVolleyPause and kMaxVolleyPause and fires again;
 * - KOTOR's attack roll still counts: a volley's bolts aimed at the
 *   character are as many as the attacks of KOTOR's that would have hit,
 *   and the rest fly kNearMiss wide of it; a bolt that meets the character
 *   does its weapon's damage, with no further roll;
 * - an enemy with a melee weapon closes in and swings, KOTOR's animation,
 *   and its blow lands at the animation's impact if the character is still
 *   within its reach and in front of it; then it waits between
 *   kMinSwingPause and kMaxSwingPause. The roll decides whether a blow that
 *   lands hits, and a hit does KOTOR's damage for it;
 * - the character's saber defends itself, as Jedi Academy's player's does
 *   (jadefense.h): it parries the blows of armed enemies that come from the
 *   front or the sides while it stands in a wide guard (the ready stance or
 *   a parry); and a bolt that meets its guard's box, before the body, is
 *   stopped, by the rank and the bolt's direction, and sent back or
 *   absorbed. A bolt sent back hits the enemies it meets, with its own
 *   weapon's damage, as the character's attack.
 *
 * KOTOR's combat rounds still run alongside, uncounted, so that combat
 * starts and ends and each enemy's end-of-round script, KOTOR's AI, runs.
 * Undeflected bolts hit only the character, and bolts sent back only its
 * enemies, never KOTOR's friendly factions (jaSparedBySaber).
 */
class JaFoes : boost::noncopyable {
public:
    /** How fast a bolt flies, in metres a second: reone's own projectiles' speed. */
    static constexpr float kBoltSpeed = 16.0f;

    /** How thick a bolt is, as a ball, in metres. */
    static constexpr float kBoltRadius = 0.05f;

    /** How long a bolt flies before it is gone, in seconds. */
    static constexpr float kBoltLife = 3.0f;

    /** How far wide of the character, in metres, a bolt not aimed at it flies. */
    static constexpr float kNearMiss = 0.8f;

    /** The pause after a volley, in seconds, at least and at most. */
    static constexpr float kMinVolleyPause = 0.5f;
    static constexpr float kMaxVolleyPause = 1.5f;

    /** The pause after a swing, in seconds, at least and at most. */
    static constexpr float kMinSwingPause = 0.4f;
    static constexpr float kMaxSwingPause = 1.0f;

    /** How much farther than its attack range, in metres, a blow still reaches. */
    static constexpr float kReachSlack = 0.5f;

    /**
     * How far ahead the saber sees a bolt coming, in seconds: JA's scanner
     * looks 256 units out (wp_saber.cpp:7551-7779), which its blaster bolts,
     * at 2300 units a second, cross in about 0.11 s.
     */
    static constexpr float kBoltWarning = 256.0f / 2300.0f;

    /**
     * How far round a guard reaches for a blow, as the direction to the
     * attacker along the view: JA's box stands 12 units forward of the body
     * and 8 out, so a blow from the side meets it, one from behind the body.
     */
    static constexpr float kGuardArc = -0.25f;

    enum class Step {
        Continue, /**< the action goes on */
        Finished  /**< the action is done: its KOTOR round is over and the enemy between attacks */
    };

    /** What happened, for the log's summary and tests. */
    struct Counts {
        int volleys {0};
        int bolts {0};
        int aimed {0};
        int sentBack {0};
        int absorbed {0};
        int passedGuard {0};
        int hitCharacter {0};
        int hitEnemies {0};
        int swings {0};
        int blowsParried {0};
        int blowsLanded {0};
        int blowsMissed {0};
    };

    JaFoes(Game &game, ServicesView &services, JaSaber &saber);
    ~JaFoes();

    /** Whether enemies fight saber mode's character in real time; on by default. */
    bool enabled() const { return _enabled; }
    void setEnabled(bool enabled);

    /** Replaces the random numbers, for tests. */
    void setRandom(JaRandom random, JaRandomFloat randomFloat);

    /** Whether attacks on \p target go in real time: saber mode drives it, and this is enabled. */
    bool fightsInRealTime(const Creature &target) const;

    /** Whether \p attacker is in the middle of a volley or a swing, and should stand. */
    bool busy(const Creature &attacker) const;

    /**
     * One tick of an attack action of \p attacker's on \p target, which
     * fightsInRealTime: the action has brought the attacker within its
     * attack range and turned it to the target.
     */
    Step attack(const std::shared_ptr<Action> &action, Creature &attacker, Creature &target, const JaFoeAttack &kind, float dt);

    /** Moves the bolts in flight, and resolves what they meet; saber mode calls it each tick. */
    void update(float dt);

    /** Forgets every enemy's clock and every bolt, and removes their models. */
    void reset();

    size_t boltsInFlight() const { return _bolts.size(); }
    const Counts &counts() const { return _counts; }

    /** Plays a sound at \p point, by its resource name, if the game has it. */
    void playSound(const std::string &name, const glm::vec3 &point);

    /** Shows one of KOTOR's impact models at \p point, by its resource name, for as long as its impact animation plays. */
    void playFlash(const std::string &model, const glm::vec3 &point);

private:
    struct Foe {
        enum class Phase {
            Idle,
            Volley,
            Swing
        };
        Phase phase {Phase::Idle};
        std::weak_ptr<Action> action;
        float wait {0.0f};   /**< until it may attack again */
        float time {0.0f};   /**< into the volley or the swing */
        float length {0.0f}; /**< of the volley or the swing */
        std::string animation;
        // A volley's shots: when, whether aimed, and from which hand.
        struct Shot {
            float time {0.0f};
            bool aimed {false};
            bool offhand {false};
            bool fired {false};
        };
        std::vector<Shot> shots;
        // A swing's blow.
        float impact {0.0f};
        bool struck {false};
        std::shared_ptr<AttackBuffer> blow;
        uint32_t target {0};
    };

    struct Bolt {
        uint32_t shooter {0};
        std::shared_ptr<Item> weapon;
        glm::vec3 position {0.0f};
        glm::vec3 velocity {0.0f};
        float age {0.0f};
        bool sentBack {false};
        bool parryPrepared {false};
        std::shared_ptr<scene::ModelSceneNode> model;
    };

    struct Flash {
        std::shared_ptr<scene::ModelSceneNode> model;
        float left {0.0f};
    };

    Game &_game;
    ServicesView &_services;
    JaSaber &_saber;
    bool _enabled {true};
    JaRandom _random;
    JaRandomFloat _randomFloat;

    std::map<uint32_t, Foe> _foes;
    std::vector<Bolt> _bolts;
    std::vector<Flash> _flashes;
    Counts _counts;

    void startVolley(Foe &foe, Creature &attacker, Creature &target, const JaFoeAttack &kind);
    void fire(Foe &foe, const Foe::Shot &shot, Creature &attacker, Creature &target);
    void startSwing(Foe &foe, Creature &attacker, Creature &target, const JaFoeAttack &kind);
    void strike(Foe &foe, Creature &attacker, Creature &target);
    void rest(Foe &foe, Creature &attacker, float low, float high);

    /** Moves one bolt on; false when it is gone. */
    bool moveBolt(Bolt &bolt, float dt);
    void hitCharacter(Bolt &bolt, const glm::vec3 &point);
    void sendBack(Bolt &bolt, const glm::vec3 &point);
    void absorb(Bolt &bolt, const glm::vec3 &point);
    void removeBolt(Bolt &bolt);
    void showBolt(Bolt &bolt);

    float timeNow() const;
    void log(const std::string &line);
};

} // namespace game

} // namespace reone
