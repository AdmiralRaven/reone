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

#include "reone/system/binaryreader.h"
#include "reone/system/stream/input.h"

namespace reone {

namespace graphics {

class GhoulAnimation;

/**
 * Reads a Ghoul2 animation file (.gla, version 6), as shipped with Jedi
 * Academy. Layout, little-endian:
 *
 * - header (100 bytes): "2LGA", version, name[64], scale, numFrames,
 *   ofsFrames, numBones, ofsCompBonePool, ofsSkel, ofsEnd;
 * - one offset per bone, relative to the end of the header, to the bone's
 *   record: name[64], flags, parent, base pose and its inverse as 3x4
 *   row-major matrices, numChildren, children;
 * - at ofsFrames, a 24-bit pool index per bone per frame;
 * - from ofsCompBonePool to ofsEnd, compressed bone transforms, 14 bytes each.
 */
class GlaReader : boost::noncopyable {
public:
    static constexpr int kHeaderSize = 100;

    GlaReader(IInputStream &gla) :
        _gla(BinaryReader(gla)) {
    }

    void load();

    std::shared_ptr<GhoulAnimation> animation() const { return _animation; }

private:
    BinaryReader _gla;

    std::shared_ptr<GhoulAnimation> _animation;
};

} // namespace graphics

} // namespace reone
