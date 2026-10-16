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

#include "reone/game/jasaber.h"

#include "reone/game/console.h"
#include "reone/game/game.h"
#include "reone/game/object/area.h"
#include "reone/game/object/camera/thirdperson.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/module.h"
#include "reone/game/party.h"
#include "reone/game/player.h"
#include "reone/graphics/animation.h"
#include "reone/graphics/format/glareader.h"
#include "reone/graphics/model.h"
#include "reone/graphics/types.h"
#include "reone/scene/animproperties.h"
#include "reone/scene/node/model.h"
#include "reone/scene/types.h"
#include "reone/system/logutil.h"
#include "reone/system/stream/fileinput.h"

using namespace reone::graphics;
using namespace reone::scene;

namespace reone {

namespace game {

static constexpr const char *kWeaponNode = "rhand";

static std::filesystem::path findFile(const std::filesystem::path &folder, const std::string &name) {
    auto path = folder / name;
    if (std::filesystem::is_regular_file(path)) {
        return path;
    }
    if (std::filesystem::is_directory(folder)) {
        for (auto &entry : std::filesystem::directory_iterator(folder)) {
            if (entry.is_regular_file() && boost::iequals(entry.path().filename().string(), name)) {
                return entry.path();
            }
        }
    }
    throw std::runtime_error("Not found: " + path.string());
}

// JaLibrary

void JaLibrary::load(const std::filesystem::path &folderOrGla) {
    std::filesystem::path folder(folderOrGla);
    std::filesystem::path glaPath;
    if (std::filesystem::is_regular_file(folder)) {
        glaPath = folder;
        folder = folder.parent_path();
    } else {
        glaPath = findFile(folder, "_humanoid.gla");
    }
    auto cfgPath = findFile(folder, "animation.cfg");

    FileInputStream glaStream(glaPath);
    GlaReader reader(glaStream);
    reader.load();

    std::ifstream cfgStream(cfgPath);
    std::stringstream cfgText;
    cfgText << cfgStream.rdbuf();
    auto clips = parseGhoulAnimationConfig(cfgText.str());
    if (clips.empty()) {
        throw std::runtime_error("No animations in " + cfgPath.string());
    }

    // Model scene nodes may still be playing what the old bodies built.
    for (auto &[_, body] : _bodies) {
        for (auto &[_, animation] : body.animations) {
            _keptAlive.push_back(animation);
        }
        if (body.livePose) {
            _keptAlive.push_back(body.livePose->sharedAnimation());
        }
    }
    _bodies.clear();
    _gla = reader.animation();
    _clips = std::move(clips);
}

const GhoulAnimation &JaLibrary::gla() const {
    if (!_gla) {
        throw std::runtime_error("Run jaload <folder> first");
    }
    return *_gla;
}

JaLibrary::Body &JaLibrary::body(const Model &model) {
    if (!_gla) {
        throw std::runtime_error("Run jaload <folder> first");
    }
    auto it = _bodies.find(model.name());
    if (it != _bodies.end()) {
        return it->second;
    }
    Body body;
    body.retargeter = std::make_unique<JaRetargeter>(*_gla, model);
    body.sequencer = std::make_unique<JaSequencer>(*body.retargeter, _clips);
    return _bodies.insert({model.name(), std::move(body)}).first->second;
}

void JaLibrary::keepAlive(std::shared_ptr<Animation> animation) {
    _keptAlive.push_back(std::move(animation));
}

// JaSaber

void JaSaber::print(const std::string &line) {
    _console.printLine(line);
    info(line);
}

void JaSaber::enter() {
    if (_active) {
        return;
    }
    if (!_library.isLoaded()) {
        throw std::runtime_error("Run jaload <folder> first");
    }
    auto module = _game.module();
    if (!module || !module->area()) {
        throw std::runtime_error("Load a game first");
    }
    if (_game.currentScreen() != Game::Screen::InGame) {
        throw std::runtime_error("Saber mode starts in the game itself, not in a menu or a conversation");
    }
    if (_game.cameraType() != CameraType::ThirdPerson) {
        throw std::runtime_error("Saber mode needs the third-person camera");
    }
    auto leader = _game.party().getLeader();
    if (!leader || leader->isDead()) {
        throw std::runtime_error("No living party leader");
    }
    auto model = std::dynamic_pointer_cast<ModelSceneNode>(leader->sceneNode());
    if (!model) {
        throw std::runtime_error("The party leader has no model");
    }
    auto weapon = model->getAttachment(kWeaponNode);
    if (!weapon || weapon->type() != SceneNodeType::Model ||
        static_cast<ModelSceneNode *>(weapon)->model().classification() != MdlClassification::lightsaber) {
        throw std::runtime_error("No lightsaber in " + leader->tag() + "'s right hand; jasaber equips one");
    }
    auto camera = module->area()->getCamera<ThirdPersonCamera>(CameraType::ThirdPerson);
    if (!camera) {
        throw std::runtime_error("The area has no third-person camera");
    }
    JaLibrary::Body &body = _library.body(model->model());
    auto driver = std::make_unique<JaSaberDriver>(*body.sequencer, *body.retargeter, _library.clips());
    if (!body.livePose) {
        body.livePose = std::make_unique<JaLivePose>(*body.retargeter, "ja_saber_" + model->model().name());
    }

    // Pose every frame of every clip now, rather than mid-swing.
    auto prewarm = [&](const std::string &name) {
        const auto &clip = body.sequencer->clip(name);
        float rate = clip.fps != 0.0f ? std::abs(clip.fps) : 20.0f;
        for (int frame = 0; frame < clip.numFrames; ++frame) {
            body.sequencer->sample(name, static_cast<float>(frame) / rate);
        }
    };
    for (auto &move : driver->machine().moves().moves()) {
        prewarm(move.clip);
    }
    for (auto &clip : JaSaberDriver::locomotionClips()) {
        prewarm(clip);
    }

    // Take the character over, from the pose it shows now.
    module->player().stopMovement();
    camera->stopMovement();
    leader->clearAllActions();
    camera->setFacing(leader->getFacing());
    driver->start(jaCurrentPose(*body.retargeter, *model));
    leader->holdAnimation(true);
    leader->setLightsabersOn(true);

    _creature = leader;
    _module = module;
    _model = model.get();
    _body = &body;
    _driver = std::move(driver);
    _keys = Keys();
    _modeTime = 0.0f;
    _active = true;
    showPose();
    _game.setRelativeMouseMode(true);
    _capturedMouse = true;
    print(str(boost::format("jasaber: saber mode on for %s (%s); X puts the saber away") %
              leader->tag() % model->model().name()));
}

void JaSaber::leave(const std::string &reason) {
    if (!_active) {
        return;
    }
    if (!reason.empty()) {
        print("jasaber: " + reason);
    }
    _active = false;
    if (auto creature = _creature.lock()) {
        creature->setLightsabersOn(false);
        // KOTOR's own animation blends in from the last pose shown.
        creature->holdAnimation(false);
    }
    // A screen or camera that took over has set the mouse as it needs it.
    if (_capturedMouse && _game.currentScreen() == Game::Screen::InGame && _game.cameraType() == CameraType::ThirdPerson) {
        _game.setRelativeMouseMode(false);
    }
    _capturedMouse = false;
    _driver.reset();
    _model = nullptr;
    _body = nullptr;
    _creature.reset();
    _module.reset();
    _keys = Keys();
    print("jasaber: saber mode off");
}

bool JaSaber::handle(const input::Event &event) {
    if (!_active) {
        if (event.type == input::EventType::KeyDown && event.key.code == input::KeyCode::X &&
            !event.key.repeat && _library.isLoaded()) {
            try {
                enter();
                return true;
            } catch (const std::exception &e) {
                // KOTOR's own X still plays its draw animation.
                print(std::string("jasaber: ") + e.what());
            }
        }
        return false;
    }
    switch (event.type) {
    case input::EventType::KeyDown:
    case input::EventType::KeyUp:
        return handleKey(event.key);
    case input::EventType::MouseMotion:
        turn(-_mouseTurn * event.motion.xrel);
        return true;
    case input::EventType::MouseButtonDown:
    case input::EventType::MouseButtonUp:
        if (event.button.button == input::MouseButton::Left) {
            _keys.attack = event.type == input::EventType::MouseButtonDown;
        }
        // Every button, so that the camera's own mouse look cannot let the mouse go.
        return true;
    default:
        return false;
    }
}

bool JaSaber::handleKey(const input::KeyEvent &event) {
    bool down = event.pressed;
    switch (event.code) {
    case input::KeyCode::W:
        _keys.forward = down;
        return true;
    case input::KeyCode::S:
        _keys.back = down;
        return true;
    case input::KeyCode::A:
        _keys.leftA = down;
        return true;
    case input::KeyCode::Z:
        _keys.leftZ = down;
        return true;
    case input::KeyCode::D:
        _keys.rightD = down;
        return true;
    case input::KeyCode::C:
        _keys.rightC = down;
        return true;
    case input::KeyCode::B:
        _keys.walk = down;
        return true;
    case input::KeyCode::LeftShift:
    case input::KeyCode::RightShift:
        _keys.shiftWalk = down;
        return true;
    case input::KeyCode::X:
        if (down && !event.repeat) {
            leave();
        }
        return true;
    default:
        return false;
    }
}

JaSaberInput JaSaber::input() const {
    if (_scripted) {
        return *_scripted;
    }
    JaSaberInput result;
    result.attack = _keys.attack;
    result.forward = (_keys.forward ? 1 : 0) - (_keys.back ? 1 : 0);
    result.right = ((_keys.rightD || _keys.rightC) ? 1 : 0) - ((_keys.leftA || _keys.leftZ) ? 1 : 0);
    return result;
}

bool JaSaber::walking() const {
    return !_scripted && (_keys.walk || _keys.shiftWalk);
}

void JaSaber::turn(float radians) {
    auto module = _game.module();
    if (!module || !module->area()) {
        return;
    }
    auto camera = module->area()->getCamera<ThirdPersonCamera>(CameraType::ThirdPerson);
    if (camera) {
        camera->setFacing(glm::mod(camera->facing() + radians, glm::two_pi<float>()));
    }
}

std::string JaSaber::moveName() const {
    if (!_active) {
        return std::string();
    }
    const auto &machine = _driver->machine();
    return machine.moves().move(machine.move()).name;
}

std::string JaSaber::whyNotInPlay(const std::shared_ptr<Creature> &creature) const {
    if (!creature) {
        return "the character is gone";
    }
    auto module = _game.module();
    if (!module || !module->area() || module != _module.lock()) {
        return "a new area";
    }
    if (_game.party().getLeader() != creature) {
        return "another party leader";
    }
    if (creature->isDead()) {
        return "the character died";
    }
    if (creature->sceneNode().get() != _model) {
        return "the character's model changed";
    }
    switch (_game.currentScreen()) {
    case Game::Screen::InGame:
        if (_game.cameraType() != CameraType::ThirdPerson) {
            return "the camera changed";
        }
        return "";
    case Game::Screen::InGameMenu:
    case Game::Screen::SaveLoad:
        // A menu over the game only pauses saber mode.
        return "";
    case Game::Screen::Conversation:
        return "a conversation";
    default:
        return "the game left play";
    }
}

bool JaSaber::update(float dt) {
    if (!_active) {
        return false;
    }
    auto creature = _creature.lock();
    std::string reason(whyNotInPlay(creature));
    if (!reason.empty()) {
        leave(reason);
        return false;
    }
    _modeTime += dt;
    JaSaberInput in(input());
    bool walk = walking();
    auto step = _driver->step(dt, in, walk, walk ? creature->walkSpeed() : creature->runSpeed());
    const auto &moves = _driver->machine().moves();
    for (int id : step.started) {
        info(str(boost::format("jasaber: %s (%s) at %.3f s") % moves.move(id).name % moves.move(id).clip % _modeTime));
    }

    // The character faces the camera, its legs turned toward the movement.
    auto area = _game.module()->area();
    auto camera = area->getCamera<ThirdPersonCamera>(CameraType::ThirdPerson);
    float view = camera ? camera->facing() : creature->getFacing();
    if (step.moving) {
        glm::vec2 forward(-std::sin(view), std::cos(view));
        glm::vec2 right(std::cos(view), std::sin(view));
        glm::vec2 direction(glm::normalize(static_cast<float>(in.forward) * forward + static_cast<float>(in.right) * right));
        creature->clearAllActions();
        creature->clearPath();
        area->moveCreature(creature, direction, !walk, dt, FLT_MAX);
    }
    creature->setFacing(view + _driver->legsYaw());
    showPose();
    return true;
}

void JaSaber::showPose() {
    _body->livePose->set(_driver->pose());
    auto base = _model->baseAnimationChannel();
    if (!base || base->get().anim != &_body->livePose->animation()) {
        auto properties = AnimationProperties::fromFlags(AnimationFlags::loop);
        properties.scale = 1.0f;
        _model->playAnimation(_body->livePose->animation(), nullptr, properties);
    }
}

void JaSaber::updateFrame() {
    if (!_active) {
        return;
    }
    // The module drives saber mode only with the third-person camera, so the
    // ways out are checked here, every frame.
    std::string reason(whyNotInPlay(_creature.lock()));
    if (!reason.empty()) {
        leave(reason);
        return;
    }
    bool inPlay = _game.currentScreen() == Game::Screen::InGame;
    if (inPlay == _capturedMouse) {
        return;
    }
    _game.setRelativeMouseMode(inPlay);
    _capturedMouse = inPlay;
    // Keys let go while a menu had them are not seen.
    _keys = Keys();
}

} // namespace game

} // namespace reone
