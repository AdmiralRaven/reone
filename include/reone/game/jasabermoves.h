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

#include "reone/graphics/ghoulanimation.h"

namespace reone {

namespace game {

/*
 * Jedi Academy's single-player lightsaber rules for the ordinary case: the
 * player on the ground, the fast style, one saber, no saber contact, no Force
 * powers and no special attacks. They decide which move plays, when, and with
 * which clip, speed and blend.
 *
 * The rules follow OpenJK's single-player game code at commit 260c59c
 * (code/game/ and code/cgame/), cited per rule. They are written anew from
 * those rules: OpenJK is licensed GPL-2.0-only, so none of its code is here.
 */

/** Where the blade is, in Jedi Academy's own order (wp_saber.h:440-450). */
enum class JaQuad {
    BR,
    R,
    TR,
    T,
    TL,
    L,
    BL,
    B
};

enum class JaMoveKind {
    Ready,     /**< the ready stance, LS_READY */
    Start,     /**< from ready to an attack's first pose, LS_S_* */
    Attack,    /**< LS_A_* */
    Return,    /**< from an attack's last pose back to ready, LS_R_* */
    Transition /**< between attacks, from one quadrant to another, LS_T1_* */
};

/** How a move's clip claims the body (bg_panimate.cpp:116-119, bg_public.h:170-174). */
enum class JaMoveHold {
    Idle,   /**< no hold time; waits for a held clip to run out (AFLAG_IDLE) */
    Active, /**< cuts in at once; held for its length (AFLAG_ACTIVE) */
    Finish  /**< waits for a held clip to run out; held for its length (AFLAG_FINISH) */
};

/** A row of Jedi Academy's saber move table (bg_panimate.cpp:122-323). */
struct JaSaberMove {
    int id {-1};
    std::string name; /**< OpenJK's, such as LS_A_TL2BR */
    std::string clip; /**< animation.cfg name */
    JaMoveKind kind {JaMoveKind::Ready};
    JaQuad startQuad {JaQuad::R};
    JaQuad endQuad {JaQuad::R};
    JaMoveHold hold {JaMoveHold::Idle};
    int blendMs {0};        /**< blend into the move (bg_pmove.cpp:9525) */
    float timeScale {1.0f}; /**< 1.5 for the fast style's transitions (bg_panimate.cpp:4516-4531) */
    int chainAttack {-1};   /**< the next move when the attack button is held and nothing else is chosen */
    /**
     * A spin: one of the 12 transitions that turn the whole body around
     * (PM_SpinningSaberAnim, bg_pmove.cpp:7022-7038). Jedi Academy plays it on
     * the whole body even on the move, and slows the movement while it plays.
     */
    bool spin {false};
};

/** How a move plays its clip. */
struct JaMoveTiming {
    int durationMs {0}; /**< until the clip's last frame (bg_panimate.cpp:4721) */
    int holdMs {0};     /**< how long the move holds the body; 0 for a move without a hold (4722, 4988-4993) */
    float rate {1.0f};  /**< playing speed relative to the clip's own frame rate (4719) */
    int blendMs {0};    /**< blend into the move: the table's, or 0 for a clip played in reverse (4736-4753) */
};

/**
 * The fast style's ordinary moves: the ready stance, seven attacks with their
 * starts and returns, and the 42 transitions between attacks. The fast style
 * plays the table's own clips (bg_pmove.cpp:9389-9399), with the fast ready
 * pose (9086-9116).
 */
class JaSaberMoves : boost::noncopyable {
public:
    static constexpr int kNone = -1;

    static const JaSaberMoves &fast();

    const std::vector<JaSaberMove> &moves() const { return _moves; }
    const JaSaberMove &move(int id) const;

    /** \return the move with OpenJK's name, or kNone */
    int find(const std::string &name) const;

    int ready() const { return 0; }

    /** The attack that starts in a quadrant (PM_AttackMoveForQuad); kNone for none. */
    int attackFrom(JaQuad quad) const;

    int startOf(int attack) const;
    int returnOf(int attack) const;

    /** The attack that a start or a return belongs to. */
    int attackOf(int startOrReturn) const;

    /**
     * The transition from one quadrant to another, as the transitionMove table
     * has it (bg_panimate.cpp:326-408); kNone for none. From the bottom it
     * takes the bottom right's, except to the bottom right itself.
     */
    int transition(JaQuad from, JaQuad to) const;

    /**
     * The attack for a movement, as PM_SaberAttackForMovement chooses it for
     * an ordinary standing player with no enemy to aim at
     * (bg_panimate.cpp:3423-3995). Only the signs of \p forward and \p right
     * count. With neither, it is kNone, except from ready while looking more
     * than 30 degrees up or down (3981-3984).
     */
    int attackForMovement(int forward, int right, float pitchDegrees, int current) const;

    /**
     * What plays on the way from \p current to \p attack
     * (PM_SaberAnimTransitionMove, bg_panimate.cpp:3997-4165): from ready, the
     * attack's start; after an attack or a return, the attack itself if it
     * starts where the current move ends, else the transition there; after a
     * start or a transition, the attack itself.
     */
    int nextMove(int current, int attack) const;

private:
    std::vector<JaSaberMove> _moves;
    std::unordered_map<std::string, int> _byName;

    JaSaberMoves();
};

/** Ghoul2's milliseconds per frame for an animation.cfg rate (NPC_stats.cpp:986-1008). */
int jaFrameLerp(float fps);

/**
 * A remainder of a hold timer that counts as run out. Jedi Academy's timers
 * count whole milliseconds (PM_DropTimers, bg_pmove.cpp:14320-14361); these
 * count fractions of them, and less than this left is rounding, not time.
 */
constexpr double kJaTimerSlackMs = 0.01;

/**
 * \p timerMs counted down by \p dt seconds, never below zero. A hold of
 * exactly n ticks runs out on the n-th: 400 ms on the 24th tick at 60 Hz.
 */
double jaCountDown(double timerMs, float dt);

/** How \p move plays \p clip. */
JaMoveTiming jaMoveTiming(const JaSaberMove &move, const graphics::GhoulAnimationClip &clip);

/** What the player asks for in a tick. */
struct JaSaberInput {
    bool attack {false};
    int forward {0}; /**< above 0 forward, below 0 back */
    int right {0};   /**< above 0 right, below 0 left */
    float pitch {0.0f};
};

/**
 * Which saber move plays, tick by tick, as PM_WeaponLightsaber and
 * PM_TorsoAnimLightsaber decide it (bg_pmove.cpp:11916-12890,
 * bg_panimate.cpp:5160-5505):
 *
 * - a move holds the body for its hold time, and input counts only on the
 *   tick it runs out; holding the button is enough, and nothing is buffered;
 * - when a move ends with the button up, a start or a transition goes on to
 *   its attack, an attack to its return, and a return back to ready;
 * - with the button held, the held direction picks the next attack, through a
 *   transition when it starts elsewhere; with no direction, an attack returns;
 * - moving with the button up during a return frees the saber early, so a new
 *   attack can cut the return short (bg_pmove.cpp:12231-12244).
 *
 * The fast style never ends a chain (PM_SaberKataDone, bg_panimate.cpp:1944-2071).
 */
class JaSaberStateMachine {
public:
    /**
     * The wait after going back to ready with the button still held. Jedi
     * Academy reads it from weapons.dat, which this does not have; 100 ms is
     * this implementation's choice.
     */
    static constexpr int kFireTimeMs = 100;

    /** \throws std::runtime_error naming a move whose clip \p clips lacks */
    JaSaberStateMachine(
        const JaSaberMoves &moves,
        const std::map<std::string, graphics::GhoulAnimationClip> &clips,
        int fireTimeMs = kFireTimeMs);

    /** Advances \p dt seconds with \p input; returns the moves that started, in order. */
    std::vector<int> tick(float dt, const JaSaberInput &input);

    /** Back to ready, holding nothing. */
    void reset();

    const JaSaberMoves &moves() const { return _moves; }
    int move() const { return _move; }
    const JaMoveTiming &timing(int move) const { return _timings.at(move); }
    float holdTimerMs() const { return static_cast<float>(_torsoTimer); }
    float weaponTimeMs() const { return static_cast<float>(_weaponTime); }

private:
    const JaSaberMoves &_moves;
    std::vector<JaMoveTiming> _timings;
    int _fireTimeMs;

    int _move {0};
    double _torsoTimer {0.0}; /**< how long the current clip still holds the body, in ms */
    double _weaponTime {0.0}; /**< how long until input counts again, in ms */

    bool set(int move, std::vector<int> &started);
};

// Moving with the saber drawn

/**
 * Which way the legs face, in degrees from the view, positive to the left
 * (CG_PlayerLegsYawFromMovement, cg_players.cpp:1864-1903): toward the
 * movement, at most 60 degrees, mirrored when moving backwards; straight
 * ahead when standing still or moving straight back.
 */
float jaLegsYawTarget(int forward, int right);

/**
 * The saber-out walk or run clip for a movement (PM_Footsteps,
 * bg_pmove.cpp:8417-8595): backwards ones when moving back, the forward ones
 * otherwise, sideways included (14805-14810).
 */
std::string jaLocomotionClip(int forward, bool walk);

/**
 * How fast a walk or run clip plays for a speed, relative to its own frame
 * rate, as g_noFootSlide scales it (bg_panimate.cpp:4757-4835): the runs
 * match 150 units a second and the walks 50, and no clip plays faster than
 * 30 frames a second (1.5 in Ghoul2's units).
 */
float jaFootSlideRate(const graphics::GhoulAnimationClip &clip, bool walk, float unitsPerSecond);

} // namespace game

} // namespace reone
