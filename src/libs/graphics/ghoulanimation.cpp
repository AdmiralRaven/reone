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

#include "reone/graphics/ghoulanimation.h"

namespace reone {

namespace graphics {

GhoulAnimation::GhoulAnimation(
    std::string name,
    float scale,
    int numFrames,
    std::vector<Bone> bones,
    std::vector<uint8_t> frameIndices,
    std::vector<uint8_t> bonePool) :
    _name(std::move(name)),
    _scale(scale),
    _numFrames(numFrames),
    _bones(std::move(bones)),
    _frameIndices(std::move(frameIndices)),
    _bonePool(std::move(bonePool)) {

    const int numBones = static_cast<int>(_bones.size());
    if (_numFrames < 1 || numBones < 1) {
        throw std::invalid_argument("GLA needs at least one frame and one bone");
    }
    size_t expectedIndices = static_cast<size_t>(_numFrames) * numBones * kFrameIndexSize;
    if (_frameIndices.size() != expectedIndices) {
        throw std::invalid_argument("GLA frame index table has the wrong size");
    }
    if (_bonePool.size() % kPoolEntrySize != 0) {
        throw std::invalid_argument("GLA bone pool is not a whole number of entries");
    }
    size_t poolEntries = _bonePool.size() / kPoolEntrySize;
    for (size_t i = 0; i < _frameIndices.size(); i += kFrameIndexSize) {
        uint32_t index = _frameIndices[i] |
                         (_frameIndices[i + 1] << 8) |
                         (_frameIndices[i + 2] << 16);
        if (index >= poolEntries) {
            throw std::invalid_argument("GLA frame index points outside the bone pool");
        }
    }

    // Parents before children; any bone whose chain does not end at a root is rejected.
    std::vector<int> state(numBones, 0); // 0: unvisited, 1: on the current chain, 2: placed
    for (int start = 0; start < numBones; ++start) {
        std::vector<int> chain;
        int bone = start;
        while (bone >= 0 && state[bone] == 0) {
            state[bone] = 1;
            chain.push_back(bone);
            int parent = _bones[bone].parent;
            if (parent >= numBones || parent < -1) {
                throw std::invalid_argument("GLA bone has an invalid parent: " + _bones[bone].name);
            }
            bone = parent;
        }
        if (bone >= 0 && state[bone] == 1) {
            throw std::invalid_argument("GLA bones form a cycle at: " + _bones[bone].name);
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            state[*it] = 2;
            _order.push_back(*it);
        }
    }

    for (int i = 0; i < numBones; ++i) {
        _boneByName.insert({boost::to_lower_copy(_bones[i].name), i});
    }
}

int GhoulAnimation::boneIndex(const std::string &name) const {
    auto it = _boneByName.find(boost::to_lower_copy(name));
    return it != _boneByName.end() ? it->second : -1;
}

glm::mat4 GhoulAnimation::boneDelta(int frame, int bone) const {
    if (frame < 0 || frame >= _numFrames || bone < 0 || bone >= numBones()) {
        throw std::out_of_range("GLA frame or bone out of range");
    }
    size_t at = (static_cast<size_t>(frame) * _bones.size() + bone) * kFrameIndexSize;
    uint32_t index = _frameIndices[at] |
                     (_frameIndices[at + 1] << 8) |
                     (_frameIndices[at + 2] << 16);
    const uint8_t *entry = &_bonePool[static_cast<size_t>(index) * kPoolEntrySize];
    float values[7];
    for (int i = 0; i < 7; ++i) {
        values[i] = static_cast<float>(entry[2 * i] | (entry[2 * i + 1] << 8));
    }
    // Quaternion components are stored as (value + 2) * 16383, translations
    // as (value + 512) * 64. The quaternion is used as stored, unnormalized.
    glm::quat orientation(
        values[0] / 16383.0f - 2.0f,
        values[1] / 16383.0f - 2.0f,
        values[2] / 16383.0f - 2.0f,
        values[3] / 16383.0f - 2.0f);
    glm::mat4 delta(glm::mat4_cast(orientation));
    delta[3] = glm::vec4(
        values[4] / 64.0f - 512.0f,
        values[5] / 64.0f - 512.0f,
        values[6] / 64.0f - 512.0f,
        1.0f);
    return delta;
}

std::vector<glm::mat4> GhoulAnimation::boneTransforms(int frame) const {
    std::vector<glm::mat4> animated(_bones.size());
    for (int bone : _order) {
        glm::mat4 delta(boneDelta(frame, bone));
        int parent = _bones[bone].parent;
        animated[bone] = parent < 0 ? delta : animated[parent] * delta;
    }
    for (size_t i = 0; i < _bones.size(); ++i) {
        animated[i] = animated[i] * _bones[i].basePose;
    }
    return animated;
}

std::map<std::string, GhoulAnimationClip> parseGhoulAnimationConfig(const std::string &text) {
    std::map<std::string, GhoulAnimationClip> clips;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        size_t comment = line.find("//");
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        std::istringstream fields(line);
        std::string name;
        GhoulAnimationClip clip;
        if (!(fields >> name >> clip.firstFrame >> clip.numFrames >> clip.loopFrames >> clip.fps)) {
            continue;
        }
        if (!std::isalpha(static_cast<unsigned char>(name[0])) || clip.numFrames < 1 || clip.firstFrame < 0) {
            continue;
        }
        clips[boost::to_upper_copy(name)] = clip;
    }
    return clips;
}

} // namespace graphics

} // namespace reone
