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
 * Saber mode: with the lightsaber drawn, the player's character fights the
 * way Jedi Academy's single-player player does, in the fast style.
 *
 * - The character stands in the fast ready stance and faces where the camera
 *   looks; the mouse turns the camera.
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
 *   conversation, a menu screen, a new area or another party leader.
 *
 * Jedi Academy's blades pass through everything here: hits, blocking and
 * damage are not part of saber mode yet.
 */
class JaSaber : boost::noncopyable {
public:
    /** Radians the camera turns per unit of mouse movement: about Jedi Academy's default. */
    static constexpr float kDefaultMouseTurn = 0.0019f;

    JaSaber(Game &game, IConsole &console) :
        _game(game),
        _console(console) {
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

    /** Ends saber mode: puts the saber away and hands the character back to KOTOR's controls. */
    void leave();

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

    /** Every frame: the mouse stays captured only while saber mode is on and the game is in play. */
    void updateMouse();

    /**
     * Scripted input for unattended runs: what the player holds, in place of
     * the keyboard and mouse, until set to nothing.
     */
    void setScriptedInput(std::optional<JaSaberInput> input) { _scripted = std::move(input); }

    /** Turns the camera, and so the character, by \p radians, positive to the left. */
    void turn(float radians);

    float mouseTurn() const { return _mouseTurn; }
    void setMouseTurn(float radiansPerUnit) { _mouseTurn = radiansPerUnit; }

    /** The move playing, by OpenJK's name; empty when saber mode is off. */
    std::string moveName() const;

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

    JaSaberInput input() const;
    bool walking() const;
    bool handleKey(const input::KeyEvent &event);
    /** Why saber mode cannot go on; empty when it can. */
    std::string whyNotInPlay(const std::shared_ptr<Creature> &creature) const;
    void showPose();
    void print(const std::string &line);
};

} // namespace game

} // namespace reone
