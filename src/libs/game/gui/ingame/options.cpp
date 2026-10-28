/*
 * Copyright (c) 2020-2023 The reone project contributors
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

#include "reone/game/gui/ingame/options.h"

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

#include "reone/game/game.h"
#include "reone/graphics/texture.h"
#include "reone/gui/control/button.h"

using namespace reone::audio;
using namespace reone::graphics;
using namespace reone::gui;
using namespace reone::resource;

namespace reone {

namespace game {

namespace {

// Around what each panel backs, in the authored 640x480.
constexpr int kBackingMargin = 4;

struct Backing {
    const char *tag;
    std::vector<std::string> controls;
};

} // namespace

void seeThroughOptions(IGUI &gui) {
    gui.rootControl().setBorderFill(std::shared_ptr<Texture>());

    static const std::array<Backing, 3> backings {{
        {"LBL_SEETHROUGH_TITLE", {"LBL_TITLE"}},
        {"LBL_SEETHROUGH_BUTTONS", {"BTN_LOADGAME", "BTN_SAVEGAME", "BTN_GAMEPLAY", "BTN_FEEDBACK", "BTN_AUTOPAUSE", "BTN_GRAPHICS", "BTN_SOUND", "BTN_QUIT"}},
        {"LBL_SEETHROUGH_EXIT", {"BTN_EXIT"}},
    }};
    for (auto &backing : backings) {
        // A new game builds the menu again on the GUI its last one cached,
        // which has its panels already.
        if (gui.findControl(backing.tag)) {
            continue;
        }
        std::optional<Control::Extent> around;
        for (auto &tag : backing.controls) {
            auto control = gui.findControl(tag);
            if (!control) {
                continue;
            }
            const auto &extent = control->authoredExtent();
            if (!around) {
                around = extent;
                continue;
            }
            int right = std::max(around->left + around->width, extent.left + extent.width);
            int bottom = std::max(around->top + around->height, extent.top + extent.height);
            around->left = std::min(around->left, extent.left);
            around->top = std::min(around->top, extent.top);
            around->width = right - around->left;
            around->height = bottom - around->top;
        }
        if (!around) {
            continue;
        }
        Control::Extent extent {
            around->left - kBackingMargin,
            around->top - kBackingMargin,
            around->width + 2 * kBackingMargin,
            around->height + 2 * kBackingMargin};
        auto panel = std::shared_ptr<Control>(gui.newControl(ControlType::Panel, backing.tag));
        panel->setAuthoredExtent(extent);
        panel->setExtent(extent);
        panel->setBorderFill("blackfill");
        // Drawn first, so behind the controls it backs.
        gui.addControlToFront(std::move(panel), IGUI::ControlCoordinates::Authored);
    }
}

void OptionsMenu::onGUILoaded() {
    bindControls();

    if (!_game.isTSL()) {
        seeThroughOptions(*_gui);
    } else {
        loadBackground(BackgroundType::Menu);
        fillK2SectionStrip(_controls.LBL_BAR1, _controls.LBL_BAR2);
        _controls.LB_DESC->setTintBorderFill(true);
        useK2ShellTitle(_controls.LBL_TITLE);
        for (auto &button : {
                 _controls.BTN_SAVEGAME,
                 _controls.BTN_LOADGAME,
                 _controls.BTN_GAMEPLAY,
                 _controls.BTN_FEEDBACK,
                 _controls.BTN_AUTOPAUSE,
                 _controls.BTN_GRAPHICS,
                 _controls.BTN_SOUND,
                 _controls.BTN_QUIT,
                 _controls.BTN_EXIT}) {
            enableK2ButtonBodyFill(button);
        }
    }
    _controls.BTN_LOADGAME->setOnClick([this]() {
        _game.openSaveLoad(SaveLoadMode::LoadFromInGame);
    });
    _controls.BTN_SAVEGAME->setOnClick([this]() {
        _game.openSaveLoad(SaveLoadMode::Save);
    });
    _controls.BTN_EXIT->setOnClick([this]() {
        _game.openInGame();
    });
}

} // namespace game

} // namespace reone
