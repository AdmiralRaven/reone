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

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace reone {

namespace test {

/**
 * Writes small Ghoul2 animation files (.gla, version 6) for tests, following
 * the layout GlaReader documents. Pool entries hold a quaternion (w, x, y, z)
 * stored as (value + 2) * 16383 and a translation stored as (value + 512) * 64.
 */
struct GlaBuilder {
    struct Bone {
        std::string name;
        int parent {-1};
        glm::mat4 basePose {1.0f};
    };

    using Entry = std::array<uint16_t, 7>;

    int version {6};
    std::string name {"synthetic"};
    float scale {1.0f};
    std::vector<Bone> bones;
    std::vector<Entry> pool;
    std::vector<std::vector<uint32_t>> frames; /**< per frame, a pool index per bone */

    static Entry entry(const glm::quat &orientation, const glm::vec3 &translation) {
        auto quantizeRotation = [](float v) {
            return static_cast<uint16_t>(std::lround((v + 2.0f) * 16383.0f));
        };
        auto quantizeTranslation = [](float v) {
            return static_cast<uint16_t>(std::lround((v + 512.0f) * 64.0f));
        };
        return {quantizeRotation(orientation.w),
                quantizeRotation(orientation.x),
                quantizeRotation(orientation.y),
                quantizeRotation(orientation.z),
                quantizeTranslation(translation.x),
                quantizeTranslation(translation.y),
                quantizeTranslation(translation.z)};
    }

    /** A pool entry for a rotation about a model-space point, then a move. */
    static Entry turnAbout(const glm::vec3 &point, const glm::quat &orientation, const glm::vec3 &move = glm::vec3(0.0f)) {
        return entry(orientation, point - orientation * point + move);
    }

    std::string build() const {
        static constexpr int kHeaderSize = 100;
        static constexpr int kNameSize = 64;
        static constexpr int kRecordSize = kNameSize + 4 + 4 + 48 + 48 + 4;

        size_t numBones = bones.size();
        size_t ofsSkel = kHeaderSize + 4 * numBones;
        size_t ofsFrames = ofsSkel + kRecordSize * numBones;
        size_t ofsPool = ofsFrames + 3 * frames.size() * numBones;
        size_t ofsEnd = ofsPool + 14 * pool.size();

        std::string out;
        auto putBytes = [&](const void *data, size_t size) {
            out.append(static_cast<const char *>(data), size);
        };
        auto putInt = [&](int32_t value) {
            uint8_t bytes[4] {
                static_cast<uint8_t>(value & 0xff),
                static_cast<uint8_t>((value >> 8) & 0xff),
                static_cast<uint8_t>((value >> 16) & 0xff),
                static_cast<uint8_t>((value >> 24) & 0xff)};
            putBytes(bytes, 4);
        };
        auto putFloat = [&](float value) {
            int32_t bits;
            std::memcpy(&bits, &value, 4);
            putInt(bits);
        };
        auto putName = [&](const std::string &text) {
            std::string padded(text);
            padded.resize(kNameSize, '\0');
            putBytes(padded.data(), kNameSize);
        };
        auto putMatrix = [&](const glm::mat4 &m) {
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 4; ++column) {
                    putFloat(m[column][row]);
                }
            }
        };

        putBytes("2LGA", 4);
        putInt(version);
        putName(name);
        putFloat(scale);
        putInt(static_cast<int32_t>(frames.size()));
        putInt(static_cast<int32_t>(ofsFrames));
        putInt(static_cast<int32_t>(numBones));
        putInt(static_cast<int32_t>(ofsPool));
        putInt(static_cast<int32_t>(ofsSkel));
        putInt(static_cast<int32_t>(ofsEnd));
        for (size_t i = 0; i < numBones; ++i) {
            putInt(static_cast<int32_t>(4 * numBones + kRecordSize * i));
        }
        for (auto &bone : bones) {
            putName(bone.name);
            putInt(0); // flags
            putInt(bone.parent);
            putMatrix(bone.basePose);
            putMatrix(glm::inverse(bone.basePose));
            putInt(0); // no child list; readers do not need it
        }
        for (auto &frame : frames) {
            for (uint32_t index : frame) {
                uint8_t bytes[3] {
                    static_cast<uint8_t>(index & 0xff),
                    static_cast<uint8_t>((index >> 8) & 0xff),
                    static_cast<uint8_t>((index >> 16) & 0xff)};
                putBytes(bytes, 3);
            }
        }
        for (auto &e : pool) {
            for (uint16_t value : e) {
                uint8_t bytes[2] {static_cast<uint8_t>(value & 0xff), static_cast<uint8_t>(value >> 8)};
                putBytes(bytes, 2);
            }
        }
        return out;
    }
};

} // namespace test

} // namespace reone
