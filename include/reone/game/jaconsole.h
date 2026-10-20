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

#include <memory>
#include <string>

namespace reone {

namespace game {

class Creature;
class Game;
class IConsole;
class Item;

/**
 * Developer console commands that play Jedi Academy's animations on KOTOR
 * characters, read from the player's own Jedi Academy files:
 *
 * - jaload <folder>: read _humanoid.gla and animation.cfg from a folder;
 * - jaloadsave [number]: load the saved game listgames shows with that
 *   number, or the last one it shows;
 * - japlay swing|block|overhead|guard|stance|<clip> [speed]: play a move
 *   (see jaNamedMove) or any clip on the selected creature, or the party
 *   leader;
 * - jasaber [off]: put a lightsaber in its right hand, if it has none, and
 *   ignite it (or put it out);
 * - jacam [distance] [angle] [height]: aim the free camera at it;
 * - jalist [text]: list the clips whose names contain the text;
 * - jasabermode [on|off]: start or end saber mode (see JaSaber), as X does;
 * - jainput [attack] [forward|back] [left|right], or jainput off: hold that
 *   input in saber mode in place of the keyboard and mouse, for scripted runs;
 * - jaengage [on|off]: in saber mode, turn to the nearest living enemy the
 *   party leader sees, run at it and swing at it until none is left in
 *   sight, for scripted runs (see JaSaber::setEngage); off logs the enemies
 *   then left in sight (JaSaber::enemiesInSight), or none;
 * - jaturn <degrees>: turn the camera, and in saber mode the character, to
 *   the left;
 * - jamouse [radians]: show or set how far saber mode turns per unit of
 *   mouse movement.
 *
 * And, for scripted checks of KOTOR's own play:
 *
 * - jastatus <label> [item tag]: log jaLeaderStatus for the party leader;
 * - jacreature <tag>: log whether each creature with the tag in the area is
 *   alive or dead, its hit points and how far it is from the party leader,
 *   or that none has the tag;
 * - jafight: attack the nearest living enemy the party leader sees, as a
 *   click on it does, unless the leader is attacking a living enemy already;
 * - jause <item tag>: use a healing item on the party leader, as
 *   jaUseHealingItem does;
 * - jahitpoints <points>: set the party leader's hit points, without the
 *   scripts damage would run.
 *
 * The JA animations are those of Game::jaSaber()'s library.
 */
void registerJaConsoleCommands(Game &game, IConsole &console);

/**
 * One line on a party leader for scripted checks: its hit points, whether it
 * is in combat (the HUD shows its combat panel exactly while the leader is),
 * whether it can move, and, given an item tag, how many of the healing items
 * with that tag the action bar offers it. For example: "jastatus: calm: player
 * hit points 4 of 12, combat off, movement free, items g_i_medeqpmnt01 2".
 */
std::string jaLeaderStatus(const Creature &leader, const std::string &label, const std::string &itemTag);

/**
 * Queues the use of the creature's healing item with the tag on the creature
 * itself, as a click on the action bar's healing slot does, and returns the
 * item; or returns null when none of its healing items has the tag.
 */
std::shared_ptr<Item> jaUseHealingItem(Game &game, const std::shared_ptr<Creature> &creature, const std::string &itemTag);

} // namespace game

} // namespace reone
