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
#include "../fixtures/jabody.h"

#include "reone/game/game.h"
#include "reone/game/gui/dialog.h"
#include "reone/game/jasaber.h"
#include "reone/game/object/area.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/module.h"
#include "reone/game/party.h"
#include "reone/graphics/options.h"
#include "reone/graphics/types.h"
#include "reone/resource/gff.h"
#include "reone/scene/graph.h"
#include "reone/scene/node/model.h"

using namespace reone;
using namespace reone::game;
using namespace reone::graphics;
using namespace reone::resource;
using namespace reone::scene;
using namespace reone::test;
using namespace testing;

void reone::game::TestGameModule::setSceneNode(Creature &creature, std::shared_ptr<scene::SceneNode> node) {
    creature._sceneNode = std::move(node);
}

namespace {

// A conversation that animates the party leader as it starts, as a
// conversation's first line does, and notes whether that worked.
class AnimatingDialogGUI : public DialogGUI {
public:
    AnimatingDialogGUI(Game &game, ServicesView &services, std::shared_ptr<Creature> leader, std::shared_ptr<Animation> animation) :
        DialogGUI(game, services),
        _owner(game),
        _leader(std::move(leader)),
        _animation(std::move(animation)) {
        _gui = std::make_shared<NiceMock<gui::MockGUI>>();
    }

    std::optional<bool> saberModeAtStart;
    std::optional<bool> animatedAtStart;

private:
    Game &_owner;
    std::shared_ptr<Creature> _leader;
    std::shared_ptr<Animation> _animation;

    void onStart() override {
        saberModeAtStart = _owner.jaSaber().isActive();
        animatedAtStart = _leader->playAnimation(_animation);
    }
    void onFinish() override {}
    void onLoadEntry() override {}
    void onEntryEnded() override {}
    void setMessage(std::string) override {}
    void setReplyLines(std::vector<std::string>) override {}
};

// Saber mode on a synthetic body in a test game: Jedi Academy's files are a
// synthetic skeleton and an animation.cfg naming every clip saber mode plays.
class JaSaberMode : public Test {
protected:
    void SetUp() override {
        ON_CALL(engine.sceneModule().graphs(), get(_)).WillByDefault(ReturnRef(*scene));

        folder = std::filesystem::temp_directory_path() / ("reone-jasaber-" + std::to_string(std::random_device()()));
        std::filesystem::create_directories(folder);
        std::ofstream(folder / "_humanoid.gla", std::ios::binary) << syntheticJaSkeleton().build();
        std::ofstream cfg(folder / "animation.cfg");
        auto clips = syntheticClips();
        for (auto &move : JaSaberMoves::fast().moves()) {
            clips[move.clip] = clips.at("MOVE");
        }
        for (auto &clip : JaSaberDriver::locomotionClips()) {
            clips[clip] = clips.at("LOOP");
        }
        for (auto &[name, clip] : clips) {
            cfg << name << " " << clip.firstFrame << " " << clip.numFrames << " " << clip.loopFrames << " " << clip.fps << "\n";
        }
        cfg.close();

        game.initLocalServices();
        auto area = game.newArea();
        area->initCameras(glm::vec3(0.0f), 0.0f);
        TestGameModule::setActiveModuleArea(game, area);
        TestGameModule::loadModulePlayer(*game.module());
        leader = game.newCreature();
        leader->setMaxHitPoints(10);
        leader->setCurrentHitPoints(10);
        game.party().addMember(kNpcPlayer, leader);
        game.party().setPlayer(leader);
        area->add(leader);

        // The leader's body, with a lightsaber in its right hand.
        body = syntheticKotorBody(false);
        model = std::make_shared<ModelSceneNode>(*body, ModelUsage::Creature, *scene,
                                                 engine.services().graphics, engine.services().audio, engine.services().resource);
        model->init();
        auto hilt = std::make_shared<ModelNode>(0, "test_saber", glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), true, nullptr);
        saberModel = std::make_shared<Model>("test_saber", MdlClassification::lightsaber, hilt, std::vector<std::shared_ptr<Animation>>(), "", 1.0f);
        saber = std::make_shared<ModelSceneNode>(*saberModel, ModelUsage::Equipment, *scene,
                                                 engine.services().graphics, engine.services().audio, engine.services().resource);
        saber->init();
        model->attach("rhand", *saber);
        TestGameModule::setSceneNode(*leader, model);

        game.openInGame();
        game.jaSaber().library().load(folder);
        game.jaSaber().enter();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);
    }

    bool printed(const std::string &line) const {
        return std::find(console.lines.begin(), console.lines.end(), line) != console.lines.end();
    }

    TestEngine &engine {testEngine()};
    StubConsole console;
    NiceMock<MockRenderPipelineFactory> pipelineFactory;
    GraphicsOptions graphicsOptions;
    std::unique_ptr<SceneGraph> scene {std::make_unique<SceneGraph>(
        "jasaber", pipelineFactory, graphicsOptions,
        engine.services().graphics, engine.services().audio, engine.services().resource)};
    Game game {GameID::KotOR, "", engine.options(), engine.services(), console};
    std::filesystem::path folder;
    std::shared_ptr<Creature> leader;
    std::shared_ptr<Model> body;
    std::shared_ptr<ModelSceneNode> model;
    std::shared_ptr<Model> saberModel;
    std::shared_ptr<ModelSceneNode> saber;
};

} // namespace

TEST_F(JaSaberMode, should_let_a_conversation_animate_the_character_from_its_start) {
    // given: saber mode holds the leader's animation
    ASSERT_TRUE(game.jaSaber().isActive());
    ASSERT_TRUE(leader->isAnimationHeld());
    auto talk = std::make_shared<Animation>("talk", 1.0f, 0.25f, "", body->rootNode(), std::vector<Animation::Event>());
    auto gui = std::make_unique<AnimatingDialogGUI>(game, engine.services(), leader, talk);
    auto *conversation = gui.get();
    TestGameModule::setDialogGUI(game, std::move(gui));
    auto dialog = std::make_shared<Dialog>();
    dialog->resRef = "jasaber_talk";
    Dialog::EntryReplyLink link;
    link.index = 0;
    dialog->startEntries.push_back(link);
    dialog->entries.resize(1);
    dialog->entries[0].text = "Hello there";
    // Two replies, so the conversation opens instead of barking a one-liner.
    dialog->entries[0].replies.push_back(link);
    dialog->entries[0].replies.push_back(link);
    dialog->replies.resize(1);
    dialog->replies[0].text = "Goodbye";
    EXPECT_CALL(engine.resourceModule().gffs(), get("jasaber_talk", ResType::Dlg))
        .Times(AnyNumber())
        .WillRepeatedly(Return(Gff::Builder().build()));
    EXPECT_CALL(static_cast<MockDialogs &>(engine.services().resource.dialogs), get("jasaber_talk"))
        .Times(AnyNumber())
        .WillRepeatedly(Return(dialog));

    // when
    game.startDialog(leader, "jasaber_talk");

    // then: saber mode let go before the conversation began, so its first
    // animation of the leader played
    ASSERT_TRUE(conversation->saberModeAtStart.has_value());
    EXPECT_FALSE(*conversation->saberModeAtStart);
    ASSERT_TRUE(conversation->animatedAtStart.has_value());
    EXPECT_TRUE(*conversation->animatedAtStart);
    EXPECT_FALSE(leader->isAnimationHeld());
    EXPECT_TRUE(printed("jasaber: a conversation"));
}

TEST_F(JaSaberMode, should_end_when_the_camera_changes_though_the_module_no_longer_drives_it) {
    // given
    ASSERT_TRUE(game.jaSaber().isActive());

    // when: the free camera takes over, so the module no longer hands its
    // updates to saber mode
    TestGameModule::initConsole(game);
    console.execute("camera", {"free"});
    ASSERT_EQ(reone::game::CameraType::FirstPerson, game.cameraType());
    game.update(1.0f / 60.0f);

    // then: saber mode let go, and left the mouse to the free camera
    EXPECT_FALSE(game.jaSaber().isActive());
    EXPECT_FALSE(leader->isAnimationHeld());
    EXPECT_TRUE(printed("jasaber: the camera changed"));
    EXPECT_TRUE(game.relativeMouseMode());
}

TEST_F(JaSaberMode, should_end_on_a_screen_other_than_the_game_and_its_menus) {
    // when
    TestGameModule::setCurrentScreen(game, static_cast<int>(Game::Screen::Container));
    game.update(1.0f / 60.0f);

    // then
    EXPECT_FALSE(game.jaSaber().isActive());
    EXPECT_FALSE(leader->isAnimationHeld());
    EXPECT_TRUE(printed("jasaber: the game left play"));
}

TEST_F(JaSaberMode, should_only_pause_while_the_in_game_menu_is_open) {
    // given
    ASSERT_TRUE(game.relativeMouseMode());

    // when: the menu opens
    TestGameModule::setCurrentScreen(game, static_cast<int>(Game::Screen::InGameMenu));
    game.update(1.0f / 60.0f);

    // then: the mouse is free and saber mode waits
    EXPECT_TRUE(game.jaSaber().isActive());
    EXPECT_TRUE(leader->isAnimationHeld());
    EXPECT_FALSE(game.relativeMouseMode());

    // and when it closes, saber mode takes the mouse again
    TestGameModule::setCurrentScreen(game, static_cast<int>(Game::Screen::InGame));
    game.update(1.0f / 60.0f);
    EXPECT_TRUE(game.jaSaber().isActive());
    EXPECT_TRUE(game.relativeMouseMode());
}
