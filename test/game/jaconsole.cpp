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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../fixtures/engine.h"
#include "../fixtures/gla.h"
#include "reone/game/action/castspellatobject.h"
#include "reone/game/d20/spell.h"
#include "reone/game/game.h"
#include "reone/game/jaconsole.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/item.h"
#include "reone/resource/2da.h"
#include "reone/resource/format/gffwriter.h"
#include "reone/resource/gff.h"
#include "reone/resource/types.h"

using namespace reone;
using namespace reone::game;
using namespace reone::resource;
using namespace reone::test;
using namespace testing;

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

std::string failure(StubConsole &console, const std::string &name, std::vector<std::string> args) {
    try {
        console.execute(name, std::move(args));
    } catch (const std::exception &e) {
        return std::string(e.what());
    }
    return std::string("no error");
}

// A save slot complete enough for the engine to list it.
void writeSaveSlot(const std::filesystem::path &directory) {
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "SAVEGAME.sav", std::ios::binary).put('x');
    auto nfo = Gff::Builder()
                   .field(Gff::Field::newCExoString("AREANAME", "Upper City"))
                   .field(Gff::Field::newCExoString("LASTMODULE", "tar_m02aa"))
                   .field(Gff::Field::newCExoString("PCNAME", "Revan"))
                   .field(Gff::Field::newDword("TIMEPLAYED", 3723))
                   .field(Gff::Field::newCExoString("SAVEGAMENAME", directory.filename().string()))
                   .field(Gff::Field::newCExoString("PORTRAIT0", "po_pmhc01"))
                   .build();
    auto bytes = GffWriter(GffFileFormat::v32("NFO "), *nfo).toBytes();
    std::ofstream nfoFile(directory / "savenfo.res", std::ios::binary);
    nfoFile.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// The engine's mocks are shared by every test: once a test is done, the
// engine's spells find none again, as before it.
class SpellLookupsReset : boost::noncopyable {
public:
    explicit SpellLookupsReset(TestEngine &engine) :
        _engine(engine) {
    }

    ~SpellLookupsReset() {
        EXPECT_CALL(_engine.gameModule().spells(), get(_)).Times(AnyNumber());
    }

private:
    TestEngine &_engine;
};

// KOTOR's medpacs cast spell 64 when used; the engine's spells say it heals.
// Every other lookup the items make finds nothing, as the engine's mocks
// leave it.
std::shared_ptr<Item> healingItem(TestEngine &engine, Game &game, const std::string &tag, int stackSize) {
    auto medpac = std::make_shared<Spell>();
    medpac->type = static_cast<SpellType>(64);
    EXPECT_CALL(engine.gameModule().spells(), get(_)).Times(AnyNumber()).WillRepeatedly(Return(medpac));
    std::shared_ptr<TwoDA> baseItems = TwoDA::Builder()
                                           .columns({"equipableslots", "bodyvar", "ammunitiontype"})
                                           .row({"0", "", "0"})
                                           .build();
    EXPECT_CALL(engine.resourceModule().twoDas(), get(_)).Times(AnyNumber());
    EXPECT_CALL(engine.resourceModule().twoDas(), get("baseitems")).Times(AnyNumber()).WillRepeatedly(Return(baseItems));
    auto property = Gff::Builder()
                        .field(Gff::Field::newWord("PropertyName", static_cast<uint16_t>(ItemProperty::ActivateItem)))
                        .field(Gff::Field::newWord("Subtype", 64))
                        .field(Gff::Field::newByte("CostTable", 3))
                        .field(Gff::Field::newWord("CostValue", 1))
                        .build();
    auto uti = Gff::Builder()
                   .field(Gff::Field::newCExoString("Tag", tag))
                   .field(Gff::Field::newInt("BaseItem", 0))
                   .field(Gff::Field::newWord("StackSize", static_cast<uint16_t>(stackSize)))
                   .field(Gff::Field::newList("PropertiesList", {property}))
                   .build();
    auto item = game.newItem(*uti, SerializedIdentityContext::templateResource(tag));
    item->setStackSize(stackSize);
    return item;
}

} // namespace

TEST(JaConsole, should_register_the_commands) {
    TestEngine &engine = testEngine();
    StubConsole console;
    Game game(resource::GameID::KotOR, "", engine.options(), engine.services(), console);

    registerJaConsoleCommands(game, console);

    for (auto name : {"jaload", "jaloadsave", "japlay", "jasaber", "jacam", "jalist", "jaengage",
                      "jastatus", "jacreature", "jafight", "jause", "jahitpoints", "jadefense", "jaenemies"}) {
        EXPECT_TRUE(console.hasCommand(name)) << name;
    }
}

TEST(JaConsole, should_describe_the_leader_for_scripted_checks) {
    // given: a leader at 4 of 12 hit points, with two medpacs in one stack
    TestEngine &engine = testEngine();
    StubConsole console;
    Game game(resource::GameID::KotOR, "", engine.options(), engine.services(), console);
    SpellLookupsReset reset(engine);
    auto medpacs = healingItem(engine, game, "g_i_medeqpmnt01", 2);
    auto leader = game.newCreature();
    leader->setTag("player");
    leader->setMaxHitPoints(12);
    ASSERT_EQ(12, leader->maxHitPoints());
    leader->setCurrentHitPoints(4);
    leader->addItem(medpacs);

    // then: one line, in words a script can read
    EXPECT_EQ(std::string("jastatus: calm: player hit points 4 of 12, combat off, movement free, items g_i_medeqpmnt01 2"),
              jaLeaderStatus(*leader, "calm", "g_i_medeqpmnt01"));
    EXPECT_EQ(std::string("jastatus: calm: player hit points 4 of 12, combat off, movement free"),
              jaLeaderStatus(*leader, "calm", ""));
    EXPECT_EQ(std::string("jastatus: calm: player hit points 4 of 12, combat off, movement free, items g_i_adrnaline003 0"),
              jaLeaderStatus(*leader, "calm", "g_i_adrnaline003"));

    // when: the leader is in combat and held in place
    leader->activateCombat();
    leader->setMovementRestricted(true);

    // then: the line says so; tags match whatever their case
    EXPECT_EQ(std::string("jastatus: fight: player hit points 4 of 12, combat on, movement held, items G_I_MEDEQPMNT01 2"),
              jaLeaderStatus(*leader, "fight", "G_I_MEDEQPMNT01"));
}

TEST(JaConsole, should_use_a_healing_item_as_the_action_bar_does) {
    // given: a leader with two medpacs in one stack
    TestEngine &engine = testEngine();
    StubConsole console;
    Game game(resource::GameID::KotOR, "", engine.options(), engine.services(), console);
    SpellLookupsReset reset(engine);
    auto medpacs = healingItem(engine, game, "g_i_medeqpmnt01", 2);
    auto leader = game.newCreature();
    leader->addItem(medpacs);

    // then: nothing happens for an item the leader hasn't
    EXPECT_EQ(nullptr, jaUseHealingItem(game, leader, "g_i_adrnaline003"));
    EXPECT_TRUE(leader->actions().empty());

    // when: the medpacs are used
    EXPECT_EQ(medpacs, jaUseHealingItem(game, leader, "g_i_medeqpmnt01"));

    // then: the leader casts their spell from them, as a player's click does
    ASSERT_EQ(1u, leader->actions().size());
    std::shared_ptr<game::Action> action = leader->actions().front();
    ASSERT_EQ(ActionType::CastSpellAtObject, action->type());
    EXPECT_TRUE(action->isUserAction());
    auto &cast = static_cast<CastSpellAtObjectAction &>(*action);
    EXPECT_EQ(static_cast<SpellType>(64), cast.spell()->type);
    ASSERT_TRUE(cast.item().has_value());
    EXPECT_EQ(medpacs, cast.item().value());
}

TEST(JaConsole, should_number_saved_games_as_listgames_does) {
    TestEngine &engine = testEngine();
    StubConsole console;
    // One complete slot, a second copy of the same slot, a folder that is not
    // a slot and a slot without its files: four folders, one saved game.
    auto root = std::filesystem::temp_directory_path() /
                ("reone-jaconsole-saves-" + std::to_string(std::random_device()()));
    writeSaveSlot(root / "saves" / "000002 - Game1");
    writeSaveSlot(root / "saves" / "000002 - Game1 copy");
    std::filesystem::create_directories(root / "saves" / "backup");
    std::filesystem::create_directories(root / "saves" / "000007 - Game6");
    Game game(resource::GameID::KotOR, root, engine.options(), engine.services(), console);
    registerJaConsoleCommands(game, console);

    EXPECT_EQ(1ll, game.savedGames().size());
    EXPECT_NE(std::string::npos, failure(console, "jaloadsave", {"1"}).find("No saved game 1: listgames numbers them 0 to 0"));
    EXPECT_NE(std::string::npos, failure(console, "jaloadsave", {"3"}).find("0 to 0"));
    EXPECT_NE(std::string::npos, failure(console, "jaloadsave", {"-1"}).find("No saved game -1"));
    EXPECT_NE(std::string::npos, failure(console, "jaloadsave", {"0", "1"}).find("Usage"));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(JaConsole, should_say_when_there_is_no_saved_game) {
    TestEngine &engine = testEngine();
    StubConsole console;
    auto root = std::filesystem::temp_directory_path() /
                ("reone-jaconsole-nosaves-" + std::to_string(std::random_device()()));
    std::filesystem::create_directories(root);
    Game game(resource::GameID::KotOR, root, engine.options(), engine.services(), console);
    registerJaConsoleCommands(game, console);

    EXPECT_NE(std::string::npos, failure(console, "jaloadsave", {}).find("No saved games"));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
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

    EXPECT_NE(std::string::npos, failure(console, "japlay", {"swing"}).find("Run jaload"));
    EXPECT_NE(std::string::npos, failure(console, "jalist", {}).find("Run jaload"));
    EXPECT_NE(std::string::npos, failure(console, "jaload", {}).find("Usage"));
    EXPECT_NE(std::string::npos, failure(console, "jaload", {"/no/such/folder"}).find("Not found"));

    JaFiles files;
    console.execute("jaload", {files.folder().string()});
    EXPECT_NE(std::string::npos, failure(console, "japlay", {"swing"}).find("Load a game first"));
    EXPECT_NE(std::string::npos, failure(console, "japlay", {"swing", "0"}).find("Speed must be"));
    EXPECT_NE(std::string::npos, failure(console, "jasaber", {}).find("Load a game first"));
    EXPECT_NE(std::string::npos, failure(console, "jacam", {}).find("Load a game first"));
    EXPECT_NE(std::string::npos, failure(console, "jastatus", {"calm"}).find("Load a game first"));
    EXPECT_NE(std::string::npos, failure(console, "jafight", {}).find("Load a game first"));
    EXPECT_NE(std::string::npos, failure(console, "jause", {"g_i_medeqpmnt01"}).find("Load a game first"));
    EXPECT_NE(std::string::npos, failure(console, "jahitpoints", {"4"}).find("Load a game first"));
    EXPECT_NE(std::string::npos, failure(console, "jastatus", {}).find("Usage"));
    EXPECT_NE(std::string::npos, failure(console, "jafight", {"end_sith2"}).find("Usage"));
    EXPECT_NE(std::string::npos, failure(console, "jause", {}).find("Usage"));
    EXPECT_NE(std::string::npos, failure(console, "jahitpoints", {}).find("Usage"));
    EXPECT_NE(std::string::npos, failure(console, "jacreature", {"end_sith2"}).find("Load a game first"));
    EXPECT_NE(std::string::npos, failure(console, "jacreature", {}).find("Usage"));
    EXPECT_NE(std::string::npos, failure(console, "jacreature", {"end_sith2", "end_sith3"}).find("Usage"));

    // Without a game, turning engaging off has no enemies to report.
    console.execute("jaengage", {"off"});
    EXPECT_EQ(std::string("jaengage: off, the keyboard and mouse again"), lastLine(console));

    EXPECT_NE(std::string::npos, failure(console, "jadefense", {"4"}).find("Usage"));
    EXPECT_NE(std::string::npos, failure(console, "jadefense", {"-1"}).find("Usage"));
    EXPECT_NE(std::string::npos, failure(console, "jaenemies", {"maybe"}).find("Usage"));
}

TEST(JaConsole, should_show_and_set_the_defense_rank_and_the_real_time_enemies) {
    TestEngine &engine = testEngine();
    StubConsole console;
    Game game(resource::GameID::KotOR, "", engine.options(), engine.services(), console);
    registerJaConsoleCommands(game, console);

    console.execute("jadefense", {});
    EXPECT_EQ(std::string("jadefense: saber defense rank 2"), lastLine(console));
    console.execute("jadefense", {"3"});
    EXPECT_EQ(std::string("jasaber: saber defense rank 3"), lastLine(console));
    EXPECT_EQ(3, game.jaSaber().defense());
    console.execute("jadefense", {"0"});
    EXPECT_EQ(std::string("jasaber: saber defense rank 0, no defense"), lastLine(console));

    console.execute("jaenemies", {});
    EXPECT_EQ(std::string("jaenemies: enemies fight saber mode in real time; 0 volleys, 0 bolts (0 aimed): 0 sent back, "
                          "0 stopped, 0 through the guard, 0 hit the character, 0 hit enemies; 0 swings: 0 parried, 0 hit, 0 missed"),
              lastLine(console));
    console.execute("jaenemies", {"off"});
    EXPECT_FALSE(game.jaSaber().foes().enabled());
    EXPECT_NE(std::string::npos, lastLine(console).find("enemies fight by KOTOR's rounds"));
    console.execute("jaenemies", {"on"});
    EXPECT_TRUE(game.jaSaber().foes().enabled());
}
