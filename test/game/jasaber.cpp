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
#include "reone/game/action/attackobject.h"
#include "reone/game/camerastyle.h"
#include "reone/game/effect/acincrease.h"
#include "reone/game/effect/damage.h"
#include "reone/game/game.h"
#include "reone/game/gui/dialog.h"
#include "reone/game/jaconsole.h"
#include "reone/game/jasaber.h"
#include "reone/game/object/area.h"
#include "reone/game/object/camera/thirdperson.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/door.h"
#include "reone/game/object/item.h"
#include "reone/game/object/module.h"
#include "reone/game/party.h"
#include "reone/game/saveprovenance.h"
#include "reone/graphics/options.h"
#include "reone/graphics/types.h"
#include "reone/resource/2da.h"
#include "reone/resource/gff.h"
#include "reone/scene/graph.h"
#include "reone/scene/node/model.h"
#include "reone/system/randomutil.h"

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

void reone::game::TestGameModule::setCameraStyle(Area &area, CameraStyle style) {
    area._camStyleDefault = std::move(style);
}

namespace {

input::Event mouseMotion(float xrel, float yrel) {
    return input::Event::newMouseMotion(input::MouseMotionEvent(0, 0, xrel, yrel));
}

input::Event keyDown(input::KeyCode code) {
    return input::Event::newKeyDown(input::KeyEvent(true, code, 0, false));
}

input::Event wheel(float notches, input::MouseWheelDirection direction = input::MouseWheelDirection::Normal) {
    return input::Event::newMouseWheel(input::MouseWheelEvent(0.0f, notches, direction));
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
        saberModel = syntheticSaber();
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

TEST_F(JaSaberMode, should_open_the_options_menu_on_escape_and_wait_behind_it) {
    // when: the player presses Escape, in saber mode
    EXPECT_TRUE(game.handle(keyDown(input::KeyCode::Escape)));
    play(0.5f);

    // then: the options menu is open, and saber mode waits with the mouse
    // free; the pause key's pause isn't taken
    EXPECT_EQ(Game::Screen::InGameMenu, game.currentScreen());
    EXPECT_TRUE(game.isInPlayerMenu());
    EXPECT_TRUE(game.jaSaber().isActive());
    EXPECT_FALSE(game.relativeMouseMode());
    EXPECT_FALSE(game.isPausedByPlayer());

    // when: Escape again
    EXPECT_TRUE(game.handle(keyDown(input::KeyCode::Escape)));
    play(0.5f);

    // then: play goes on, in saber mode
    EXPECT_EQ(Game::Screen::InGame, game.currentScreen());
    EXPECT_FALSE(game.isInPlayerMenu());
    EXPECT_TRUE(game.jaSaber().isActive());
    EXPECT_TRUE(game.relativeMouseMode());
}

TEST_F(JaSaberMode, should_let_go_of_walking_and_the_mouse_when_the_escape_menu_opens) {
    // given: out of saber mode, walking forward, the mouse held
    game.jaSaber().leave("the test leaves saber mode");
    game.update(1.0f / 60.0f);
    EXPECT_TRUE(game.handle(keyDown(input::KeyCode::W)));
    ASSERT_TRUE(game.module()->player().isMovementRequested());
    game.setRelativeMouseMode(true);

    // when: Escape opens the menu, which takes the walking key's release,
    // and Escape closes it
    EXPECT_TRUE(game.handle(keyDown(input::KeyCode::Escape)));
    EXPECT_FALSE(game.relativeMouseMode());
    EXPECT_TRUE(game.handle(keyDown(input::KeyCode::Escape)));

    // then: the character doesn't walk on by itself
    EXPECT_FALSE(game.module()->player().isMovementRequested());
}

TEST_F(JaSaberMode, should_keep_the_pause_keys_pause_through_the_escape_menu) {
    // given: paused by the pause key, out of saber mode
    game.jaSaber().leave("the test leaves saber mode");
    game.setPaused(true);

    // when: the options menu opens and closes
    EXPECT_TRUE(game.handle(keyDown(input::KeyCode::Escape)));
    EXPECT_TRUE(game.handle(keyDown(input::KeyCode::Escape)));

    // then: the game is still paused
    EXPECT_EQ(Game::Screen::InGame, game.currentScreen());
    EXPECT_TRUE(game.isPaused());
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

TEST_F(JaSaberMode, should_stand_as_far_back_as_kotor_camera_and_zoom_with_the_wheel) {
    // given: the area's camera style, KOTOR's default on the Endar Spire, 3.2 m back
    TestGameModule::setCameraStyle(*game.module()->area(), CameraStyle {"", 3.2f, 83.0f, 0.45f, 55.0f});
    auto distance = [this]() {
        const JaCamera &view = *game.jaSaber().camera();
        return glm::distance(view.eye(), view.aim());
    };

    // when
    play(2.0f);

    // then: JA's camera stands as far behind its aim point as KOTOR's
    EXPECT_NEAR(3.2f, game.jaSaber().cameraRange(), 1e-5f);
    EXPECT_NEAR(3.2f, distance(), 0.005f);

    // when: the wheel turns one notch away
    EXPECT_TRUE(game.jaSaber().handle(wheel(1.0f)));
    play(2.0f);

    // then: a tenth nearer
    EXPECT_NEAR(3.2f / 1.1f, distance(), 0.005f);
    EXPECT_TRUE(printed("jasaber: camera range 2.91 m behind, 2.25 m over the shoulder"));

    // when: three notches toward the player, on a mouse whose wheel is flipped
    game.jaSaber().handle(wheel(-3.0f, input::MouseWheelDirection::Flipped));
    play(2.0f);

    // then: three notches nearer still
    EXPECT_NEAR(3.2f / std::pow(1.1f, 4.0f), distance(), 0.005f);

    // and over the shoulder, the camera keeps JA's share of the range, with
    // the character as far to the side on the screen
    game.jaSaber().setShoulderView(true);
    play(2.0f);
    float range = 3.2f / std::pow(1.1f, 4.0f) * 62.0f / 80.0f;
    EXPECT_NEAR(range, distance(), 0.005f);
    const JaCamera &view = *game.jaSaber().camera();
    glm::vec3 target(view.idealTarget(leader->position(), thirdPersonCamera().facing(),
                                      JaCamera::Settings::shoulder().atRange(range / view.scale())));
    expectNear(target, view.aim(), 1e-3f);

    // and the zoom stops at half and twice KOTOR's distance, and lasts into
    // the next saber mode
    for (int i = 0; i < 50; ++i) {
        game.jaSaber().handle(wheel(-1.0f));
    }
    game.jaSaber().leave();
    game.jaSaber().enter();
    EXPECT_NEAR(6.4f, game.jaSaber().cameraRange(), 1e-4f);
    for (int i = 0; i < 50; ++i) {
        game.jaSaber().handle(wheel(1.0f));
    }
    EXPECT_NEAR(1.6f, game.jaSaber().cameraRange(), 1e-4f);
}

TEST_F(JaSaberMode, should_set_the_camera_range_from_the_console) {
    // given
    TestGameModule::initConsole(game);
    TestGameModule::setCameraStyle(*game.module()->area(), CameraStyle {"", 3.2f, 83.0f, 0.45f, 55.0f});

    // when
    console.execute("jarange", {"4"});

    // then
    EXPECT_NEAR(4.0f, game.jaSaber().cameraRange(), 1e-5f);
    EXPECT_TRUE(printed("jarange: 4.00 m behind the aim point; KOTOR's camera stands 3.20 m back"));

    // and back to KOTOR's
    console.execute("jarange", {"kotor"});
    EXPECT_NEAR(3.2f, game.jaSaber().cameraRange(), 1e-5f);

    // and only in saber mode
    game.jaSaber().leave();
    EXPECT_THROW(console.execute("jarange", {"4"}), std::runtime_error);
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

TEST_F(JaSaberMode, should_keep_space_from_pausing_the_game) {
    // when: Space reaches the game, as a key press in its window does
    EXPECT_TRUE(game.handle(keyDown(input::KeyCode::Space)));
    play(0.1f);

    // then: saber mode took it, and the game runs on
    EXPECT_FALSE(game.isPaused());
    EXPECT_TRUE(game.jaSaber().isActive());
    EXPECT_TRUE(printed("jasaber: Space would jump, which saber mode doesn't do yet"));

    // and with the saber away, Space is KOTOR's pause again
    game.jaSaber().leave();
    EXPECT_TRUE(game.handle(keyDown(input::KeyCode::Space)));
    EXPECT_TRUE(game.isPaused());
    EXPECT_TRUE(game.handle(keyDown(input::KeyCode::Space)));
    EXPECT_FALSE(game.isPaused());
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

TEST_F(JaSaberMode, should_keep_the_saber_lit_until_saber_mode_ends) {
    // given: saber mode keeps the leader's saber on
    EXPECT_TRUE(leader->lightsabersKeptOn());

    // when
    game.jaSaber().leave();

    // then: KOTOR's combat decides again
    EXPECT_FALSE(leader->lightsabersKeptOn());
}

TEST_F(JaSaberMode, should_cut_as_thick_as_jas_traces_at_the_bodys_scale) {
    float scale = game.jaSaber().library().body(*body).retargeter->scale();
    EXPECT_FLOAT_EQ(kJaBladeTraceUnits * scale, game.jaSaber().bladeRadius());
    EXPECT_NE(kJaBladeRadius, game.jaSaber().bladeRadius());
}

TEST_F(JaSaberMode, should_drop_an_attack_of_kotors_own_so_that_only_the_blade_hits) {
    // given: an attack of KOTOR's own, queued for the leader as a combat round
    // queues one when its target falls
    auto other = game.newCreature();
    game.module()->area()->add(other);
    leader->addAction(game.newAction<AttackObjectAction>(other));
    ASSERT_FALSE(leader->actions().empty());

    // when
    play(1.0f / 60.0f);

    // then
    EXPECT_TRUE(leader->actions().empty());
}

TEST_F(JaSaberMode, should_drop_a_kotor_swing_already_begun_in_saber_mode_and_as_it_starts) {
    // given: an attack of KOTOR's own whose swing has begun, which locks its
    // action against an ordinary clear
    auto other = game.newCreature();
    game.module()->area()->add(other);
    auto swing = game.newAction<AttackObjectAction>(other);
    swing->lock();
    leader->addAction(swing);

    // when: a tick of saber mode
    play(1.0f / 60.0f);

    // then: it is cancelled, its blow with it, and gone
    EXPECT_TRUE(swing->isCancelled());
    EXPECT_TRUE(leader->actions().empty());

    // and when saber mode starts during such a swing
    game.jaSaber().leave();
    auto another = game.newAction<AttackObjectAction>(other);
    another->lock();
    leader->addAction(another);
    game.jaSaber().enter();

    // then: the swing is dropped as saber mode takes the character over
    EXPECT_TRUE(another->isCancelled());
    EXPECT_TRUE(leader->actions().empty());
}

namespace {

// Saber mode with creatures standing round for its blade to hit, which are
// hostile to the party leader as the test says.
class JaSaberHits : public JaSaberMode {
protected:
    void SetUp() override {
        JaSaberMode::SetUp();
        EXPECT_CALL(reputes(), getIsEnemy(An<const Creature &>(), An<const Creature &>()))
            .Times(AnyNumber())
            .WillRepeatedly(Invoke([this](const Creature &source, const Creature &target) {
                return hostile.count(source.id()) > 0 || hostile.count(target.id()) > 0;
            }));
    }

    void TearDown() override {
        Mock::VerifyAndClearExpectations(&reputes());
        JaSaberMode::TearDown();
    }

    MockReputes &reputes() {
        return static_cast<MockReputes &>(engine.services().game.reputes);
    }

    // A creature with 20 hit points at \p at, \p width across: a box body,
    // one upright capsule half as round, 3 m tall. It is of \p faction, or
    // else hostile_1 when hostile and neutral when not.
    std::shared_ptr<Creature> addCreature(const std::string &tag, const glm::vec2 &at, float width, bool isHostile,
                                          std::optional<Faction> faction = std::nullopt,
                                          std::shared_ptr<Item> weapon = nullptr) {
        auto creature = game.newCreature();
        // Equipped before it has a model, which equipping would rebuild from
        // KOTOR's appearance table.
        if (weapon) {
            EXPECT_TRUE(creature->equip(InventorySlots::rightWeapon, weapon));
        }
        creature->setTag(tag);
        creature->setFaction(faction.value_or(isHostile ? Faction::Hostile1 : Faction::Neutral));
        creature->setMaxHitPoints(20);
        creature->setCurrentHitPoints(20);
        bodies.push_back(syntheticBoxBody(width, 3.0f));
        auto node = std::make_shared<ModelSceneNode>(*bodies.back(), ModelUsage::Creature, *scene,
                                                     engine.services().graphics, engine.services().audio, engine.services().resource);
        node->init();
        TestGameModule::setSceneNode(*creature, node);
        creature->setPosition(glm::vec3(at, 0.0f));
        game.module()->area()->add(creature);
        if (isHostile) {
            hostile.insert(creature->id());
        }
        nodes.push_back(node);
        return creature;
    }

    std::set<uint32_t> hostile;
    std::vector<std::shared_ptr<Model>> bodies;
    std::vector<std::shared_ptr<ModelSceneNode>> nodes;
};

} // namespace

TEST_F(JaSaberHits, should_hit_a_hostile_creature_the_blade_reaches_and_pass_through_a_friendly_one) {
    // given: the blade lit in the hand
    play(1.0f / 60.0f);
    auto blades = game.jaSaber().blades();
    ASSERT_EQ(1u, blades.size());
    // two bodies 5 m across round the leader, so the blade is inside both
    // wherever the swing takes it, one hostile and one not; and a hostile
    // one out of reach
    auto enemy = addCreature("enemy", glm::vec2(0.0f, 0.3f), 5.0f, true);
    auto friendly = addCreature("friendly", glm::vec2(0.0f, 0.3f), 5.0f, false);
    auto far = addCreature("far", glm::vec2(0.0f, 6.0f), 1.0f, true);

    // when: the leader swings, a start, then the attack
    JaSaberInput attack;
    attack.attack = true;
    game.jaSaber().setScriptedInput(attack);
    play(1.5f);
    game.jaSaber().setScriptedInput(std::nullopt);

    // then: the blade hit the enemy once, and as it can't block, killed it
    EXPECT_EQ(1, game.jaSaber().hitCount());
    EXPECT_TRUE(enemy->isDead());
    // and passed through the creature on the leader's side
    EXPECT_FALSE(friendly->isDead());
    EXPECT_EQ(20, friendly->currentHitPoints());
    // and didn't reach the one 6 m away
    EXPECT_EQ(20, far->currentHitPoints());
}

TEST_F(JaSaberHits, should_hit_nothing_while_the_leader_stands_ready) {
    // given: a hostile body round the leader
    auto enemy = addCreature("enemy", glm::vec2(0.0f, 0.3f), 5.0f, true);

    // when: the leader stands in the ready stance, no button held
    play(1.0f);

    // then
    EXPECT_EQ(0, game.jaSaber().hitCount());
    EXPECT_EQ(20, enemy->currentHitPoints());
}

TEST_F(JaSaberHits, should_turn_to_the_nearest_enemy_and_swing_until_none_is_left_when_engaging) {
    // given: an enemy 1 m to the right, within reach, and a friendly creature
    // nearer, to the left, both in the leader's sight
    auto enemy = addCreature("enemy", glm::vec2(1.0f, 0.0f), 5.0f, true);
    auto friendly = addCreature("friendly", glm::vec2(-0.5f, 0.0f), 1.0f, false);
    TestGameModule::updatePerception(*game.module()->area());

    // when
    game.jaSaber().setEngage(true);
    play(1.0f / 60.0f);

    // then: the camera, and so the character, faces the enemy, not the
    // nearer friendly creature: facing f looks along (-sin f, cos f)
    EXPECT_NEAR(1.5f * glm::pi<float>(), thirdPersonCamera().facing(), 1e-4f);

    // and when it has had time to swing
    play(2.0f);

    // then: it killed the enemy, once, and spared the friendly creature
    EXPECT_TRUE(enemy->isDead());
    EXPECT_EQ(1, game.jaSaber().hitCount());
    EXPECT_EQ(20, friendly->currentHitPoints());
    EXPECT_TRUE(game.jaSaber().engaging());
}

TEST_F(JaSaberHits, should_engage_only_an_enemy_the_character_sees) {
    // given: an enemy 30 m to the right, past the leader's sight of 20 m
    auto enemy = addCreature("enemy", glm::vec2(30.0f, 0.0f), 1.0f, true);
    TestGameModule::updatePerception(*game.module()->area());
    float facing = thirdPersonCamera().facing();

    // when
    game.jaSaber().setEngage(true);
    play(0.5f);

    // then: engaging leaves it alone: no turn, no run, no swing
    EXPECT_FLOAT_EQ(facing, thirdPersonCamera().facing());
    EXPECT_NEAR(0.0f, glm::length(glm::vec2(leader->position())), 1e-4f);
    EXPECT_EQ("LS_READY", game.jaSaber().moveName());

    // and when it comes within sight, 5 m to the left
    enemy->setPosition(glm::vec3(-5.0f, 0.0f, 0.0f));
    TestGameModule::updatePerception(*game.module()->area());
    play(1.0f / 60.0f);

    // then: engaging turns to it: facing f looks along (-sin f, cos f)
    EXPECT_NEAR(0.5f * glm::pi<float>(), thirdPersonCamera().facing(), 1e-4f);
}

TEST_F(JaSaberHits, should_pass_through_a_republic_soldier_kotors_factions_make_hostile) {
    // given: in the same place, a Sith soldier of hostile_1, and a Republic
    // soldier of friendly_2, whom KOTOR's factions make hostile to the party
    // leader, of friendly_1, as on the Endar Spire past its first locked door
    auto sith = addCreature("end_sith3", glm::vec2(0.0f, 0.3f), 5.0f, true);
    auto soldier = addCreature("end_soldier2", glm::vec2(0.0f, 0.3f), 5.0f, true, Faction::Friendly2);

    // when: the leader swings
    JaSaberInput attack;
    attack.attack = true;
    game.jaSaber().setScriptedInput(attack);
    play(1.5f);
    game.jaSaber().setScriptedInput(std::nullopt);

    // then: the blade killed the Sith, and passed through the soldier
    EXPECT_TRUE(sith->isDead());
    EXPECT_EQ(1, game.jaSaber().hitCount());
    EXPECT_EQ(20, soldier->currentHitPoints());
}

TEST_F(JaSaberHits, should_not_engage_a_republic_soldier_kotors_factions_make_hostile) {
    // given: such a soldier 1 m to the left, and a Sith farther, 3 m to the
    // right, both in the leader's sight
    auto soldier = addCreature("end_soldier2", glm::vec2(-1.0f, 0.0f), 1.0f, true, Faction::Friendly2);
    auto sith = addCreature("end_sith3", glm::vec2(3.0f, 0.0f), 1.0f, true);
    TestGameModule::updatePerception(*game.module()->area());

    // when
    game.jaSaber().setEngage(true);
    play(1.0f / 60.0f);

    // then: engaging turns to the Sith, past the nearer soldier: facing f
    // looks along (-sin f, cos f)
    EXPECT_NEAR(1.5f * glm::pi<float>(), thirdPersonCamera().facing(), 1e-4f);
}

TEST_F(JaSaberHits, should_report_the_enemies_left_in_sight_as_engaging_ends) {
    // given: the console's commands; a Sith 1 m to the right, in reach and
    // in sight, a Republic soldier of friendly_2 2 m to the left, in sight,
    // and a Sith 30 m away, past the leader's sight of 20 m
    registerJaConsoleCommands(game, console);
    auto sith = addCreature("end_sith3", glm::vec2(1.0f, 0.0f), 5.0f, true);
    addCreature("end_soldier2", glm::vec2(-2.0f, 0.0f), 1.0f, true, Faction::Friendly2);
    addCreature("end_sith2", glm::vec2(0.0f, 30.0f), 1.0f, true);
    TestGameModule::updatePerception(*game.module()->area());

    // when: engaging is turned off
    console.execute("jaengage", {"off"});

    // then: it names the one enemy the blade may hit in sight
    EXPECT_EQ("jaengage: off, the keyboard and mouse again; enemies in sight: end_sith3 1.00 m", console.lines.back());

    // and when engaging has killed it
    console.execute("jaengage", {"on"});
    play(2.0f);
    ASSERT_TRUE(sith->isDead());
    console.execute("jaengage", {"off"});

    // then: none is left in sight, though one lives out of sight
    EXPECT_EQ("jaengage: off, the keyboard and mouse again; enemies in sight: none", console.lines.back());

    // and jacreature tells the dead from the living by tag, whatever its case
    console.execute("jacreature", {"end_sith3"});
    EXPECT_EQ(0u, console.lines.back().find("jacreature: end_sith3 dead, hit points ")) << console.lines.back();
    console.execute("jacreature", {"END_SITH2"});
    EXPECT_EQ(0u, console.lines.back().find("jacreature: end_sith2 alive, hit points 20 of 20, ")) << console.lines.back();
    EXPECT_NE(std::string::npos, console.lines.back().find(" m from the party leader")) << console.lines.back();
    console.execute("jacreature", {"end_sith9"});
    EXPECT_EQ("jacreature: no creature tagged end_sith9 in the area", console.lines.back());
}

TEST_F(JaSaberHits, should_list_the_enemies_in_sight_nearest_first) {
    // given: three enemies in sight, 3 m, 1 m and 2 m away; and, not to be
    // listed, one that isn't hostile, a Republic soldier of friendly_2, one
    // 30 m away, past the leader's sight, and one that died since the
    // leader's perception last looked, as the blade's kills do
    addCreature("sith_3m", glm::vec2(0.0f, 3.0f), 0.5f, true);
    addCreature("sith_1m", glm::vec2(1.0f, 0.0f), 0.5f, true);
    addCreature("sith_2m", glm::vec2(-2.0f, 0.0f), 0.5f, true);
    auto dead = addCreature("dead", glm::vec2(0.0f, -1.5f), 0.5f, true);
    addCreature("neutral", glm::vec2(0.0f, -2.5f), 0.5f, false);
    addCreature("end_soldier2", glm::vec2(1.5f, 1.5f), 0.5f, true, Faction::Friendly2);
    addCreature("out_of_sight", glm::vec2(0.0f, 30.0f), 0.5f, true);
    TestGameModule::updatePerception(*game.module()->area());
    ASSERT_TRUE(leader->perception().sees(dead->id()));
    dead->damage(100, nullptr);
    ASSERT_TRUE(dead->isDead());

    // when
    auto enemies = game.jaSaber().enemiesInSight(*leader);

    // then
    std::vector<std::string> tags;
    for (auto &enemy : enemies) {
        tags.push_back(enemy->tag());
    }
    EXPECT_EQ((std::vector<std::string> {"sith_1m", "sith_2m", "sith_3m"}), tags);
}

TEST_F(JaSaberHits, should_go_for_the_nearest_of_the_enemies_in_sight) {
    // given: an enemy 1 m to the right and one 3 m to the left, both in sight
    addCreature("near", glm::vec2(1.0f, 0.0f), 1.0f, true);
    addCreature("far", glm::vec2(-3.0f, 0.0f), 1.0f, true);
    TestGameModule::updatePerception(*game.module()->area());

    // when
    game.jaSaber().setEngage(true);
    play(1.0f / 60.0f);

    // then: engaging turns to the nearer, on the right: facing f looks along
    // (-sin f, cos f)
    EXPECT_NEAR(1.5f * glm::pi<float>(), thirdPersonCamera().facing(), 1e-4f);
}

TEST_F(JaSaberHits, should_wound_a_creature_too_big_to_cut_down_not_kill_it) {
    // given: a hostile creature round the leader, with 20 hit points and no
    // weapon, of a large row of KOTOR's appearance table, as a rancor is
    EXPECT_CALL(engine.resourceModule().twoDas(), get("appearance"))
        .Times(AnyNumber())
        .WillRepeatedly(Return(sizedAppearances()));
    auto big = addCreature("big", glm::vec2(0.0f, 0.3f), 5.0f, true);
    big->setAppearance(2);
    big->loadAppearance();
    ASSERT_TRUE(jaTooBigToCut(*big));

    // when: the leader swings
    JaSaberInput attack;
    attack.attack = true;
    game.jaSaber().setScriptedInput(attack);
    play(1.5f);
    game.jaSaber().setScriptedInput(std::nullopt);

    // then: the blade hit it once in each attack the held button chained,
    // each time for the weapon's damage alone, here the leader's bare
    // hand's, at least 1; and it lives
    int hits = game.jaSaber().hitCount();
    EXPECT_GE(hits, 1);
    EXPECT_FALSE(big->isDead());
    EXPECT_LE(big->currentHitPoints(), 20 - hits);
    EXPECT_GT(big->currentHitPoints(), 0);

    // The engine's mocks are shared by every test: later ones find no
    // appearance table, as before this one.
    EXPECT_CALL(engine.resourceModule().twoDas(), get("appearance")).Times(AnyNumber());
}

// Enemies in real time, and the saber's defense

namespace {

// KOTOR's base items as these tests need them, at their retail rows: a
// vibroblade and a blaster pistol.
std::shared_ptr<TwoDA> foeWeaponTable() {
    TwoDA::Builder builder;
    builder.columns({"maxattackrange", "crithitmult", "critthreat", "damageflags", "dietoroll",
                     "equipableslots", "itemclass", "numdice", "weapontype", "weaponwield",
                     "ammunitiontype", "bodyvar"});
    for (int i = 0; i <= 12; ++i) {
        if (i == 5) {
            builder.row({"1.5", "2", "2", "2", "10", "48", "w_vbroshort", "1", "1", "2", "-1", ""});
        } else if (i == 12) {
            builder.row({"23", "2", "2", "2", "6", "48", "w_blstrpstl", "1", "4", "4", "-1", ""});
        } else {
            builder.row({"", "", "", "", "", "", "", "", "", "", "", ""});
        }
    }
    return builder.build();
}

constexpr int kVibroblade = 5;
constexpr int kBlasterPistol = 12;

// Draws that always come up as \p pick says, from the range asked.
JaRandom drawing(std::function<int(int, int)> pick) {
    return [pick = std::move(pick)](int low, int high) { return glm::clamp(pick(low, high), low, high); };
}

JaRandomFloat lowest() {
    return [](float low, float) { return low; };
}

// Saber mode with enemies that attack the leader, KOTOR's way: an attack
// action on it, which the real-time foes take over.
class JaSaberFoes : public JaSaberHits {
protected:
    void SetUp() override {
        JaSaberHits::SetUp();
        EXPECT_CALL(engine.resourceModule().twoDas(), get("baseitems"))
            .Times(AnyNumber())
            .WillRepeatedly(Return(weapons));
        leader->setMaxHitPoints(100);
        leader->setCurrentHitPoints(100);
        // A Jedi of level 4 or more: Jedi Defense and Advanced Jedi Defense,
        // defense rank 2.
        leader->attributes().addFeat(FeatType::JediDefense);
        leader->attributes().addFeat(FeatType::AdvancedJediDefense);
        // The blade lit, and the guard with it.
        play(1.0f / 60.0f);
    }

    void TearDown() override {
        // The engine's mocks are shared by every test: later ones find no
        // base items, as before these.
        EXPECT_CALL(engine.resourceModule().twoDas(), get("baseitems")).Times(AnyNumber());
        JaSaberHits::TearDown();
    }

    std::shared_ptr<Item> weapon(int baseItem) {
        auto gff = Gff::Builder()
                       .field(Gff::Field::newCExoString("Tag", "test_weapon_" + std::to_string(baseItem)))
                       .field(Gff::Field::newInt("BaseItem", baseItem))
                       .field(Gff::Field::newWord("StackSize", 1))
                       .build();
        auto item = game.newItem();
        item->deserialize(*gff, SerializedIdentityContext::templateResource());
        return item;
    }

    // A hostile creature with \p baseItem in hand at \p at, or none, whose
    // attacks always hit, unless \p assuredHit is false, attacking the leader.
    std::shared_ptr<Creature> addFoe(const std::string &tag, const glm::vec2 &at, std::optional<int> baseItem, bool assuredHit = true) {
        auto foe = addCreature(tag, at, 1.0f, true, std::nullopt, baseItem ? weapon(*baseItem) : nullptr);
        if (assuredHit) {
            foe->applyAssuredHit();
        }
        foe->addAction(game.newAction<AttackObjectAction>(leader));
        return foe;
    }

    JaFoes &foes() {
        return game.jaSaber().foes();
    }

    std::shared_ptr<TwoDA> weapons {foeWeaponTable()};
};

} // namespace

TEST_F(JaSaberFoes, should_take_the_defense_rank_from_kotors_jedi_defense_feats) {
    // given: the fixture's leader, with Jedi Defense and Advanced Jedi Defense
    EXPECT_TRUE(game.jaSaber().defenseFromFeats());
    EXPECT_EQ(2, game.jaSaber().defense());

    // when: Master Jedi Defense, at level 8
    leader->attributes().addFeat(FeatType::MasterJediDefense);

    // then
    EXPECT_EQ(3, game.jaSaber().defense());

    // when: set by hand, then left to the feats again
    game.jaSaber().setDefense(1);
    EXPECT_FALSE(game.jaSaber().defenseFromFeats());
    EXPECT_EQ(1, game.jaSaber().defense());
    game.jaSaber().setDefenseFromFeats();
    EXPECT_EQ(3, game.jaSaber().defense());
}

TEST(JaSaberDefense, should_rank_the_highest_jedi_defense_feat_and_none_without_any) {
    TestEngine &engine = testEngine();
    StubConsole console;
    Game game(GameID::KotOR, "", engine.options(), engine.services(), console);
    auto creature = game.newCreature();
    EXPECT_EQ(0, jaDefenseFromFeats(*creature));
    creature->attributes().addFeat(FeatType::JediDefense);
    EXPECT_EQ(1, jaDefenseFromFeats(*creature));
    creature->attributes().addFeat(FeatType::AdvancedJediDefense);
    EXPECT_EQ(2, jaDefenseFromFeats(*creature));
    creature->attributes().addFeat(FeatType::MasterJediDefense);
    EXPECT_EQ(3, jaDefenseFromFeats(*creature));

    // one feat alone stands for its rank, as a save may hold only the last
    auto master = game.newCreature();
    master->attributes().addFeat(FeatType::MasterJediDefense);
    EXPECT_EQ(3, jaDefenseFromFeats(*master));
}

TEST(JaFoesRules, should_see_a_bolt_coming_as_long_before_as_ja_does_its_enemies_bolts) {
    // JA's scanner looks 256 units out, and its enemies' E-11 bolts below
    // hard difficulty fly 1150 units a second: about 0.22 s
    EXPECT_NEAR(256.0f / 1150.0f, JaFoes::kBoltWarning, 1e-6f);
    EXPECT_NEAR(0.2226f, JaFoes::kBoltWarning, 1e-3f);
}

TEST_F(JaSaberFoes, should_shoot_on_its_own_clock_and_hit_the_character_from_behind) {
    // given: a blaster 6 m behind the leader, who looks the other way, and
    // the shortest pauses between volleys
    foes().setRandom(drawing([](int, int high) { return high; }), lowest());
    auto trooper = addFoe("trooper", glm::vec2(0.0f, -6.0f), kBlasterPistol);
    ASSERT_TRUE(foes().fightsInRealTime(*leader));

    // when
    play(3.0f);

    // then: it fired volleys in real time, more than KOTOR's one round of 3 s
    // gives, every bolt aimed, its attacks always hitting
    EXPECT_GE(foes().counts().volleys, 2);
    EXPECT_EQ(foes().counts().bolts, foes().counts().aimed);
    // and its bolts met the leader's back: no parry, no bolt sent back
    EXPECT_GE(foes().counts().hitCharacter, 2);
    EXPECT_EQ(0, foes().counts().sentBack);
    EXPECT_EQ(0, game.jaSaber().parryCount());
    EXPECT_LT(leader->currentHitPoints(), 100);
    EXPECT_EQ(20, trooper->currentHitPoints());
}

TEST_F(JaSaberFoes, should_parry_bolts_from_the_front_and_send_them_back_at_the_shooter) {
    // given: a blaster 6 m in front of the leader, at defense rank 3, with
    // draws that stop every bolt and send it back, at the current enemy
    game.jaSaber().setDefense(3);
    foes().setRandom(drawing([](int, int high) { return high; }), lowest());
    auto trooper = addFoe("trooper", glm::vec2(0.0f, 6.0f), kBlasterPistol);

    // when
    play(2.0f);

    // then: the saber saw the bolts coming and parried them, sent them back,
    // and they hit the trooper; none reached the leader
    EXPECT_GE(foes().counts().bolts, 1);
    EXPECT_GE(game.jaSaber().parryCount(), 1);
    EXPECT_GE(foes().counts().sentBack, 1);
    EXPECT_GE(foes().counts().hitEnemies, 1);
    EXPECT_EQ(0, foes().counts().hitCharacter);
    EXPECT_EQ(100, leader->currentHitPoints());
    EXPECT_LT(trooper->currentHitPoints(), 20);
}

TEST_F(JaSaberFoes, should_let_a_bolt_through_the_guard_when_its_draw_comes_up_0) {
    // given: draws from 0 to d² that come up 0, letting bolts through the guard
    game.jaSaber().setDefense(3);
    foes().setRandom(drawing([](int low, int) { return low; }), lowest());
    addFoe("trooper", glm::vec2(0.0f, 6.0f), kBlasterPistol);

    // when
    play(1.5f);

    // then: they went through to the body
    EXPECT_GE(foes().counts().passedGuard, 1);
    EXPECT_GE(foes().counts().hitCharacter, 1);
    EXPECT_EQ(0, foes().counts().sentBack);
    EXPECT_LT(leader->currentHitPoints(), 100);
}

TEST_F(JaSaberFoes, should_draw_once_for_a_bolt_that_meets_the_guard) {
    // given: at rank 3, the guard's first draw, from 0 to 9, comes up 0 and
    // lets a bolt through; every later one comes up 5 and would stop it
    game.jaSaber().setDefense(3);
    int guardDraws = 0;
    foes().setRandom(drawing([&guardDraws](int low, int high) {
        if (high != 9) {
            return low;
        }
        return guardDraws++ == 0 ? 0 : 5;
    }),
                     lowest());
    addFoe("trooper", glm::vec2(0.0f, 6.0f), kBlasterPistol);

    // when
    play(1.5f);

    // then: the first bolt reached the body, through the guard once
    EXPECT_EQ(1, foes().counts().passedGuard);
    EXPECT_EQ(1, foes().counts().hitCharacter);
    EXPECT_LT(leader->currentHitPoints(), 100);
}

TEST_F(JaSaberFoes, should_absorb_a_stopped_bolt_when_the_draw_to_send_it_back_comes_up_0) {
    // given: the guard's draw, from 0 to 9, comes up 5, stopping the bolt;
    // the draw to send it back, from 0 to 10, comes up 0
    game.jaSaber().setDefense(3);
    foes().setRandom(drawing([](int low, int high) { return high == 9 ? 5 : low; }), lowest());
    addFoe("trooper", glm::vec2(0.0f, 6.0f), kBlasterPistol);

    // when
    play(1.5f);

    // then
    EXPECT_GE(foes().counts().absorbed, 1);
    EXPECT_EQ(0, foes().counts().sentBack);
    EXPECT_EQ(0, foes().counts().hitCharacter);
    EXPECT_EQ(100, leader->currentHitPoints());
}

TEST_F(JaSaberFoes, should_aim_bolts_and_land_blows_only_as_kotors_roll_hits) {
    // given: a leader no roll but a natural 20 hits, with no defense; a
    // blaster and a vibroblade behind it whose attacks aren't assured; the
    // shortest pauses, and bolts that miss passing straight over the head,
    // as near the synthetic body's head as a miss can. Only KOTOR's roll is
    // left to chance, so the checks below hold whatever the platform's
    // random numbers, unless every roll comes up 20.
    setRandomSeed(36);
    game.jaSaber().setDefense(0);
    foes().setRandom(drawing([](int low, int) { return low; }), [](float low, float high) { return low + (high - low) * 0.25f; });
    leader->applyEffect(std::make_shared<ACIncreaseEffect>(100, ACBonus::Deflection, kAllDamageTypeFlags), DurationType::Permanent);
    addFoe("trooper", glm::vec2(0.0f, -6.0f), kBlasterPistol, /*assuredHit=*/false);
    addFoe("blade", glm::vec2(0.0f, -1.2f), kVibroblade, /*assuredHit=*/false);

    // when
    play(6.0f);

    // then, over the one KOTOR round the test has, with no end-of-round
    // script to begin another: bolts fired that weren't aimed, and only
    // aimed ones hit; blows came that missed by the roll; every swing but
    // one cut short as the round ended came to a blow that missed or landed
    const auto &counts = foes().counts();
    EXPECT_GE(counts.bolts, 2);
    EXPECT_LT(counts.aimed, counts.bolts);
    EXPECT_LE(counts.hitCharacter, counts.aimed);
    EXPECT_GE(counts.swings, 2);
    EXPECT_GE(counts.blowsMissed, 1);
    EXPECT_EQ(0, counts.blowsParried);
    EXPECT_LE(counts.blowsMissed + counts.blowsLanded, counts.swings);
    EXPECT_GE(counts.blowsMissed + counts.blowsLanded, counts.swings - 1);
}

TEST_F(JaSaberFoes, should_not_defend_at_rank_0) {
    game.jaSaber().setDefense(0);
    foes().setRandom(drawing([](int, int high) { return high; }), lowest());
    addFoe("trooper", glm::vec2(0.0f, 6.0f), kBlasterPistol);

    play(1.5f);

    EXPECT_EQ(0, game.jaSaber().parryCount());
    EXPECT_EQ(0, foes().counts().sentBack);
    EXPECT_GE(foes().counts().hitCharacter, 1);
}

TEST_F(JaSaberFoes, should_parry_an_armed_blow_from_the_front_and_take_one_from_behind) {
    // given: a vibroblade 1.2 m in front of the leader, in its reach
    auto front = addFoe("front", glm::vec2(0.0f, 1.2f), kVibroblade);

    // when: it swings
    play(1.0f);

    // then: the saber parried, and the leader is unhurt
    EXPECT_GE(foes().counts().swings, 1);
    EXPECT_GE(foes().counts().blowsParried, 1);
    EXPECT_GE(game.jaSaber().parryCount(), 1);
    EXPECT_EQ(0, foes().counts().blowsLanded);
    EXPECT_EQ(100, leader->currentHitPoints());

    // when: one comes at its back
    front->clearAllActions();
    auto back = addFoe("back", glm::vec2(0.0f, -1.2f), kVibroblade);
    play(1.0f);

    // then: that blow lands
    EXPECT_GE(foes().counts().blowsLanded, 1);
    EXPECT_LT(leader->currentHitPoints(), 100);
}

TEST_F(JaSaberFoes, should_let_a_swing_miss_a_character_that_gets_behind_it) {
    // given: a vibroblade in front of the leader starts a swing
    addFoe("blade", glm::vec2(0.0f, 1.2f), kVibroblade);
    for (int frame = 0; frame < 120 && foes().counts().swings == 0; ++frame) {
        play(1.0f / 60.0f);
    }
    ASSERT_EQ(1, foes().counts().swings);

    // when: the leader steps behind it, still in its reach
    leader->setPosition(glm::vec3(0.0f, 2.0f, 0.0f));
    const auto &counts = foes().counts();
    for (int frame = 0; frame < 120 && counts.blowsMissed + counts.blowsLanded + counts.blowsParried == 0; ++frame) {
        play(1.0f / 60.0f);
    }

    // then: the swing went where it began, and missed
    EXPECT_EQ(1, counts.blowsMissed);
    EXPECT_EQ(0, counts.blowsLanded);
    EXPECT_EQ(0, counts.blowsParried);
    EXPECT_EQ(100, leader->currentHitPoints());
}

TEST_F(JaSaberFoes, should_take_a_blow_from_fists_or_claws_even_in_front) {
    // given: an enemy with no weapon in the leader's reach, in front
    addFoe("brawler", glm::vec2(0.0f, 1.2f), std::nullopt);

    // when
    play(1.0f);

    // then: the saber doesn't parry it, as Jedi Academy's doesn't parry a punch
    EXPECT_GE(foes().counts().swings, 1);
    EXPECT_EQ(0, foes().counts().blowsParried);
    EXPECT_GE(foes().counts().blowsLanded, 1);
    EXPECT_LT(leader->currentHitPoints(), 100);
}

TEST_F(JaSaberFoes, should_send_a_bolt_back_through_a_friend_into_the_shooter) {
    // given: a blaster 6 m in front, with a creature of the leader's side
    // halfway, on the bolt's line, and draws that send every bolt back
    game.jaSaber().setDefense(3);
    foes().setRandom(drawing([](int, int high) { return high; }), lowest());
    auto trooper = addFoe("trooper", glm::vec2(0.0f, 6.0f), kBlasterPistol);
    auto friendly = addCreature("friendly", glm::vec2(0.0f, 3.0f), 1.0f, false);

    // when
    play(2.0f);

    // then: the bolts passed the friend both ways, and hit the trooper
    EXPECT_GE(foes().counts().sentBack, 1);
    EXPECT_GE(foes().counts().hitEnemies, 1);
    EXPECT_EQ(20, friendly->currentHitPoints());
    EXPECT_LT(trooper->currentHitPoints(), 20);
}

TEST_F(JaSaberFoes, should_hand_the_attack_back_to_kotors_round_when_saber_mode_ends) {
    // given: a blaster's bolts in flight
    auto trooper = addFoe("trooper", glm::vec2(0.0f, -6.0f), kBlasterPistol);
    auto attack = trooper->getCurrentAction();
    play(0.6f);
    ASSERT_GE(foes().counts().bolts, 1);

    // when: saber mode ends in the round's first half
    game.jaSaber().leave();

    // then: the bolts are gone, and the counts stay for jaenemies
    EXPECT_EQ(0u, foes().boltsInFlight());
    EXPECT_FALSE(foes().fightsInRealTime(*leader));
    EXPECT_GE(foes().counts().bolts, 1);
    int volleys = foes().counts().volleys;
    int hitPoints = leader->currentHitPoints();

    // when: the round goes on
    play(1.5f);

    // then: the real-time attack has had this round: no more volleys, and
    // no KOTOR attack on top of it
    EXPECT_FALSE(attack->isCompleted());
    EXPECT_EQ(hitPoints, leader->currentHitPoints());
    EXPECT_EQ(volleys, foes().counts().volleys);

    // when: the round ends
    play(1.0f);

    // then: the attack ends with it, for the end-of-round script to choose the next
    EXPECT_TRUE(attack->isCompleted());
}

TEST_F(JaSaberFoes, should_not_land_a_kotor_shot_rolled_before_the_real_time_attack_took_over) {
    // given: a KOTOR attack under way, its shot rolled at once and due 1 s in
    foes().setEnabled(false);
    auto trooper = addFoe("trooper", glm::vec2(0.0f, -6.0f), kBlasterPistol);
    auto attack = trooper->getCurrentAction();
    play(0.5f);
    ASSERT_EQ(100, leader->currentHitPoints());

    // when: real-time enemies take it over, then hand it back
    foes().setEnabled(true);
    play(1.0f);
    foes().setEnabled(false);
    int hitPoints = leader->currentHitPoints();
    play(1.6f);

    // then: the dropped shot never lands, and the attack ends with the round
    EXPECT_EQ(hitPoints, leader->currentHitPoints());
    EXPECT_TRUE(attack->isCompleted());
}

TEST_F(JaSaberFoes, should_end_an_attack_handed_back_past_its_turn_with_the_round) {
    // given: a blaster in the round's second half, past its turn to attack
    auto trooper = addFoe("trooper", glm::vec2(0.0f, -6.0f), kBlasterPistol);
    auto attack = trooper->getCurrentAction();
    play(1.8f);

    // when
    game.jaSaber().leave();
    play(0.5f);

    // then: it waits for the round's end
    EXPECT_FALSE(attack->isCompleted());

    // when
    play(1.0f);

    // then: it ended with the round, rather than wait on a turn that's gone
    EXPECT_TRUE(attack->isCompleted());
}

TEST_F(JaSaberFoes, should_leave_attacks_to_kotors_rounds_when_turned_off) {
    // given
    foes().setEnabled(false);
    addFoe("trooper", glm::vec2(0.0f, -6.0f), kBlasterPistol);

    // when: a KOTOR round, whose blow lands 1 s in
    play(2.0f);

    // then: no volleys of the real-time kind, but KOTOR's own hit
    EXPECT_EQ(0, foes().counts().volleys);
    EXPECT_LT(leader->currentHitPoints(), 100);
}
