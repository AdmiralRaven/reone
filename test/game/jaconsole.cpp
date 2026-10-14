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

#include <gtest/gtest.h>

#include "../fixtures/engine.h"
#include "../fixtures/gla.h"
#include "reone/game/game.h"
#include "reone/game/jaconsole.h"
#include "reone/resource/types.h"

using namespace reone;
using namespace reone::game;
using namespace reone::test;

namespace {

// A folder with a one-bone GLA and an animation.cfg, removed afterwards.
class JaFiles {
public:
    JaFiles(const std::string &glaName = "_humanoid.gla") {
        _folder = std::filesystem::temp_directory_path() /
                  ("reone-jaconsole-" + std::to_string(std::random_device()()));
        std::filesystem::create_directories(_folder);
        GlaBuilder gla;
        gla.bones = {{"model_root", -1, glm::mat4(1.0f)}};
        gla.pool = {GlaBuilder::entry(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.0f))};
        gla.frames = {{0}, {0}, {0}};
        std::ofstream(_folder / glaName, std::ios::binary) << gla.build();
        std::ofstream(_folder / "animation.cfg") << "// test\n"
                                                    "BOTH_STAND2 0 2 0 20\n"
                                                    "BOTH_A1_TL_BR 0 3 -1 30\n";
    }

    ~JaFiles() {
        std::error_code ec;
        std::filesystem::remove_all(_folder, ec);
    }

    const std::filesystem::path &folder() const { return _folder; }

private:
    std::filesystem::path _folder;
};

std::string lastLine(const StubConsole &console) {
    return console.lines.empty() ? std::string() : console.lines.back();
}

} // namespace

TEST(JaConsole, should_register_the_commands) {
    TestEngine &engine = testEngine();
    StubConsole console;
    Game game(resource::GameID::KotOR, "", engine.options(), engine.services(), console);

    registerJaConsoleCommands(game, console);

    for (auto name : {"jaload", "japlay", "jasaber", "jacam", "jalist"}) {
        EXPECT_TRUE(console.hasCommand(name)) << name;
    }
}

TEST(JaConsole, should_load_animations_from_a_folder_and_list_clips) {
    TestEngine &engine = testEngine();
    StubConsole console;
    Game game(resource::GameID::KotOR, "", engine.options(), engine.services(), console);
    registerJaConsoleCommands(game, console);
    // Windows file names ignore case; so does the lookup.
    JaFiles files("_HUMANOID.GLA");

    console.execute("jaload", {files.folder().string()});
    EXPECT_EQ(std::string("jaload: synthetic, 1 bones, 3 frames, 2 clips"), lastLine(console));

    console.execute("jalist", {"a1"});
    ASSERT_GE(console.lines.size(), 2u);
    EXPECT_EQ(std::string("BOTH_A1_TL_BR: 3 frames at 30 fps"), console.lines[console.lines.size() - 2]);
    EXPECT_EQ(std::string("jalist: 1 clips match"), lastLine(console));
}

TEST(JaConsole, should_explain_what_is_missing) {
    TestEngine &engine = testEngine();
    StubConsole console;
    Game game(resource::GameID::KotOR, "", engine.options(), engine.services(), console);
    registerJaConsoleCommands(game, console);

    auto failure = [&](const std::string &name, std::vector<std::string> args) {
        try {
            console.execute(name, std::move(args));
        } catch (const std::exception &e) {
            return std::string(e.what());
        }
        return std::string("no error");
    };

    EXPECT_NE(std::string::npos, failure("japlay", {"swing"}).find("Run jaload"));
    EXPECT_NE(std::string::npos, failure("jalist", {}).find("Run jaload"));
    EXPECT_NE(std::string::npos, failure("jaload", {}).find("Usage"));
    EXPECT_NE(std::string::npos, failure("jaload", {"/no/such/folder"}).find("Not found"));

    JaFiles files;
    console.execute("jaload", {files.folder().string()});
    EXPECT_NE(std::string::npos, failure("japlay", {"swing"}).find("Load a game first"));
    EXPECT_NE(std::string::npos, failure("japlay", {"swing", "0"}).find("Speed must be"));
    EXPECT_NE(std::string::npos, failure("jasaber", {}).find("Load a game first"));
    EXPECT_NE(std::string::npos, failure("jacam", {}).find("Load a game first"));
}
