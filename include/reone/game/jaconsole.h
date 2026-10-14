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

namespace reone {

namespace game {

class Game;
class IConsole;

/**
 * Developer console commands that play Jedi Academy's animations on KOTOR
 * characters, read from the player's own Jedi Academy files:
 *
 * - jaload <folder>: read _humanoid.gla and animation.cfg from a folder;
 * - japlay swing|block|overhead|guard|stance|<clip> [speed]: play a move
 *   (see jaNamedMove) or any clip on the selected creature, or the party
 *   leader;
 * - jasaber [off]: put a lightsaber in its right hand, if it has none, and
 *   ignite it (or put it out);
 * - jacam [distance] [angle] [height]: aim the free camera at it;
 * - jalist [text]: list the clips whose names contain the text.
 */
void registerJaConsoleCommands(Game &game, IConsole &console);

} // namespace game

} // namespace reone
