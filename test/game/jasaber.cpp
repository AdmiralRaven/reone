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

#include "reone/game/action.h"
#include "reone/game/camerastyle.h"
#include "reone/game/game.h"
#include "reone/game/gui/dialog.h"
#include "reone/game/jasaber.h"
#include "reone/game/object/area.h"
#include "reone/game/object/camera/thirdperson.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/door.h"
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

input::Event mouseMotion(float xrel, float yrel) {
    return input::Event::newMouseMotion(input::MouseMotionEvent(0, 0, xrel, yrel));
}

input::Event keyDown(input::KeyCode code) {
    return input::Event::newKeyDown(input::KeyEvent(true, code, 0, false));
}

void expectNear(const glm::vec3 &expected, const glm::vec3 &actual, float tolerance) {
    EXPECT_NEAR(expected.x, actual.x, tolerance);
    EXPECT_NEAR(expected.y, actual.y, tolerance);
    EXPECT_NEAR(expected.z, actual.z, tolerance);
}

// Turns reone's developer mode on or off for one test, and back after it.
class DeveloperMode {
public:
    DeveloperMode(reone::game::OptionsView &options, bool on) :
        _options(options),
        _was(options.game.developer) {
        _options.game.developer = on;
    }

    ~DeveloperMode() {
        _options.game.developer = _was;
    }

private:
    reone::game::OptionsView &_options;
    bool _was;
};

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

    ThirdPersonCamera &thirdPersonCamera() {
        return *game.module()->area()->getCamera<ThirdPersonCamera>(reone::game::CameraType::ThirdPerson);
    }

    void play(float seconds) {
        for (int frame = 0; frame < static_cast<int>(seconds * 60.0f + 0.5f); ++frame) {
            game.update(1.0f / 60.0f);
        }
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

TEST_F(JaSaberMode, should_tilt_the_view_with_the_mouse_within_75_degrees) {
    // given
    float start = game.jaSaber().pitch();

    // when: the mouse moves 100 units toward the player
    EXPECT_TRUE(game.jaSaber().handle(mouseMotion(0.0f, 100.0f)));

    // then: the view looks down as far as the same move sideways would turn it
    EXPECT_NEAR(start + 100.0f * JaSaber::kDefaultMouseTurn, game.jaSaber().pitch(), 1e-5f);

    // and however far the mouse goes, no further than JA's 75 degrees
    game.jaSaber().handle(mouseMotion(0.0f, 1e6f));
    EXPECT_NEAR(glm::radians(75.0f), game.jaSaber().pitch(), 1e-5f);
    game.jaSaber().handle(mouseMotion(0.0f, -1e6f));
    EXPECT_NEAR(-glm::radians(75.0f), game.jaSaber().pitch(), 1e-5f);
}

TEST_F(JaSaberMode, should_glide_from_kotor_camera_into_ja_camera_and_hand_it_back) {
    // given: KOTOR's default camera, 3.2 m behind and 0.45 m above what it
    // aims at
    auto &camera = thirdPersonCamera();
    game.jaSaber().leave();
    camera.setStyle(CameraStyle {"", 3.2f, 83.0f, 0.45f, 55.0f});

    // when
    game.jaSaber().enter();

    // then: JA's camera starts where KOTOR's stands, tilted as it looks
    ASSERT_NE(nullptr, game.jaSaber().camera());
    expectNear(camera.shownView().eye, game.jaSaber().camera()->eye(), 1e-5f);
    EXPECT_NEAR(std::atan2(0.45f, 3.15f), game.jaSaber().pitch(), 1e-4f);

    // and when a second passes
    play(1.0f);

    // then: the camera shows JA's view, 80 JA units from its aim point
    const JaCamera &view = *game.jaSaber().camera();
    expectNear(view.eye(), camera.shownView().eye, 1e-5f);
    expectNear(view.aim(), camera.shownView().aim, 1e-5f);
    EXPECT_NEAR(80.0f * view.scale(), glm::distance(view.eye(), view.aim()), 0.005f);

    // and when saber mode ends
    game.jaSaber().leave();

    // then: KOTOR's own view is back, facing the same way
    float facing = camera.facing();
    expectNear(glm::vec3(3.15f * std::sin(facing), -3.15f * std::cos(facing), 0.45f),
               camera.shownView().eye - camera.shownView().aim, 1e-4f);
    EXPECT_EQ(nullptr, game.jaSaber().camera());
}

TEST_F(JaSaberMode, should_switch_between_the_view_behind_and_over_the_shoulder_with_v) {
    // when
    EXPECT_TRUE(game.jaSaber().handle(keyDown(input::KeyCode::V)));
    play(2.0f);

    // then: the camera has come round over the right shoulder
    EXPECT_TRUE(game.jaSaber().shoulderView());
    EXPECT_TRUE(printed("jasaber: camera over the right shoulder"));
    const JaCamera &view = *game.jaSaber().camera();
    auto settings = JaCamera::Settings::shoulder();
    float yaw = thirdPersonCamera().facing();
    glm::vec3 target(view.idealTarget(leader->position(), yaw, settings));
    expectNear(target, view.aim(), 1e-3f);
    expectNear(view.idealLocation(target, yaw, game.jaSaber().pitch(), settings), view.eye(), 1e-3f);

    // and the next time saber mode starts, it is still over the shoulder
    game.jaSaber().leave();
    game.jaSaber().enter();
    EXPECT_TRUE(game.jaSaber().shoulderView());

    // and V puts it back behind
    game.jaSaber().handle(keyDown(input::KeyCode::V));
    EXPECT_FALSE(game.jaSaber().shoulderView());
    EXPECT_TRUE(printed("jasaber: camera behind"));
}

TEST_F(JaSaberMode, should_take_v_through_the_game_with_developer_mode_on_or_off) {
    for (bool on : {false, true}) {
        // given: developer mode, whose own V switches to the free camera
        DeveloperMode developer(engine.options(), on);
        bool shoulder = game.jaSaber().shoulderView();

        // when: V reaches the game, as a key press in its window does
        EXPECT_TRUE(game.handle(keyDown(input::KeyCode::V))) << "developer mode " << on;
        play(0.1f);

        // then: saber mode took it, and is still on in the third-person camera
        EXPECT_NE(shoulder, game.jaSaber().shoulderView()) << "developer mode " << on;
        EXPECT_TRUE(game.jaSaber().isActive()) << "developer mode " << on;
        EXPECT_EQ(reone::game::CameraType::ThirdPerson, game.cameraType()) << "developer mode " << on;
    }
}

TEST_F(JaSaberMode, should_leave_v_to_the_developer_camera_once_saber_mode_is_off) {
    // given
    DeveloperMode developer(engine.options(), true);
    game.jaSaber().leave();

    // when
    EXPECT_TRUE(game.handle(keyDown(input::KeyCode::V)));

    // then: developer mode's free camera, and the saber's view as it was
    EXPECT_EQ(reone::game::CameraType::FirstPerson, game.cameraType());
    EXPECT_FALSE(game.jaSaber().shoulderView());
    EXPECT_FALSE(printed("jasaber: camera over the right shoulder"));
}

TEST_F(JaSaberMode, should_tilt_and_switch_the_camera_from_the_console) {
    // given
    TestGameModule::initConsole(game);
    float start = game.jaSaber().pitch();

    // when
    console.execute("jalook", {"20"});
    console.execute("jaview", {"shoulder"});

    // then
    EXPECT_NEAR(start + glm::radians(20.0f), game.jaSaber().pitch(), 1e-5f);
    EXPECT_TRUE(game.jaSaber().shoulderView());

    // and back
    console.execute("jaview", {"behind"});
    EXPECT_FALSE(game.jaSaber().shoulderView());

    // and jalook only tilts saber mode's camera
    game.jaSaber().leave();
    EXPECT_THROW(console.execute("jalook", {"10"}), std::runtime_error);
}

TEST_F(JaSaberMode, should_log_the_globals_whose_names_start_with_a_prefix) {
    // given
    TestGameModule::initConsole(game);
    game.setGlobalNumber("END_TRASK_DLG", 11);
    game.setGlobalNumber("K_CURRENT_PLANET", 5);

    // when
    console.execute("jaglobals", {"end_"});

    // then
    EXPECT_TRUE(printed("jaglobals: END_TRASK_DLG = 11"));
    EXPECT_FALSE(printed("jaglobals: K_CURRENT_PLANET = 5"));
}

TEST_F(JaSaberMode, should_act_on_the_nearest_object_with_a_tag_as_a_second_click_does) {
    // given: two doors tagged alike, with KOTOR's own controls
    game.jaSaber().leave();
    TestGameModule::initConsole(game);
    auto area = game.module()->area();
    auto nearDoor = game.newDoor();
    nearDoor->setTag("end_door02");
    nearDoor->setPosition(glm::vec3(2.0f, 0.0f, 0.0f));
    area->add(nearDoor);
    auto farDoor = game.newDoor();
    farDoor->setTag("end_door02");
    farDoor->setPosition(glm::vec3(20.0f, 0.0f, 0.0f));
    area->add(farDoor);

    // when
    console.execute("jaclick", {"end_door02"});

    // then: the leader goes to open the nearer door
    EXPECT_EQ(nearDoor, area->selectedObject());
    ASSERT_FALSE(leader->actions().empty());
    EXPECT_EQ(ActionType::OpenDoor, leader->actions().back()->type());
}

TEST_F(JaSaberMode, should_swing_top_to_bottom_from_ready_when_looking_more_than_30_degrees_down) {
    // given: the view level, then tilted 40 degrees down
    struct Case {
        float degrees;
        const char *start;
    };
    for (auto c : {Case {0.0f, "LS_S_R2L"}, Case {40.0f, "LS_S_T2B"}, Case {-40.0f, "LS_S_T2B"}}) {
        game.jaSaber().leave();
        game.jaSaber().enter();
        play(0.5f);
        game.jaSaber().look(glm::radians(c.degrees) - game.jaSaber().pitch());

        // when: the button goes down with no movement key held
        JaSaberInput attack;
        attack.attack = true;
        game.jaSaber().setScriptedInput(attack);
        play(1.0f / 60.0f);

        // then: level, ready's own right-to-left swing; looking far down or
        // up, top to bottom (bg_panimate.cpp:3981-3984)
        EXPECT_EQ(c.start, game.jaSaber().moveName()) << c.degrees;
        game.jaSaber().setScriptedInput(std::nullopt);
    }
}
