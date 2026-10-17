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

#include "reone/game/jaconsole.h"

#include "reone/game/console.h"
#include "reone/game/game.h"
#include "reone/game/jaretarget.h"
#include "reone/game/jasaber.h"
#include "reone/game/object/area.h"
#include "reone/game/object/camera/firstperson.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/item.h"
#include "reone/game/object/module.h"
#include "reone/game/party.h"
#include "reone/graphics/animation.h"
#include "reone/graphics/ghoulanimation.h"
#include "reone/graphics/model.h"
#include "reone/graphics/types.h"
#include "reone/scene/animproperties.h"
#include "reone/scene/node/model.h"
#include "reone/scene/types.h"
#include "reone/system/cast.h"
#include "reone/system/logutil.h"

using namespace reone::graphics;
using namespace reone::scene;

namespace reone {

namespace game {

static constexpr float kTransitionTime = 0.25f;
static constexpr const char *kSaberTemplate = "g_w_lghtsbr01";
static constexpr const char *kWeaponNode = "rhand";

namespace {

class JaConsole : boost::noncopyable {
public:
    JaConsole(Game &game, IConsole &console) :
        _game(game),
        _console(console) {
    }

    void load(const ConsoleArgs &args);
    void loadSave(const ConsoleArgs &args);
    void play(const ConsoleArgs &args);
    void saber(const ConsoleArgs &args);
    void camera(const ConsoleArgs &args);
    void list(const ConsoleArgs &args);
    void saberMode(const ConsoleArgs &args);
    void input(const ConsoleArgs &args);
    void turn(const ConsoleArgs &args);
    void mouse(const ConsoleArgs &args);
    void look(const ConsoleArgs &args);
    void view(const ConsoleArgs &args);
    void range(const ConsoleArgs &args);
    void click(const ConsoleArgs &args);
    void globals(const ConsoleArgs &args);

private:
    Game &_game;
    IConsole &_console;

    JaSaber &saberControl() { return _game.jaSaber(); }
    JaLibrary &library() { return _game.jaSaber().library(); }

    std::shared_ptr<Creature> targetCreature();
    std::vector<JaSequenceStep> steps(const std::string &move, const JaSequencer &sequencer) const;
    void print(const std::string &line);
};

std::string joinArgs(const ConsoleArgs &args, size_t first) {
    std::string result;
    for (size_t i = first; i < args.size(); ++i) {
        if (!result.empty()) {
            result += " ";
        }
        result += std::string(*args[i]);
    }
    return result;
}

std::string describeSteps(const std::vector<JaSequenceStep> &steps) {
    std::vector<std::string> names;
    for (auto &step : steps) {
        names.push_back(step.clip);
    }
    return boost::join(names, " > ");
}

} // namespace

void JaConsole::print(const std::string &line) {
    _console.printLine(line);
    info(line);
}

std::shared_ptr<Creature> JaConsole::targetCreature() {
    auto module = _game.module();
    if (!module || !module->area()) {
        throw std::runtime_error("Load a game first");
    }
    if (auto selected = module->area()->selectedObject()) {
        if (auto creature = dyn_cast<Creature>(selected)) {
            return creature;
        }
    }
    if (auto leader = _game.party().getLeader()) {
        return leader;
    }
    throw std::runtime_error("No party leader");
}

std::vector<JaSequenceStep> JaConsole::steps(const std::string &move, const JaSequencer &sequencer) const {
    auto named = jaNamedMove(move);
    if (!named.empty()) {
        return named;
    }
    if (!sequencer.hasClip(move)) {
        throw std::runtime_error("No move or JA clip named " + move + "; try swing, block, overhead, guard, stance or jalist");
    }
    // Any other clip, from the fast ready stance and back.
    const GhoulAnimationClip &clip = sequencer.clip(move);
    float seconds = clip.loopFrames >= 0 ? 2.0f : -1.0f;
    return {{kJaFastStance, 0.4f, 0.0f},
            {boost::to_upper_copy(move), seconds, 0.1f},
            {kJaFastStance, 0.6f, 0.35f}};
}

void JaConsole::load(const ConsoleArgs &args) {
    if (args.size() < 2) {
        throw std::runtime_error("Usage: jaload <folder with _humanoid.gla and animation.cfg>");
    }
    // Saber mode plays from the bodies a reload drops.
    saberControl().leave();
    library().load(std::filesystem::path(joinArgs(args, 1)));
    const auto &gla = library().gla();
    print(str(boost::format("jaload: %s, %d bones, %d frames, %d clips") %
              gla.name() % gla.numBones() % gla.numFrames() % library().clips().size()));
}

void JaConsole::loadSave(const ConsoleArgs &args) {
    if (args.size() > 2) {
        throw std::runtime_error("Usage: jaloadsave [number from listgames]");
    }
    // The list listgames prints and loadgame indexes: only complete save
    // slots, one per slot number, by slot.
    auto saves = _game.savedGames();
    if (saves.empty()) {
        throw std::runtime_error("No saved games to load");
    }
    size_t index = saves.size() - 1;
    if (args.size() == 2) {
        auto value = args.get<size_t>(1);
        if (!value || *value >= saves.size()) {
            throw std::runtime_error(str(boost::format("No saved game %s: listgames numbers them 0 to %d") %
                                         std::string(*args[1]) % (saves.size() - 1)));
        }
        index = *value;
    }
    std::string folder(saves[index].descriptor.directory.filename().string());
    print(str(boost::format("jaloadsave: loading [%d] %s") % index % folder));
    if (!_game.loadGame(saves[index].descriptor)) {
        throw std::runtime_error("Could not load " + folder);
    }
}

void JaConsole::play(const ConsoleArgs &args) {
    if (!library().isLoaded()) {
        throw std::runtime_error("Run jaload <folder> first");
    }
    if (args.size() < 2 || args.size() > 3) {
        throw std::runtime_error("Usage: japlay swing|block|overhead|guard|stance|<clip> [speed]");
    }
    std::string move(boost::to_lower_copy(std::string(*args[1])));
    float speed = 1.0f;
    if (args.size() == 3) {
        auto value = args.get<float>(2);
        if (!value || *value <= 0.0f) {
            throw std::runtime_error("Speed must be a positive number");
        }
        speed = *value;
    }

    auto creature = targetCreature();
    if (saberControl().isActive()) {
        throw std::runtime_error("Saber mode is on; X puts the saber away first");
    }
    auto model = std::dynamic_pointer_cast<ModelSceneNode>(creature->sceneNode());
    if (!model) {
        throw std::runtime_error("The creature has no model");
    }
    JaLibrary::Body &entry = library().body(model->model());
    auto sequence = steps(move, *entry.sequencer);
    auto &animation = entry.animations["ja_" + move];
    if (!animation) {
        animation = entry.sequencer->animation("ja_" + move, sequence, kTransitionTime);
        library().keepAlive(animation);
    }

    if (model->activeAnimationName() == animation->name() && !model->isAnimationFinished()) {
        throw std::runtime_error("japlay " + move + " is still playing");
    }
    auto properties = AnimationProperties::fromFlags(AnimationFlags::blend);
    properties.speed = speed;
    properties.scale = 1.0f;
    if (!creature->playAnimation(animation, properties)) {
        throw std::runtime_error("The creature must be standing still");
    }
    print(str(boost::format("japlay: %s on %s (%s), %.2f s: %s") %
              move % creature->tag() % model->model().name() % (animation->length() / speed) % describeSteps(sequence)));
}

void JaConsole::saber(const ConsoleArgs &args) {
    if (args.size() > 2) {
        throw std::runtime_error("Usage: jasaber [off]");
    }
    bool powered = !(args.size() == 2 && boost::iequals(std::string(*args[1]), "off"));
    auto creature = targetCreature();
    auto model = std::dynamic_pointer_cast<ModelSceneNode>(creature->sceneNode());
    if (!model) {
        throw std::runtime_error("The creature has no model");
    }
    auto weaponModel = [&]() -> ModelSceneNode * {
        auto attachment = model->getAttachment(kWeaponNode);
        if (!attachment || attachment->type() != SceneNodeType::Model) {
            return nullptr;
        }
        auto weapon = static_cast<ModelSceneNode *>(attachment);
        return weapon->model().classification() == MdlClassification::lightsaber ? weapon : nullptr;
    };

    if (powered && !weaponModel()) {
        auto item = _game.newItemFromBlueprint(kSaberTemplate);
        bool equipped = false;
        if (creature->isSlotEquipped(InventorySlots::rightWeapon)) {
            auto receiver = _game.party().sharedInventoryReceiver(creature);
            equipped = receiver && creature->replaceEquipment(InventorySlots::rightWeapon, item, *receiver);
        } else {
            equipped = creature->equip(InventorySlots::rightWeapon, item);
        }
        if (!equipped) {
            if (item->isRuntimeLive()) {
                _game.destroyRuntimeObjectGraph(item);
            }
            throw std::runtime_error(str(boost::format("Could not equip %s on %s") % kSaberTemplate % creature->tag()));
        }
        print(str(boost::format("jasaber: equipped %s on %s") % kSaberTemplate % creature->tag()));
    }

    auto weapon = weaponModel();
    if (!weapon) {
        throw std::runtime_error("No lightsaber in the right hand");
    }
    weapon->playAnimation(powered ? "powerup" : "powerdown");
    if (auto item = creature->getEquippedItem(InventorySlots::rightWeapon)) {
        if (powered) {
            item->powerUp(creature->position());
        } else {
            item->powerDown(creature->position());
        }
    }
    print(str(boost::format("jasaber: lightsaber %s") % (powered ? "on" : "off")));
}

void JaConsole::camera(const ConsoleArgs &args) {
    if (args.size() > 4) {
        throw std::runtime_error("Usage: jacam [distance] [angle] [height]");
    }
    float distance = args.get<float>(1).value_or(3.2f);
    float angle = args.get<float>(2).value_or(45.0f);
    float height = args.get<float>(3).value_or(1.4f);
    auto creature = targetCreature();
    auto camera = _game.module()->area()->getCamera<FirstPersonCamera>(CameraType::FirstPerson);
    if (!camera) {
        throw std::runtime_error("The area has no free camera");
    }
    // Model space: +Y forward, +X to the creature's right, +Z up.
    float radians = glm::radians(angle);
    glm::vec3 eye(creature->transform() * glm::vec4(distance * std::sin(radians), distance * std::cos(radians), height, 1.0f));
    glm::vec3 target(creature->transform() * glm::vec4(0.0f, 0.2f, 0.9f, 1.0f));
    camera->setPosition(eye);
    camera->setLookAt(target);
    if (_game.cameraType() != CameraType::FirstPerson) {
        print("jacam: aimed the free camera; type 'camera free' and then jacam again to look through it");
        return;
    }
    print(str(boost::format("jacam: camera at %.2f %.2f %.2f") % eye.x % eye.y % eye.z));
}

void JaConsole::list(const ConsoleArgs &args) {
    if (!library().isLoaded()) {
        throw std::runtime_error("Run jaload <folder> first");
    }
    std::string filter(boost::to_upper_copy(joinArgs(args, 1)));
    int shown = 0;
    int matched = 0;
    for (auto &[name, clip] : library().clips()) {
        if (!filter.empty() && name.find(filter) == std::string::npos) {
            continue;
        }
        ++matched;
        if (shown < 40) {
            _console.printLine(str(boost::format("%s: %d frames at %g fps%s") %
                                   name % clip.numFrames % clip.fps % (clip.loopFrames >= 0 ? ", loops" : "")));
            ++shown;
        }
    }
    _console.printLine(str(boost::format("jalist: %d clips match") % matched));
}

void JaConsole::saberMode(const ConsoleArgs &args) {
    if (args.size() > 2) {
        throw std::runtime_error("Usage: jasabermode [on|off]");
    }
    bool on = !saberControl().isActive();
    if (args.size() == 2) {
        std::string value(boost::to_lower_copy(std::string(*args[1])));
        if (value != "on" && value != "off") {
            throw std::runtime_error("Usage: jasabermode [on|off]");
        }
        on = value == "on";
    }
    if (on) {
        saberControl().enter();
    } else {
        saberControl().leave();
    }
}

void JaConsole::input(const ConsoleArgs &args) {
    if (args.size() == 2 && boost::iequals(std::string(*args[1]), "off")) {
        saberControl().setScriptedInput(std::nullopt);
        print("jainput: the keyboard and mouse again");
        return;
    }
    JaSaberInput input;
    std::vector<std::string> held;
    for (size_t i = 1; i < args.size(); ++i) {
        std::string word(boost::to_lower_copy(std::string(*args[i])));
        if (word == "attack") {
            input.attack = true;
        } else if (word == "forward") {
            input.forward = 1;
        } else if (word == "back") {
            input.forward = -1;
        } else if (word == "left") {
            input.right = -1;
        } else if (word == "right") {
            input.right = 1;
        } else {
            throw std::runtime_error("Usage: jainput [attack] [forward|back] [left|right], or jainput off");
        }
        held.push_back(word);
    }
    saberControl().setScriptedInput(input);
    print("jainput: holding " + (held.empty() ? std::string("nothing") : boost::join(held, " ")));
}

void JaConsole::turn(const ConsoleArgs &args) {
    auto degrees = args.get<float>(1);
    if (args.size() != 2 || !degrees) {
        throw std::runtime_error("Usage: jaturn <degrees to the left>");
    }
    if (!_game.module() || !_game.module()->area()) {
        throw std::runtime_error("Load a game first");
    }
    saberControl().turn(glm::radians(*degrees));
    print(str(boost::format("jaturn: %g degrees") % *degrees));
}

void JaConsole::mouse(const ConsoleArgs &args) {
    if (args.size() > 2) {
        throw std::runtime_error("Usage: jamouse [radians per unit of mouse movement]");
    }
    if (args.size() == 2) {
        auto value = args.get<float>(1);
        if (!value || *value <= 0.0f) {
            throw std::runtime_error("The turn must be a positive number");
        }
        saberControl().setMouseTurn(*value);
    }
    print(str(boost::format("jamouse: %g radians per unit (%g by default)") %
              saberControl().mouseTurn() % JaSaber::kDefaultMouseTurn));
}

void JaConsole::look(const ConsoleArgs &args) {
    auto degrees = args.get<float>(1);
    if (args.size() != 2 || !degrees) {
        throw std::runtime_error("Usage: jalook <degrees down; negative looks up>");
    }
    if (!saberControl().isActive()) {
        throw std::runtime_error("Saber mode is off; jalook tilts its camera");
    }
    saberControl().look(glm::radians(*degrees));
    print(str(boost::format("jalook: %g degrees, now %.1f down") % *degrees % glm::degrees(saberControl().pitch())));
}

void JaConsole::view(const ConsoleArgs &args) {
    if (args.size() > 2) {
        throw std::runtime_error("Usage: jaview [behind|shoulder]");
    }
    if (args.size() == 2) {
        std::string value(boost::to_lower_copy(std::string(*args[1])));
        if (value != "behind" && value != "shoulder") {
            throw std::runtime_error("Usage: jaview [behind|shoulder]");
        }
        saberControl().setShoulderView(value == "shoulder");
    }
    std::string line(saberControl().shoulderView() ? "jaview: over the right shoulder" : "jaview: behind, Jedi Academy's view");
    if (auto *camera = saberControl().camera()) {
        line += str(boost::format("; eyes %.2f m up, %.4f m per JA unit; range %.2f m") %
                    camera->eyeHeight() % camera->scale() % saberControl().cameraRange());
    }
    print(line);
}

void JaConsole::range(const ConsoleArgs &args) {
    if (args.size() > 2) {
        throw std::runtime_error("Usage: jarange [metres behind the aim point|kotor]");
    }
    if (!saberControl().isActive()) {
        throw std::runtime_error("Saber mode is off; jarange sets its camera's range");
    }
    float kotor = saberControl().kotorCameraDistance();
    if (args.size() == 2) {
        if (boost::to_lower_copy(std::string(*args[1])) == "kotor") {
            saberControl().setZoom(1.0f);
        } else {
            auto metres = args.get<float>(1);
            if (!metres || !std::isfinite(*metres) || *metres <= 0.0f) {
                throw std::runtime_error("The range must be a positive number of metres, or kotor");
            }
            saberControl().setZoom(*metres / kotor);
        }
    }
    print(str(boost::format("jarange: %.2f m behind the aim point; KOTOR's camera stands %.2f m back") %
              saberControl().cameraRange() % kotor));
}

void JaConsole::click(const ConsoleArgs &args) {
    if (args.size() != 2) {
        throw std::runtime_error("Usage: jaclick <tag>");
    }
    auto module = _game.module();
    if (!module || !module->area()) {
        throw std::runtime_error("Load a game first");
    }
    auto leader = _game.party().getLeader();
    if (!leader) {
        throw std::runtime_error("No party leader");
    }
    // Of the objects with the tag, the one nearest the leader.
    std::string tag(*args[1]);
    std::shared_ptr<Object> target;
    float nearest = std::numeric_limits<float>::max();
    for (auto &object : module->area()->objects()) {
        if (!boost::iequals(object->tag(), tag)) {
            continue;
        }
        float distance = glm::distance(object->position(), leader->position());
        if (distance < nearest) {
            nearest = distance;
            target = object;
        }
    }
    if (!target) {
        throw std::runtime_error("No object tagged " + tag);
    }
    module->area()->selectObject(target, /*force=*/true);
    module->onObjectClick(target);
    print(str(boost::format("jaclick: %s, %.2f m from %s") % target->tag() % nearest % leader->tag()));
}

void JaConsole::globals(const ConsoleArgs &args) {
    if (args.size() > 2) {
        throw std::runtime_error("Usage: jaglobals [name prefix]");
    }
    std::string prefix(args.size() == 2 ? std::string(*args[1]) : std::string());
    auto matches = [&prefix](const std::string &name) {
        return boost::istarts_with(name, prefix);
    };
    int count = 0;
    for (auto &[name, value] : _game.globalNumbers()) {
        if (matches(name)) {
            print(str(boost::format("jaglobals: %s = %d") % name % value));
            ++count;
        }
    }
    for (auto &[name, value] : _game.globalBooleans()) {
        if (matches(name)) {
            print(str(boost::format("jaglobals: %s = %s") % name % (value ? "true" : "false")));
            ++count;
        }
    }
    for (auto &[name, value] : _game.globalStrings()) {
        if (matches(name)) {
            print(str(boost::format("jaglobals: %s = \"%s\"") % name % value));
            ++count;
        }
    }
    if (count == 0) {
        print("jaglobals: none" + (prefix.empty() ? std::string() : " starting with " + prefix));
    }
}

void registerJaConsoleCommands(Game &game, IConsole &console) {
    auto tool = std::make_shared<JaConsole>(game, console);
    // The console shows a failed command's message but does not log it; log
    // it too, so scripted runs leave a record.
    auto logged = [tool](const char *name, void (JaConsole::*command)(const ConsoleArgs &)) {
        return [tool, name, command](const ConsoleArgs &args) {
            try {
                ((*tool).*command)(args);
            } catch (const std::exception &e) {
                error(std::string(name) + " failed: " + e.what());
                throw;
            }
        };
    };
    console.registerCommand("jaload", "load Jedi Academy animations: jaload <folder with _humanoid.gla and animation.cfg>",
                            logged("jaload", &JaConsole::load));
    console.registerCommand("jaloadsave", "load a saved game by its listgames number, or the last one: jaloadsave [number]",
                            logged("jaloadsave", &JaConsole::loadSave));
    console.registerCommand("japlay", "play a Jedi Academy move on the selected creature: japlay swing|block|overhead|guard|stance|<clip> [speed]",
                            logged("japlay", &JaConsole::play));
    console.registerCommand("jasaber", "equip and ignite a lightsaber on the selected creature: jasaber [off]",
                            logged("jasaber", &JaConsole::saber));
    console.registerCommand("jacam", "aim the free camera at the selected creature: jacam [distance] [angle] [height]",
                            logged("jacam", &JaConsole::camera));
    console.registerCommand("jalist", "list Jedi Academy animations: jalist [text]",
                            logged("jalist", &JaConsole::list));
    console.registerCommand("jasabermode", "start or end Jedi Academy saber mode, as X does: jasabermode [on|off]",
                            logged("jasabermode", &JaConsole::saberMode));
    console.registerCommand("jainput", "hold saber mode input for scripted runs: jainput [attack] [forward|back] [left|right], or jainput off",
                            logged("jainput", &JaConsole::input));
    console.registerCommand("jaturn", "turn the camera, and in saber mode the character, to the left: jaturn <degrees>",
                            logged("jaturn", &JaConsole::turn));
    console.registerCommand("jamouse", "show or set saber mode's mouse turn: jamouse [radians per unit]",
                            logged("jamouse", &JaConsole::mouse));
    console.registerCommand("jalook", "tilt saber mode's camera down, as moving the mouse toward you does: jalook <degrees>",
                            logged("jalook", &JaConsole::look));
    console.registerCommand("jaview", "show or switch saber mode's camera, as V does: jaview [behind|shoulder]",
                            logged("jaview", &JaConsole::view));
    console.registerCommand("jarange", "show or set how far saber mode's camera stands behind, as the mouse wheel does: jarange [metres|kotor]",
                            logged("jarange", &JaConsole::range));
    console.registerCommand("jaclick", "act on the object with a tag nearest the party leader, as a second left click does: jaclick <tag>",
                            logged("jaclick", &JaConsole::click));
    console.registerCommand("jaglobals", "log global variables: jaglobals [name prefix]",
                            logged("jaglobals", &JaConsole::globals));
}

} // namespace game

} // namespace reone
