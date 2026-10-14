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

namespace graphics {

/**
 * Skeleton and frames of a Ghoul2 animation file (.gla, version 6), the
 * format Jedi Academy keeps its humanoid animations in. Frames stay
 * compressed as stored and are decoded on request.
 *
 * Written for this project from the file layout; it contains no Jedi Academy
 * or OpenJK code. Game data is read from the player's own installation and is
 * never bundled.
 */
class GhoulAnimation : boost::noncopyable {
public:
    static constexpr int kFrameIndexSize = 3; /**< bytes per bone per frame */
    static constexpr int kPoolEntrySize = 14; /**< bytes per compressed bone transform */

    struct Bone {
        std::string name;
        uint32_t flags {0};
        int parent {-1};
        glm::mat4 basePose {1.0f}; /**< bind pose in model space, scaled as stored */
        glm::mat4 basePoseInverse {1.0f};
    };

    /**
     * \param frameIndices for each frame, then each bone, a 24-bit
     *        little-endian index into \p bonePool
     * \param bonePool compressed bone transforms, 14 bytes each
     * \throws std::invalid_argument when the bones do not form a tree or an
     *         index points outside the pool
     */
    GhoulAnimation(
        std::string name,
        float scale,
        int numFrames,
        std::vector<Bone> bones,
        std::vector<uint8_t> frameIndices,
        std::vector<uint8_t> bonePool);

    const std::string &name() const { return _name; }
    float scale() const { return _scale; }
    int numFrames() const { return _numFrames; }
    int numBones() const { return static_cast<int>(_bones.size()); }
    const std::vector<Bone> &bones() const { return _bones; }

    /** Index of the bone with this name, ignoring case; -1 when absent. */
    int boneIndex(const std::string &name) const;

    /**
     * The stored transform of a bone in a frame. A bone's animated transform
     * is its parent's animated transform times this.
     */
    glm::mat4 boneDelta(int frame, int bone) const;

    /**
     * Model-space transforms of every bone in a frame: the deltas down the
     * bone's chain, then its base pose.
     */
    std::vector<glm::mat4> boneTransforms(int frame) const;

private:
    std::string _name;
    float _scale;
    int _numFrames;
    std::vector<Bone> _bones;
    std::vector<uint8_t> _frameIndices;
    std::vector<uint8_t> _bonePool;

    std::vector<int> _order; /**< bone indices, parents before children */
    std::unordered_map<std::string, int> _boneByName;
};

/** A clip of Jedi Academy's animation.cfg. */
struct GhoulAnimationClip {
    int firstFrame {0};
    int numFrames {0};
    int loopFrames {-1}; /**< -1 plays once; anything else loops */
    float fps {0.0f};    /**< negative plays the frames in reverse */
};

/**
 * Clips of a Jedi Academy animation.cfg, keyed by upper-case name. Lines that
 * are not "name firstFrame numFrames loopFrames fps" are skipped, as are
 * comments after "//".
 */
std::map<std::string, GhoulAnimationClip> parseGhoulAnimationConfig(const std::string &text);

} // namespace graphics

} // namespace reone
