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

#include <filesystem>
#include <iostream>

#include "../fixtures/engine.h"
#include "../fixtures/game.h"

#include "reone/game/game.h"
#include "reone/game/modulesnapshot.h"
#include "reone/game/object/area.h"
#include "reone/game/object/door.h"
#include "reone/game/object/placeable.h"
#include "reone/game/savedruntime.h"
#include "reone/game/savedtemplates.h"
#include "reone/game/saveprovenance.h"
#include "reone/resource/director.h"
#include "reone/resource/2da.h"
#include "reone/resource/container/erf.h"
#include "reone/resource/container/rim.h"
#include "reone/resource/format/gffreader.h"
#include "reone/resource/gff.h"
#include "reone/system/stream/memoryinput.h"

using namespace reone;
using namespace reone::game;
using namespace reone::resource;
using namespace testing;

namespace {

// The Endar Spire's first locked door and the scene object behind it, with
// the fields their templates give them (end_door02.utd, invisible002.utp).
std::shared_ptr<Gff> lockedDoorTemplate() {
    return Gff::Builder()
        .type(0xffffffff)
        .field(Gff::Field::newCExoString("Tag", "end_door02"))
        .field(Gff::Field::newResRef("TemplateResRef", "end_door02"))
        .field(Gff::Field::newCExoLocString("LocName", 1234, ""))
        .field(Gff::Field::newResRef("Conversation", "end_trask01"))
        .field(Gff::Field::newResRef("OnOpen", "k_pend_door18"))
        .field(Gff::Field::newResRef("OnFailToOpen", "k_pend_door19"))
        .field(Gff::Field::newResRef("OnDeath", "k_pend_door18"))
        .field(Gff::Field::newByte("Locked", 1))
        .field(Gff::Field::newByte("OpenLockDC", 1))
        .field(Gff::Field::newByte("GenericType", 1))
        .field(Gff::Field::newDword("Faction", 1))
        .build();
}

std::shared_ptr<Gff> sceneObjectTemplate() {
    return Gff::Builder()
        .type(0xffffffff)
        .field(Gff::Field::newCExoString("Tag", "end01_sceneobj01"))
        .field(Gff::Field::newResRef("TemplateResRef", "invisible002"))
        .field(Gff::Field::newDword("Appearance", 54))
        .field(Gff::Field::newResRef("Conversation", "end_room3"))
        .field(Gff::Field::newResRef("OnUserDefined", "k_pend_resume"))
        .field(Gff::Field::newResRef("OnUsed", ""))
        .build();
}

std::shared_ptr<Gff> place(Gff::Builder builder, float x, float y, float z = 0.0f) {
    return builder
        .field(Gff::Field::newFloat("X", x))
        .field(Gff::Field::newFloat("Y", y))
        .field(Gff::Field::newFloat("Z", z))
        .build();
}

// The module's own GIT names the door's template, tag and place, and the
// placeable's template and place only.
std::shared_ptr<Gff> authoredDoor(std::string resRef = "end_door02", std::string tag = "end_door02", float x = 40.8f) {
    return place(Gff::Builder().type(8)
                     .field(Gff::Field::newResRef("TemplateResRef", std::move(resRef)))
                     .field(Gff::Field::newCExoString("Tag", std::move(tag))),
                 x, 20.71f);
}

std::shared_ptr<Gff> authoredSceneObject() {
    return place(Gff::Builder().type(9).field(Gff::Field::newResRef("TemplateResRef", "invisible002")), 48.63f, 21.85f);
}

std::shared_ptr<Gff> git(
    std::vector<std::shared_ptr<Gff>> doors,
    std::vector<std::shared_ptr<Gff>> placeables,
    uint8_t useTemplates) {
    return Gff::Builder()
        .type(0xffffffff)
        .field(Gff::Field::newByte("UseTemplates", useTemplates))
        .field(Gff::Field::newList("Door List", std::move(doors)))
        .field(Gff::Field::newList("Placeable List", std::move(placeables)))
        .build();
}

std::shared_ptr<Gff> authoredGit() {
    return git({authoredDoor()}, {authoredSceneObject()}, 1);
}

// As reone saved them before the fix: no template, and none of its fields.
std::shared_ptr<Gff> savedDoor(std::string tag = "end_door02", float x = 40.8f) {
    return place(Gff::Builder().type(8)
                     .field(Gff::Field::newDword("ObjectId", 1001))
                     .field(Gff::Field::newCExoString("Tag", std::move(tag)))
                     .field(Gff::Field::newByte("Locked", 0))
                     .field(Gff::Field::newByte("OpenState", 1))
                     .field(Gff::Field::newByte("GenericType", 1)),
                 x, 20.71f);
}

std::shared_ptr<Gff> savedSceneObject(float x = 48.63f) {
    return place(Gff::Builder().type(9)
                     .field(Gff::Field::newDword("ObjectId", 1002))
                     .field(Gff::Field::newCExoString("Tag", "end01_sceneobj01"))
                     .field(Gff::Field::newByte("Useable", 0))
                     .field(Gff::Field::newList("ItemList", {})),
                 x, 21.85f);
}

std::shared_ptr<Gff> with(std::shared_ptr<Gff> record, Gff::Field field) {
    replaceSaveField(*record, std::move(field));
    return record;
}

// Two doors that share a tag on the Endar Spire, each with its own template.
std::shared_ptr<Gff> sharedTagDoorTemplate(std::string resRef, std::string onOpen) {
    return Gff::Builder()
        .type(0xffffffff)
        .field(Gff::Field::newCExoString("Tag", "end_door10"))
        .field(Gff::Field::newResRef("TemplateResRef", std::move(resRef)))
        .field(Gff::Field::newResRef("OnOpen", std::move(onOpen)))
        .build();
}

std::shared_ptr<Gff> findTemplate(const std::string &resRef, ResType type) {
    if (resRef == "end_door02" && type == ResType::Utd) {
        return lockedDoorTemplate();
    }
    if (resRef == "end_door011" && type == ResType::Utd) {
        return sharedTagDoorTemplate(resRef, "k_first");
    }
    if (resRef == "end_door013" && type == ResType::Utd) {
        return sharedTagDoorTemplate(resRef, "k_second");
    }
    if (resRef == "invisible002" && type == ResType::Utp) {
        return sceneObjectTemplate();
    }
    return nullptr;
}

TemplateLookup templates() {
    return findTemplate;
}

std::shared_ptr<Gff> recordById(const Gff &root, const std::string &list, uint32_t id) {
    for (const auto &record : root.getList(list)) {
        if (record->getUint("ObjectId", kSavedRuntimeInvalidObjectId) == id) {
            return record;
        }
    }
    return nullptr;
}

} // namespace

TEST(SavedTemplates, should_find_records_saved_without_their_templates_fields) {
    // given: KOTOR's saves hold OnOpen and Appearance even when empty, and
    // records reone saves now name their template
    auto kotorDoor = with(savedDoor(), Gff::Field::newResRef("OnOpen", ""));
    auto kotorPlaceable = with(savedSceneObject(), Gff::Field::newDword("Appearance", 54));
    auto namedDoor = with(savedDoor(), Gff::Field::newResRef("TemplateResRef", "end_door02"));

    // then
    EXPECT_TRUE(hasRecordsWithoutTemplateFields(*git({savedDoor()}, {}, 0)));
    EXPECT_TRUE(hasRecordsWithoutTemplateFields(*git({kotorDoor}, {savedSceneObject()}, 0)));
    EXPECT_FALSE(hasRecordsWithoutTemplateFields(*git({kotorDoor, namedDoor}, {kotorPlaceable}, 0)));
}

TEST(SavedTemplates, should_restore_a_door_and_a_placeable_from_their_templates_under_the_saved_state) {
    // when
    auto repair = repairSavedTemplateFields(*git({savedDoor()}, {savedSceneObject()}, 0), *authoredGit(), templates());

    // then
    ASSERT_TRUE(repair.git);
    EXPECT_EQ(2, repair.repaired);
    EXPECT_TRUE(repair.unmatched.empty());

    // the door has its template's scripts, conversation and faction, and names it
    auto door = repair.git->getList("Door List").at(0);
    EXPECT_EQ("k_pend_door19", door->getString("OnFailToOpen"));
    EXPECT_EQ("k_pend_door18", door->getString("OnOpen"));
    EXPECT_EQ("end_trask01", door->getString("Conversation"));
    EXPECT_EQ(1u, door->getUint("Faction"));
    EXPECT_EQ("end_door02", door->getString("TemplateResRef"));
    // under its saved state: unlocked and open, where the template locks it
    EXPECT_EQ(1001u, door->getUint("ObjectId"));
    EXPECT_FALSE(door->getBool("Locked"));
    EXPECT_EQ(1u, door->getUint("OpenState"));
    EXPECT_FLOAT_EQ(40.8f, door->getFloat("X"));
    EXPECT_EQ(8u, door->type());

    // the scene object has its appearance, conversation and script
    auto sceneObject = repair.git->getList("Placeable List").at(0);
    EXPECT_EQ(54u, sceneObject->getUint("Appearance"));
    EXPECT_EQ("end_room3", sceneObject->getString("Conversation"));
    EXPECT_EQ("k_pend_resume", sceneObject->getString("OnUserDefined"));
    EXPECT_EQ("invisible002", sceneObject->getString("TemplateResRef"));
    EXPECT_EQ(1002u, sceneObject->getUint("ObjectId"));
    EXPECT_EQ(9u, sceneObject->type());

    // and the rest of the GIT is the saved one
    EXPECT_EQ(0u, repair.git->getUint("UseTemplates"));
}

TEST(SavedTemplates, should_take_the_object_with_that_tag_at_that_place) {
    // given: the Endar Spire has two doors tagged end_door10, 2 m apart, each
    // with its own template
    auto authored = git({authoredDoor("end_door011", "end_door10", 47.42f), authoredDoor("end_door013", "end_door10", 49.43f)}, {}, 1);

    // when
    auto repair = repairSavedTemplateFields(*git({savedDoor("end_door10", 49.43f), savedDoor("end_door10", 47.42f)}, {}, 0), *authored, templates());

    // then: each the one at its place
    ASSERT_TRUE(repair.git);
    auto doors = repair.git->getList("Door List");
    EXPECT_EQ("end_door013", doors.at(0)->getString("TemplateResRef"));
    EXPECT_EQ("k_second", doors.at(0)->getString("OnOpen"));
    EXPECT_EQ("end_door011", doors.at(1)->getString("TemplateResRef"));
    EXPECT_EQ("k_first", doors.at(1)->getString("OnOpen"));
}

TEST(SavedTemplates, should_leave_records_that_match_no_authored_object) {
    // given: a door elsewhere, another tag at the door's place, a placeable
    // elsewhere
    auto saved = git({savedDoor("end_door02", 50.0f), savedDoor("end_door03")}, {savedSceneObject(49.63f)}, 0);

    // when
    auto repair = repairSavedTemplateFields(*saved, *authoredGit(), templates());

    // then
    EXPECT_FALSE(repair.git);
    EXPECT_EQ(0, repair.repaired);
    EXPECT_THAT(repair.unmatched, ElementsAre("Door List/end_door02", "Door List/end_door03", "Placeable List/end01_sceneobj01"));
}

TEST(SavedTemplates, should_leave_complete_records_as_they_are) {
    // given: a KOTOR door whose save says it has no OnOpen, next to a
    // placeable reone saved without its template
    auto kotorDoor = with(savedDoor(), Gff::Field::newResRef("OnOpen", ""));

    // when
    auto repair = repairSavedTemplateFields(*git({kotorDoor}, {savedSceneObject()}, 0), *authoredGit(), templates());

    // then: only the placeable takes its template
    ASSERT_TRUE(repair.git);
    EXPECT_EQ(1, repair.repaired);
    auto door = repair.git->getList("Door List").at(0);
    EXPECT_EQ("", door->getString("OnOpen"));
    EXPECT_FALSE(door->has("OnFailToOpen"));
    EXPECT_FALSE(door->has("TemplateResRef"));
}

namespace {

std::shared_ptr<TwoDA> modelNames() {
    TwoDA::Builder builder;
    builder.columns({"modelname"});
    for (int row = 0; row < 60; ++row) {
        builder.row({""});
    }
    return std::shared_ptr<TwoDA>(builder.build());
}

struct SavedTemplatesGame : Test {
    SavedTemplatesGame() :
        game(GameID::KotOR, "", engine.options(), engine.services(), console) {
    }

    void SetUp() override {
        auto &resources = engine.resourceModule();
        EXPECT_CALL(resources.gffs(), get(_, _)).Times(AnyNumber()).WillRepeatedly(Invoke(findTemplate));
        EXPECT_CALL(resources.twoDas(), get("genericdoors")).Times(AnyNumber()).WillRepeatedly(Return(modelNames()));
        EXPECT_CALL(resources.twoDas(), get("placeables")).Times(AnyNumber()).WillRepeatedly(Return(modelNames()));
        EXPECT_CALL(resources.models(), get(_)).Times(AnyNumber());
        EXPECT_CALL(resources.walkmeshes(), get(_, _)).Times(AnyNumber());
        ifo = Gff::Builder()
                  .type(0xffffffff)
                  .field(Gff::Field::newCExoString("Mod_Entry_Area", "m01aa"))
                  .build();
    }

    void TearDown() override {
        auto &resources = engine.resourceModule();
        Mock::VerifyAndClearExpectations(&resources.gffs());
        Mock::VerifyAndClearExpectations(&resources.twoDas());
        Mock::VerifyAndClearExpectations(&resources.models());
        Mock::VerifyAndClearExpectations(&resources.walkmeshes());
        Mock::VerifyAndClearExpectations(&resources.director());
    }

    std::shared_ptr<Gff> restore(std::shared_ptr<Gff> saved) {
        auto &resources = engine.resourceModule();
        return withSavedTemplateFields(resources.director(), resources.gffs(), "end_m01aa", *ifo, std::move(saved));
    }

    std::unique_ptr<PreparedModuleLoad> installed(std::shared_ptr<Gff> installedIfo) {
        auto are = Gff::Builder().type(0xffffffff).build();
        return PreparedModuleLoadTestFactory::validated("end_m01aa", std::move(installedIfo), are, authoredGit());
    }

    TestEngine &engine {testEngine()};
    StubConsole console;
    Game game;
    std::shared_ptr<Gff> ifo;
};

} // namespace

TEST_F(SavedTemplatesGame, should_restore_a_saved_module_from_the_installed_one) {
    // given: the module as installed, read without the save
    EXPECT_CALL(engine.resourceModule().director(), prepareModuleLoad("end_m01aa", IsNull()))
        .WillOnce(Return(ByMove(installed(ifo))));

    // when
    auto restored = restore(git({savedDoor()}, {savedSceneObject()}, 0));

    // then
    ASSERT_TRUE(restored);
    EXPECT_EQ("k_pend_door19", restored->getList("Door List").at(0)->getString("OnFailToOpen"));
    EXPECT_EQ("end_room3", restored->getList("Placeable List").at(0)->getString("Conversation"));
}

TEST_F(SavedTemplatesGame, should_read_no_installed_module_for_complete_records) {
    // given
    EXPECT_CALL(engine.resourceModule().director(), prepareModuleLoad(_, _)).Times(0);
    auto complete = git({with(savedDoor(), Gff::Field::newResRef("OnOpen", ""))},
                        {with(savedSceneObject(), Gff::Field::newDword("Appearance", 54))}, 0);

    // when
    auto restored = restore(complete);

    // then: the very same GIT
    EXPECT_EQ(complete, restored);
}

TEST_F(SavedTemplatesGame, should_keep_the_saved_git_when_the_installed_module_enters_another_area) {
    // given
    auto otherIfo = Gff::Builder()
                        .type(0xffffffff)
                        .field(Gff::Field::newCExoString("Mod_Entry_Area", "m01ab"))
                        .build();
    EXPECT_CALL(engine.resourceModule().director(), prepareModuleLoad("end_m01aa", IsNull()))
        .WillOnce(Return(ByMove(installed(otherIfo))));
    auto saved = git({savedDoor()}, {}, 0);

    // when
    auto restored = restore(saved);

    // then
    EXPECT_EQ(saved, restored);
}

TEST_F(SavedTemplatesGame, should_save_a_door_and_a_placeable_from_the_module_with_their_templates_fields) {
    // given: the door and the scene object as the module's own GIT loads them
    auto area = game.newArea();
    auto player = game.newCreature();
    TestGameModule::configureModuleSnapshot(game, area, player, "end_m01aa", "m01aa");
    TestGameModule::addSnapshotObject(*area, player);
    auto empty = Gff::Builder().type(0xffffffff).build();
    game.captureSaveResourceShadow({SaveResourceKind::ModuleIfo, "end_m01aa"}, *empty);
    game.captureSaveResourceShadow({SaveResourceKind::AreaAre, "m01aa"}, *empty);
    game.captureSaveResourceShadow({SaveResourceKind::AreaGit, "m01aa"}, *empty);
    auto door = game.newDoor();
    door->deserialize(*authoredDoor(), SerializedIdentityContext::templateResource("end_m01aa"));
    TestGameModule::addSnapshotObject(*area, door);
    auto sceneObject = game.newPlaceable();
    sceneObject->deserialize(*authoredSceneObject(), SerializedIdentityContext::templateResource("end_m01aa"));
    TestGameModule::addSnapshotObject(*area, sceneObject);
    // and Trask has picked the lock
    door->setLocked(false);

    // when
    auto saved = ModuleSnapshotBuilder(game, "end_m01aa").build();

    // then: the door's record has its template's scripts and conversation,
    // names the template, and keeps the door's state
    ASSERT_TRUE(saved) << saved.message;
    auto doorRecord = recordById(*saved.snapshot->git, "Door List", door->id());
    ASSERT_TRUE(doorRecord);
    EXPECT_EQ("k_pend_door19", doorRecord->getString("OnFailToOpen"));
    EXPECT_EQ("k_pend_door18", doorRecord->getString("OnOpen"));
    EXPECT_EQ("end_trask01", doorRecord->getString("Conversation"));
    EXPECT_EQ("end_door02", doorRecord->getString("TemplateResRef"));
    EXPECT_FALSE(doorRecord->getBool("Locked"));
    // and the scene object's, its appearance, conversation and script
    auto objectRecord = recordById(*saved.snapshot->git, "Placeable List", sceneObject->id());
    ASSERT_TRUE(objectRecord);
    EXPECT_EQ(54u, objectRecord->getUint("Appearance"));
    EXPECT_EQ("end_room3", objectRecord->getString("Conversation"));
    EXPECT_EQ("k_pend_resume", objectRecord->getString("OnUserDefined"));
    EXPECT_EQ("invisible002", objectRecord->getString("TemplateResRef"));

    // and loaded back as saved state, they have their scripts again
    auto reloadedDoor = game.newDoor();
    reloadedDoor->deserialize(*doorRecord, SerializedIdentityContext::moduleGraph("end_m01aa"));
    EXPECT_EQ("k_pend_door19", reloadedDoor->getOnFailToOpen());
    EXPECT_EQ("k_pend_door18", reloadedDoor->getOnOpen());
    EXPECT_EQ("end_trask01", reloadedDoor->conversation());
    EXPECT_FALSE(reloadedDoor->isLocked());
    auto reloadedObject = game.newPlaceable();
    reloadedObject->deserialize(*objectRecord, SerializedIdentityContext::moduleGraph("end_m01aa"));
    EXPECT_EQ(54, reloadedObject->appearance());
    EXPECT_EQ("end_room3", reloadedObject->conversation());
    EXPECT_EQ("k_pend_resume", reloadedObject->getOnUserDefined());
}

namespace {

std::shared_ptr<Gff> readGff(const std::optional<ByteBuffer> &bytes) {
    if (!bytes) {
        return nullptr;
    }
    ByteBuffer data(*bytes);
    MemoryInputStream stream(data);
    GffReader reader(stream);
    reader.load();
    return reader.root();
}

std::shared_ptr<Gff> recordByTag(const Gff &git, const std::string &list, const std::string &tag) {
    for (const auto &record : git.getList(list)) {
        if (boost::iequals(record->getString("Tag"), tag)) {
            return record;
        }
    }
    return nullptr;
}

} // namespace

// Runs only with the player's own files (#28):
//   REONE_TEST_KOTOR_DIR: KOTOR's folder, with modules/end_m01aa.rim and end_m01aa_s.rim
//   REONE_TEST_KOTOR_SAVE: a save folder with SAVEGAME.sav, holding end_m01aa
TEST(SavedTemplatesOnGameFiles, should_restore_the_endar_spire_door_and_scene_object) {
    const char *gameDir = std::getenv("REONE_TEST_KOTOR_DIR");
    const char *saveDir = std::getenv("REONE_TEST_KOTOR_SAVE");
    if (!gameDir || !saveDir) {
        GTEST_SKIP() << "Set REONE_TEST_KOTOR_DIR and REONE_TEST_KOTOR_SAVE to run against game files";
    }
    std::filesystem::path modules(std::filesystem::path(gameDir) / "modules");
    RimResourceContainer installed(Storage(modules / "end_m01aa.rim"));
    installed.init();
    RimResourceContainer blueprints(Storage(modules / "end_m01aa_s.rim"));
    blueprints.init();
    ErfResourceContainer save(Storage(std::filesystem::path(saveDir) / "SAVEGAME.sav"));
    save.init();
    auto moduleBytes = save.findResourceData(ResourceId("end_m01aa", ResType::Sav));
    ASSERT_TRUE(moduleBytes) << "The save holds no end_m01aa";
    ErfResourceContainer savedModule(Storage(std::move(*moduleBytes)));
    savedModule.init();
    auto savedGit = readGff(savedModule.findResourceData(ResourceId("m01aa", ResType::Git)));
    auto authoredGit = readGff(installed.findResourceData(ResourceId("m01aa", ResType::Git)));
    ASSERT_TRUE(savedGit);
    ASSERT_TRUE(authoredGit);
    if (!hasRecordsWithoutTemplateFields(*savedGit)) {
        GTEST_SKIP() << "This save's doors and placeables have their templates' fields";
    }

    // when
    auto repair = repairSavedTemplateFields(*savedGit, *authoredGit, [&](const std::string &resRef, ResType type) {
        return readGff(blueprints.findResourceData(ResourceId(resRef, type)));
    });

    // then: the locked door and the scene object behind it
    ASSERT_TRUE(repair.git);
    auto door = recordByTag(*repair.git, "Door List", "end_door02");
    ASSERT_TRUE(door);
    EXPECT_EQ("k_pend_door19", door->getString("OnFailToOpen"));
    EXPECT_EQ("k_pend_door18", door->getString("OnOpen"));
    EXPECT_EQ("end_trask01", door->getString("Conversation"));
    auto sceneObject = recordByTag(*repair.git, "Placeable List", "end01_sceneobj01");
    ASSERT_TRUE(sceneObject);
    EXPECT_EQ("end_room3", sceneObject->getString("Conversation"));
    EXPECT_EQ("k_pend_resume", sceneObject->getString("OnUserDefined"));
    EXPECT_EQ(54u, sceneObject->getUint("Appearance"));

    // and the records the module's own templates cover: every door, and the
    // placeables whose templates the module holds
    int doors = 0;
    for (const auto &record : repair.git->getList("Door List")) {
        doors += record->has("OnOpen") ? 1 : 0;
    }
    EXPECT_EQ(static_cast<int>(repair.git->getList("Door List").size()), doors);
    std::cout << "Restored " << repair.repaired << " records; unmatched:";
    for (const auto &record : repair.unmatched) {
        std::cout << " " << record;
    }
    std::cout << std::endl;
}
