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

#include <algorithm>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../fixtures/engine.h"

#include "reone/game/game.h"
#include "reone/game/gui/ingame/options.h"
#include "reone/game/object/area.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/module.h"
#include "reone/game/party.h"
#include "reone/graphics/animation.h"
#include "reone/graphics/model.h"
#include "reone/graphics/modelnode.h"
#include "reone/graphics/options.h"
#include "reone/graphics/texture.h"
#include "reone/gui/control.h"
#include "reone/gui/gui.h"
#include "reone/resource/gff.h"
#include "reone/resource/parser/gff/gui.h"
#include "reone/scene/graph.h"
#include "reone/scene/node/model.h"

using namespace reone;
using namespace reone::game;
using namespace reone::graphics;
using namespace reone::gui;
using namespace reone::resource;
using namespace reone::scene;
using namespace testing;

namespace {

// KOTOR 1's optionsingame as the user's gui.bif lays it out (640x480): a
// column of eight buttons, the title above it, and Close at the bottom right.
// Its root panel is filled with the menu's panel art, lbl_optingame.
struct AuthoredControl {
    const char *tag;
    ControlType type;
    int left, top, width, height;
};

const AuthoredControl kOptionsInGame[] {
    {"BTN_LOADGAME", ControlType::Button, 60, 110, 240, 35},
    {"BTN_SAVEGAME", ControlType::Button, 60, 145, 240, 35},
    {"BTN_GAMEPLAY", ControlType::Button, 60, 180, 240, 35},
    {"BTN_QUIT", ControlType::Button, 60, 364, 240, 35},
    {"LBL_TITLE", ControlType::Label, 102, 87, 439, 21},
    {"BTN_FEEDBACK", ControlType::Button, 60, 215, 240, 35},
    {"BTN_AUTOPAUSE", ControlType::Button, 60, 250, 240, 35},
    {"BTN_GRAPHICS", ControlType::Button, 60, 285, 240, 35},
    {"BTN_SOUND", ControlType::Button, 60, 320, 240, 35},
    {"BTN_EXIT", ControlType::Button, 393, 410, 146, 28},
};

// A GUI that counts the backdrops it is given.
class BackdropCountingGUI : public GUI {
public:
    using GUI::GUI;

    void setBackground(std::shared_ptr<Texture> texture) override {
        ++backdrops;
        GUI::setBackground(std::move(texture));
    }

    int backdrops {0};
};

std::shared_ptr<BackdropCountingGUI> optionsInGame(TestEngine &engine, std::function<void(IGUI &)> preload) {
    auto gui = std::make_shared<BackdropCountingGUI>(engine.options().graphics, engine.services().scene.graphs,
                                                     engine.services().graphics, engine.services().resource);
    if (preload) {
        preload(*gui);
    }
    auto extent = Gff::Builder()
                      .field(Gff::Field::newInt("LEFT", 0))
                      .field(Gff::Field::newInt("TOP", 0))
                      .field(Gff::Field::newInt("WIDTH", 640))
                      .field(Gff::Field::newInt("HEIGHT", 480))
                      .build();
    auto border = Gff::Builder().field(Gff::Field::newResRef("FILL", "lbl_optingame")).build();
    auto root = Gff::Builder()
                    .field(Gff::Field::newInt("CONTROLTYPE", static_cast<int>(ControlType::Panel)))
                    .field(Gff::Field::newCExoString("TAG", "TGuiPanel"))
                    .field(Gff::Field::newStruct("EXTENT", extent))
                    .field(Gff::Field::newStruct("BORDER", border))
                    .build();
    gui->load(*root);
    for (auto &authored : kOptionsInGame) {
        generated::GUI_CONTROLS definition;
        definition.CONTROLTYPE = static_cast<int>(authored.type);
        definition.TAG = authored.tag;
        definition.EXTENT.LEFT = authored.left;
        definition.EXTENT.TOP = authored.top;
        definition.EXTENT.WIDTH = authored.width;
        definition.EXTENT.HEIGHT = authored.height;
        auto control = std::shared_ptr<Control>(gui->newControl(authored.type, authored.tag));
        control->load(definition);
        gui->addControlToBack(control, IGUI::ControlCoordinates::Authored);
    }
    return gui;
}

class KotorOptionsMenu : public Test {
protected:
    void SetUp() override {
        engine.init();
        engine.options().graphics.width = 1280;
        engine.options().graphics.height = 960;
        EXPECT_CALL(engine.resourceModule().textures(), get(_, _)).WillRepeatedly(Return(art));
        EXPECT_CALL(engine.resourceModule().textures(), get("blackfill", _)).WillRepeatedly(Return(black));
    }

    // The menu's GUI, when the game asks for it.
    void layOut() {
        EXPECT_CALL(engine.guiModule().guis(), get("optionsingame", _))
            .WillOnce(Invoke([this](const std::string &, std::function<void(IGUI &)> preload) {
                gui = optionsInGame(engine, std::move(preload));
                return gui;
            }));
    }

    int childIndex(const std::string &tag) {
        auto &children = gui->rootControl().children();
        auto found = std::find_if(children.begin(), children.end(), [&tag](auto &child) {
            return child.get().tag() == tag;
        });
        return found == children.end() ? -1 : static_cast<int>(found - children.begin());
    }

    // An engine of this test's own: its mocks' expectations stay here.
    TestEngine engine;
    StubConsole console;
    std::shared_ptr<Texture> art {std::make_shared<Texture>("lbl_optingame", TextureType::TwoDim, Texture::Properties {})};
    std::shared_ptr<Texture> black {std::make_shared<Texture>("blackfill", TextureType::TwoDim, Texture::Properties {})};
    std::shared_ptr<BackdropCountingGUI> gui;
};

} // namespace

TEST_F(KotorOptionsMenu, should_show_the_game_through_and_back_the_options_with_solid_panels) {
    // given
    layOut();
    Game game(GameID::KotOR, "", engine.options(), engine.services(), console);
    OptionsMenu menu(game, engine.services());

    // when
    menu.init();

    // then: no backdrop, and no panel art over the game
    ASSERT_TRUE(gui);
    EXPECT_EQ(0, gui->backdrops);
    EXPECT_FALSE(gui->rootControl().border().fill);

    // and: solid panels around the title, the column of buttons and Close,
    // 4 authored pixels out, drawn before every control of the menu's own
    struct Expected {
        const char *tag;
        int left, top, width, height;
    };
    int firstOwn = childIndex("BTN_LOADGAME");
    ASSERT_LE(0, firstOwn);
    for (auto &authored : kOptionsInGame) {
        firstOwn = std::min(firstOwn, childIndex(authored.tag));
    }
    for (auto expected : {Expected {"LBL_SEETHROUGH_TITLE", 98, 83, 447, 29},
                          Expected {"LBL_SEETHROUGH_BUTTONS", 56, 106, 248, 297},
                          Expected {"LBL_SEETHROUGH_EXIT", 389, 406, 154, 36}}) {
        auto panel = gui->findControl(expected.tag);
        ASSERT_TRUE(panel) << expected.tag;
        EXPECT_EQ(expected.left, panel->authoredExtent().left) << expected.tag;
        EXPECT_EQ(expected.top, panel->authoredExtent().top) << expected.tag;
        EXPECT_EQ(expected.width, panel->authoredExtent().width) << expected.tag;
        EXPECT_EQ(expected.height, panel->authoredExtent().height) << expected.tag;
        // laid out with the rest, at twice the size on 1280x960
        EXPECT_EQ(2 * expected.width, panel->extent().width) << expected.tag;
        EXPECT_EQ(black, panel->border().fill) << expected.tag;
        EXPECT_FALSE(panel->isSelectable()) << expected.tag;
        int index = childIndex(expected.tag);
        EXPECT_LE(0, index) << expected.tag;
        EXPECT_LT(index, firstOwn) << expected.tag;
    }
}

TEST_F(KotorOptionsMenu, should_leave_out_a_panel_for_controls_the_menu_lacks) {
    // given: a GUI of only a root panel
    auto empty = std::make_shared<BackdropCountingGUI>(engine.options().graphics, engine.services().scene.graphs,
                                                       engine.services().graphics, engine.services().resource);
    auto root = Gff::Builder()
                    .field(Gff::Field::newInt("CONTROLTYPE", static_cast<int>(ControlType::Panel)))
                    .field(Gff::Field::newCExoString("TAG", "TGuiPanel"))
                    .field(Gff::Field::newStruct("EXTENT", Gff::Builder().build()))
                    .field(Gff::Field::newStruct("BORDER", Gff::Builder().build()))
                    .build();
    empty->load(*root);

    // when
    seeThroughOptions(*empty);

    // then
    EXPECT_FALSE(empty->findControl("LBL_SEETHROUGH_TITLE"));
    EXPECT_FALSE(empty->findControl("LBL_SEETHROUGH_BUTTONS"));
    EXPECT_FALSE(empty->findControl("LBL_SEETHROUGH_EXIT"));
    EXPECT_TRUE(empty->rootControl().children().empty());
}

TEST(PlayerMenus, should_count_no_menu_before_a_game_is_loaded) {
    // given: a game with no module loaded
    TestEngine &engine = testEngine();
    StubConsole console;
    Game game(GameID::KotOR, "", engine.options(), engine.services(), console);

    // when: the save list is up, as from the main menu
    TestGameModule::setCurrentScreen(game, static_cast<int>(Game::Screen::SaveLoad));

    // then: it isn't a menu over the game
    EXPECT_FALSE(game.isInPlayerMenu());
}

// Escape's menu over a game in play

namespace {

input::Event escapeKey() {
    return input::Event::newKeyDown(input::KeyEvent(true, input::KeyCode::Escape, 0, false));
}

// A game in play, on an engine of the test's own, whose scene graphs give
// out the one real scene the test made, with something in it moving.
class EscapeMenu : public Test {
protected:
    void SetUp() override {
        engine.init();
        scene = std::make_unique<SceneGraph>("escape", pipelineFactory, graphicsOptions,
                                             engine.services().graphics, engine.services().audio, engine.services().resource);
        ON_CALL(engine.sceneModule().graphs(), get(_)).WillByDefault(ReturnRef(*scene));

        game = std::make_unique<Game>(GameID::KotOR, "", engine.options(), engine.services(), console);
        game->initLocalServices();
        auto area = game->newArea();
        area->initCameras(glm::vec3(0.0f), 0.0f);
        TestGameModule::setActiveModuleArea(*game, area);
        TestGameModule::loadModulePlayer(*game->module());
        auto pc = game->newCreature();
        pc->setMaxHitPoints(10);
        pc->setCurrentHitPoints(10);
        game->party().addMember(kNpcPlayer, pc);
        game->party().setPlayer(pc);
        area->add(pc);
        game->openInGame();

        const glm::quat none(1.0f, 0.0f, 0.0f, 0.0f);
        root = std::make_shared<ModelNode>(0, "sway", glm::vec3(0.0f), none, true);
        model = std::make_shared<Model>("sway", 0, root, std::vector<std::shared_ptr<Animation>>(), "", 1.0f);
        node = std::make_shared<ModelSceneNode>(*model, ModelUsage::Placeable, *scene,
                                                engine.services().graphics, engine.services().audio, engine.services().resource);
        node->init();
        animation = std::make_shared<Animation>("sway", 100.0f, 0.0f, "", root, std::vector<Animation::Event>());
        node->playAnimation(*animation);
        scene->addRoot(node);
    }

    void TearDown() override {
        game.reset();
    }

    float swayed() const {
        return node->animationChannels().front().time;
    }

    void play(int frames) {
        for (int i = 0; i < frames; ++i) {
            game->update(1.0f / 60.0f);
        }
    }

    TestEngine engine;
    StubConsole console;
    NiceMock<MockRenderPipelineFactory> pipelineFactory;
    GraphicsOptions graphicsOptions;
    std::unique_ptr<SceneGraph> scene;
    std::unique_ptr<Game> game;
    std::shared_ptr<ModelNode> root;
    std::shared_ptr<Model> model;
    std::shared_ptr<ModelSceneNode> node;
    std::shared_ptr<Animation> animation;
};

} // namespace

TEST_F(EscapeMenu, should_hold_the_world_still_behind_the_options_menu) {
    // given: in play, the world moves
    play(30);
    float before = swayed();
    ASSERT_GT(before, 0.0f);

    // when: Escape
    EXPECT_TRUE(game->handle(escapeKey()));
    play(30);

    // then: the options menu is open, and the world stands still behind it
    EXPECT_EQ(Game::Screen::InGameMenu, game->currentScreen());
    EXPECT_FLOAT_EQ(before, swayed());

    // when: Escape again
    EXPECT_TRUE(game->handle(escapeKey()));
    play(30);

    // then: the world moves on
    EXPECT_EQ(Game::Screen::InGame, game->currentScreen());
    EXPECT_GT(swayed(), before);
}

TEST_F(EscapeMenu, should_keep_the_world_moving_behind_a_containers_window) {
    // given
    play(30);
    float before = swayed();

    // when: a container's window is open, which shows the world around it
    TestGameModule::setCurrentScreen(*game, static_cast<int>(Game::Screen::Container));
    play(30);

    // then: its animations go on, as before
    EXPECT_GT(swayed(), before);
}

TEST_F(EscapeMenu, should_open_no_menu_on_escape_while_a_movie_plays) {
    // given: a movie over the game
    auto movie = std::make_shared<NiceMock<movie::MockMovie>>();
    EXPECT_CALL(engine.resourceModule().movies(), get("escape_movie")).WillOnce(Return(movie));
    game->playVideo("escape_movie");
    ASSERT_TRUE(game->movie());

    // when
    EXPECT_FALSE(game->handle(escapeKey()));

    // then
    EXPECT_EQ(Game::Screen::InGame, game->currentScreen());
}
