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

#include "reone/input/event.h"

#include "jaanimator.h"
#include "jacamera.h"
#include "jacontact.h"
#include "jadefense.h"
#include "jafoes.h"
#include "jasabermoves.h"

namespace reone {

namespace scene {

class ModelSceneNode;

} // namespace scene

namespace game {

class Creature;
class Game;
class IConsole;
class Module;
class ThirdPersonCamera;
struct ServicesView;

/** Jedi Academy's animations from the player's own files, and the KOTOR bodies they pose. */
class JaLibrary : boost::noncopyable {
public:
    struct Body {
        std::unique_ptr<JaRetargeter> retargeter;
        std::unique_ptr<JaSequencer> sequencer;
        std::map<std::string, std::shared_ptr<graphics::Animation>> animations; /**< built for japlay, by name */
        std::unique_ptr<JaLivePose> livePose;                                   /**< saber mode's */
    };

    /**
     * Loads _humanoid.gla and animation.cfg from a folder, or a GLA file and
     * the animation.cfg beside it. Bodies made before are dropped.
     *
     * \throws std::runtime_error when a file is missing or unreadable
     */
    void load(const std::filesystem::path &folderOrGla);

    bool isLoaded() const { return static_cast<bool>(_gla); }

    /** \throws std::runtime_error before load() */
    const graphics::GhoulAnimation &gla() const;

    const std::map<std::string, graphics::GhoulAnimationClip> &clips() const { return _clips; }

    /**
     * The body for a KOTOR model, made on first use.
     *
     * \throws std::runtime_error before load(), or when the model cannot take JA animations
     */
    Body &body(const graphics::Model &model);

    /**
     * Keeps an animation for as long as the game runs: model scene nodes keep
     * plain pointers to the animations they play.
     */
    void keepAlive(std::shared_ptr<graphics::Animation> animation);

private:
    std::shared_ptr<graphics::GhoulAnimation> _gla;
    std::map<std::string, graphics::GhoulAnimationClip> _clips;
    std::map<std::string, Body> _bodies;
    std::vector<std::shared_ptr<graphics::Animation>> _keptAlive;
};

/**
 * The saber's defense rank from \p creature's feats: KOTOR's own chain for
 * deflecting blaster bolts with a lightsaber, which Jedi classes are given
 * as they rise. 1 with Jedi Defense (feat 55, a Jedi's from 1st level), 2
 * with Advanced Jedi Defense (feat 1, level 4), 3 with Master Jedi Defense
 * (feat 24, level 8): the highest it has. 0 without any.
 */
int jaDefenseFromFeats(const Creature &creature);

/**
 * Saber mode: with the lightsaber drawn, the player's character fights the
 * way Jedi Academy's single-player player does, in the fast style.
 *
 * - The character stands in the fast ready stance and faces where the camera
 *   looks. Moving the mouse sideways turns the view; moving it forward and
 *   back tilts it, as far as JA lets its player look, 75 degrees up or down.
 * - The camera is Jedi Academy's third-person camera (JaCamera), scaled to
 *   the character, as far behind as KOTOR's own camera stands in the area.
 *   V switches it to a view over the right shoulder, and back; the mouse
 *   wheel brings both nearer or farther.
 * - W, S, A and D (or Z and C) move it forward, back and sideways, and B or
 *   Shift walks, with Jedi Academy's saber-out walks and runs. Moving
 *   sideways, the legs turn toward the movement while the torso keeps
 *   facing the camera.
 * - The left mouse button attacks; the movement keys held pick the attack,
 *   and holding the button chains attacks, by JaSaberStateMachine's rules.
 *   The character stands still while a move plays; attacking on the move is
 *   not done yet.
 * - X draws the saber into saber mode and puts it away again; KOTOR's own
 *   controls and cursor then come back. Saber mode also ends with a
 *   conversation, before its first line plays; with another camera; with a
 *   screen other than the game, its in-game menu and the save/load screen,
 *   which only pause it; and with a new area, another party leader or the
 *   character's death.
 *
 * The saber stays lit in saber mode, as in Jedi Academy, through the end of
 * KOTOR's combat (Creature::setLightsabersOn). Its blade hits the hostile
 * creatures it touches during an attack or a spin (JaSaberContacts), once a
 * move, but never those of KOTOR's friendly factions (jaSparedBySaber),
 * through KOTOR's own damage: a creature that can't block a lightsaber
 * (jaCanBlockSaber) dies to the hit, and one that can, or is too big to cut
 * down (jaTooBigToCut), takes the weapon's damage for an ordinary hit, with
 * no attack roll, and flinches. The blade is the character's only attack:
 * KOTOR's own are dropped. Each attack and spin plays one of KOTOR's
 * lightsaber swings, and each hit its lightsaber hit, or its clash on a
 * creature that can block.
 *
 * The character's enemies fight it in real time meanwhile (JaFoes), and its
 * saber defends it as Jedi Academy's player's does, at a defense rank from 0
 * to 3, from KOTOR's Jedi Defense feats (jaDefenseFromFeats): it parries a
 * bolt it sees coming and the blows of armed enemies, and stops bolts that
 * meet its guard, sending some back.
 */
class JaSaber : boost::noncopyable {
public:
    /**
     * Radians the view turns, and tilts, per unit of mouse movement: JA's
     * 0.022 degrees (m_yaw and m_pitch) at its default sensitivity, 5.
     * reone's own mouse look turns 0.001.
     */
    static constexpr float kDefaultMouseTurn = 0.0019f;

    /** How much nearer or farther one notch of the mouse wheel brings the camera. */
    static constexpr float kWheelZoom = 1.1f;

    /** The camera's range as a share of KOTOR's camera distance, at most and at least. */
    static constexpr float kMinZoom = 0.5f;
    static constexpr float kMaxZoom = 2.0f;

    /** How far from the character, in metres, a creature is looked at for hits. */
    static constexpr float kHitReach = 4.0f;

    JaSaber(Game &game, ServicesView &services, IConsole &console) :
        _game(game),
        _services(services),
        _console(console),
        _foes(game, services, *this) {
    }

    JaLibrary &library() { return _library; }

    bool isActive() const { return _active; }

    /**
     * Starts saber mode on the party leader: lights the saber in its right
     * hand and blends into the ready stance.
     *
     * \throws std::runtime_error saying why saber mode cannot start
     */
    void enter();

    /**
     * Ends saber mode: puts the saber away and hands the character back to
     * KOTOR's controls and animations. The mouse is let go only while the
     * game is still in third-person play; anything that took over, such as
     * the first-person camera, keeps the mouse as it set it.
     *
     * \param reason why, printed first when given
     */
    void leave(const std::string &reason = std::string());

    /**
     * Takes the input saber mode uses, in the game with the third-person
     * camera; X starts saber mode when the leader holds a lightsaber and JA's
     * animations are loaded.
     *
     * \return true when saber mode took the event
     */
    bool handle(const input::Event &event);

    /**
     * Drives the party leader for one module update.
     *
     * \return false when saber mode is off, for the player's own controls
     */
    bool update(float dt);

    /**
     * Every frame, before the module updates, whatever the camera: ends saber
     * mode when the game has left it, and otherwise keeps the mouse captured
     * only while the game itself is on screen, not its menus.
     */
    void updateFrame();

    /**
     * Scripted input for unattended runs: what the player holds, in place of
     * the keyboard and mouse, until set to nothing. It ends engaging.
     */
    void setScriptedInput(std::optional<JaSaberInput> input) {
        _scripted = std::move(input);
        _engage = false;
    }

    /** How near, in metres, engaging comes before it swings. */
    static constexpr float kEngageRange = 1.1f;

    /**
     * For unattended runs: every tick, the camera, and so the character,
     * turns to the nearest of enemiesInSight(); the character runs at it
     * until within kEngageRange, then holds the attack forward and right,
     * chaining swings. When none is in sight it holds nothing, and goes for
     * the next that comes into sight. Off ends it and lets go of the input.
     */
    void setEngage(bool engage);
    bool engaging() const { return _engage; }

    /**
     * The living enemies \p creature sees that the blade may hit, nearest
     * first: hostile to the party leader but not of KOTOR's friendly factions
     * (jaSparedBySaber), and seen by the creature's own perception and in a
     * line of sight, as jafight finds them.
     */
    std::vector<std::shared_ptr<Creature>> enemiesInSight(const Creature &creature) const;

    /** Turns the camera, and so the character, by \p radians, positive to the left. */
    void turn(float radians);

    /** How far the view tilts, in radians; positive looks down. */
    float pitch() const { return _pitch; }

    /** Tilts the view by \p radians, positive down, within JA's 75 degrees either way. */
    void look(float radians);

    /** Whether the camera looks over the right shoulder rather than from straight behind. */
    bool shoulderView() const { return _shoulder; }

    /** Puts the camera over the right shoulder, or back behind; kept for the next time saber mode starts. */
    void setShoulderView(bool shoulder);

    /** Saber mode's camera; none while saber mode is off. */
    const JaCamera *camera() const { return _view ? &*_view : nullptr; }

    /**
     * How far the camera stands behind its aim point in the view behind, in
     * metres: KOTOR's own camera distance in the area, times the zoom. The
     * view over the shoulder stands at JA's share of it, 56 to 80. 0 while
     * saber mode is off.
     */
    float cameraRange() const;

    /** KOTOR's own camera distance in the area, in metres; JA's range for the character without one. */
    float kotorCameraDistance() const;

    /** The camera's range as a share of KOTOR's camera distance; kept for the next time saber mode starts. */
    float zoom() const { return _zoom; }

    /** Sets the zoom, within kMinZoom and kMaxZoom, and prints the range it gives. */
    void setZoom(float zoom);

    float mouseTurn() const { return _mouseTurn; }
    void setMouseTurn(float radiansPerUnit) { _mouseTurn = radiansPerUnit; }

    /** The move playing, by OpenJK's name; empty when saber mode is off. */
    std::string moveName() const;

    /** The blades as the pose shown holds them, in world space; none while saber mode is off. */
    std::vector<JaBlade> blades() const;

    /** How many hits the blade has made since saber mode last started. */
    int hitCount() const { return _hitCount; }

    /** How thick the blades cut, in metres: kJaBladeTraceUnits at the body's scale. */
    float bladeRadius() const { return _contacts.bladeRadius(); }

    // Defense, and the enemies that fight in real time

    /** The enemies fighting the character in real time. */
    JaFoes &foes() { return _foes; }
    const JaFoes &foes() const { return _foes; }

    /** The character saber mode drives; none while it is off. */
    std::shared_ptr<Creature> character() const;

    /** Seconds since saber mode last started. */
    float modeTime() const { return _modeTime; }

    /** Where the view looks, as a facing: where the guard faces. */
    float viewFacing() const;

    /** The move playing; none while saber mode is off. */
    const JaSaberMove *currentMove() const;

    /** Metres per JA unit at the character's scale; JA's own rules' distances are scaled by it. */
    float metresPerUnit() const;

    /** Where the character's eyes are, in world space. */
    glm::vec3 eye() const;

    /** The character's body as its model was last posed (jaBodyParts). */
    std::vector<JaBodyPart> bodyParts() const;

    /** How the saber's box stands now: none while the saber shows no blade. */
    JaGuard guard() const;

    /**
     * The saber's defense rank, from 0, no defense at all, to kJaMaxDefense:
     * the one set by setDefense, if any, or else the character's, by its
     * feats (jaDefenseFromFeats), the party leader's while saber mode is off.
     */
    int defense() const;

    /** Sets the rank by hand, whatever the feats, as for a scripted check. */
    void setDefense(int defense);

    /** Leaves the rank to the character's feats again. */
    void setDefenseFromFeats();

    /** Whether the rank comes from the character's feats, not setDefense. */
    bool defenseFromFeats() const { return !_defenseSet.has_value(); }

    /**
     * The saber's answer to a bolt coming at \p boltPosition, as JA's bolt
     * scanner gives it (WP_SaberStartMissileBlockCheck, wp_saber.cpp:7392-7839):
     * a bolt's parry for where it is, from the ready stance or a parry, with
     * the saber lit, the attack button up, a defense rank and the last
     * bolt's parry wait (jaBoltParryDebounceMs) over.
     *
     * \return whether a parry starts
     */
    bool parryBolt(const glm::vec3 &boltPosition);

    /**
     * Whether the saber parries a blow from \p attacker meeting the
     * character at \p point: in a wide guard, with a defense rank, from in
     * front or the sides (JaFoes::kGuardArc). It plays the blow's parry.
     */
    bool parryBlow(const Creature &attacker, const glm::vec3 &point);

    /** How many parries, of bolts and of blows, have started since saber mode last started. */
    int parryCount() const { return _parryCount; }

private:
    struct Keys {
        bool forward {false};
        bool back {false};
        bool leftA {false};
        bool leftZ {false};
        bool rightD {false};
        bool rightC {false};
        bool walk {false};
        bool shiftWalk {false};
        bool attack {false};
    };

    Game &_game;
    ServicesView &_services;
    IConsole &_console;
    JaLibrary _library;

    bool _active {false};
    bool _capturedMouse {false};
    std::weak_ptr<Creature> _creature;
    std::weak_ptr<Module> _module;
    scene::ModelSceneNode *_model {nullptr};
    JaLibrary::Body *_body {nullptr};
    std::unique_ptr<JaSaberDriver> _driver;

    Keys _keys;
    std::optional<JaSaberInput> _scripted;
    float _mouseTurn {kDefaultMouseTurn};
    float _modeTime {0.0f};
    float _pitch {0.0f};
    bool _shoulder {false};
    float _zoom {1.0f};
    std::optional<JaCamera> _view;

    JaSaberContacts _contacts;
    int _moveSerial {0}; /**< counts the moves started, so each move hits a creature once */
    int _hitCount {0};
    int _litBlades {-1}; /**< how many blades were lit on the last tick, for the log */
    bool _engage {false};
    uint32_t _engaged {0}; /**< the creature engaging goes for */

    std::optional<int> _defenseSet; /**< the rank setDefense gave, if any */
    float _boltParryReady {0.0f}; /**< the mode time from which the saber parries the next bolt */
    int _parryCount {0};
    JaFoes _foes;

    JaSaberInput input() const;
    bool walking() const;
    bool handleKey(const input::KeyEvent &event);
    /** Why saber mode must end; empty when it can go on, in play or paused by a menu. */
    std::string whyNotInPlay(const std::shared_ptr<Creature> &creature) const;
    void showPose();
    /** Finds what the blades hit this tick, and hits it. */
    void updateContacts(const std::shared_ptr<Creature> &creature);
    /** Turns to the nearest enemy and sets the input to go for it, for setEngage. */
    void engage(const Creature &creature);
    /**
     * Drops the attacks of KOTOR's own from \p creature's actions, so that
     * they and the blade can't both hit: one a combat round queues for the
     * party leader when it ends, and one whose swing has begun, which locks
     * its action against an ordinary clear and is cancelled by force, its
     * blow discarded. Returns whether it dropped any.
     */
    bool dropKotorAttacks(Creature &creature);
    /** Applies one of the blade's hits on \p target. */
    void strike(Creature &attacker, Creature &target, const JaSaberHit &hit);
    /** Plays one of KOTOR's lightsaber sounds, weaponsounds.2da's, at the character. */
    void saberSound(const std::vector<std::string> &names, const glm::vec3 &point);
    /** Moves saber mode's camera on by \p dt and shows its view. */
    void updateCamera(float dt);
    /** The third-person camera of the area saber mode started in. */
    ThirdPersonCamera *thirdPersonCamera() const;

    void print(const std::string &line);
};

} // namespace game

} // namespace reone
