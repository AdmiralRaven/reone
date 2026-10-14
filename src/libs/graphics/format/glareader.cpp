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

#include "reone/graphics/format/glareader.h"

#include "reone/graphics/ghoulanimation.h"
#include "reone/system/checkutil.h"

namespace reone {

namespace graphics {

static constexpr int kNameSize = 64;
static constexpr int kBoneRecordSize = kNameSize + 4 + 4 + 2 * 12 * 4 + 4; // up to numChildren
static constexpr int kMaxBones = 4096;

// A 3x4 row-major matrix as stored, widened to 4x4.
static glm::mat4 readMatrix34(BinaryReader &reader) {
    glm::mat4 matrix(1.0f);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            matrix[column][row] = reader.readFloat();
        }
    }
    return matrix;
}

static std::vector<uint8_t> readBlock(BinaryReader &reader, size_t offset, size_t size) {
    std::vector<uint8_t> block(size);
    if (size == 0) {
        return block;
    }
    reader.seek(offset);
    ByteBuffer bytes(reader.readBytes(static_cast<int>(size)));
    std::memcpy(block.data(), bytes.data(), size);
    return block;
}

void GlaReader::load() {
    checkEqual("GLA signature", _gla.readString(4), std::string("2LGA"));
    checkEqual("GLA version", _gla.readInt32(), 6);
    std::string name(_gla.readString(kNameSize));
    float scale = _gla.readFloat();
    int32_t numFrames = _gla.readInt32();
    int32_t ofsFrames = _gla.readInt32();
    int32_t numBones = _gla.readInt32();
    int32_t ofsCompBonePool = _gla.readInt32();
    _gla.readInt32(); // ofsSkel: the first bone record; records are found through the offsets
    int32_t ofsEnd = _gla.readInt32();

    const size_t length = _gla.length();
    checkThat(numBones >= 1 && numBones <= kMaxBones, "GLA bone count out of range");
    checkThat(numFrames >= 1, "GLA has no frames");
    checkThat(ofsEnd > 0 && static_cast<size_t>(ofsEnd) <= length, "GLA end offset outside the file");
    uint64_t frameBytes = static_cast<uint64_t>(numFrames) * numBones * GhoulAnimation::kFrameIndexSize;
    checkThat(ofsFrames >= kHeaderSize && ofsFrames + frameBytes <= static_cast<uint64_t>(ofsEnd),
              "GLA frame table outside the file");
    checkThat(ofsCompBonePool >= kHeaderSize && ofsCompBonePool <= ofsEnd, "GLA bone pool outside the file");
    checkThat(kHeaderSize + static_cast<size_t>(numBones) * 4 <= length, "GLA bone offsets outside the file");

    std::vector<GhoulAnimation::Bone> bones;
    bones.reserve(numBones);
    for (int32_t i = 0; i < numBones; ++i) {
        _gla.seek(kHeaderSize + 4 * static_cast<size_t>(i));
        int32_t offset = _gla.readInt32();
        checkThat(offset >= 0 && kHeaderSize + static_cast<size_t>(offset) + kBoneRecordSize <= length,
                  "GLA bone record outside the file");
        _gla.seek(kHeaderSize + static_cast<size_t>(offset));
        GhoulAnimation::Bone bone;
        bone.name = _gla.readString(kNameSize);
        bone.flags = _gla.readUint32();
        bone.parent = _gla.readInt32();
        bone.basePose = readMatrix34(_gla);
        bone.basePoseInverse = readMatrix34(_gla);
        bones.push_back(std::move(bone));
    }

    auto frameIndices = readBlock(_gla, ofsFrames, frameBytes);
    auto bonePool = readBlock(_gla, ofsCompBonePool, ofsEnd - ofsCompBonePool);
    try {
        _animation = std::make_shared<GhoulAnimation>(
            std::move(name),
            scale,
            numFrames,
            std::move(bones),
            std::move(frameIndices),
            std::move(bonePool));
    } catch (const std::invalid_argument &e) {
        throw ValidationException(e.what());
    }
}

} // namespace graphics

} // namespace reone
