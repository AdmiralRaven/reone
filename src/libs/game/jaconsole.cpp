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
#include "reone/game/object/area.h"
#include "reone/game/object/camera/firstperson.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/item.h"
#include "reone/game/object/module.h"
#include "reone/game/party.h"
#include "reone/graphics/animation.h"
#include "reone/graphics/format/glareader.h"
#include "reone/graphics/ghoulanimation.h"
#include "reone/graphics/model.h"
#include "reone/graphics/types.h"
#include "reone/scene/animproperties.h"
#include "reone/scene/node/model.h"
#include "reone/scene/types.h"
#include "reone/system/cast.h"
#include "reone/system/logutil.h"
#include "reone/system/stream/fileinput.h"

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
    void play(const ConsoleArgs &args);
    void saber(const ConsoleArgs &args);
    void camera(const ConsoleArgs &args);
    void list(const ConsoleArgs &args);

private:
    struct Body {
        std::unique_ptr<JaRetargeter> retargeter;
        std::unique_ptr<JaSequencer> sequencer;
        std::map<std::string, std::shared_ptr<Animation>> animations;
    };

    Game &_game;
    IConsole &_console;

    std::shared_ptr<GhoulAnimation> _gla;
    std::map<std::string, GhoulAnimationClip> _clips;
    std::map<std::string, Body> _bodies;

    // Model scene nodes keep plain references to the animations they play, so
    // every animation built here lives as long as the game.
    std::vector<std::shared_ptr<Animation>> _built;

    std::shared_ptr<Creature> targetCreature();
    Body &body(const Model &model);
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

std::filesystem::path findFile(const std::filesystem::path &folder, const std::string &name) {
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

JaConsole::Body &JaConsole::body(const Model &model) {
    auto it = _bodies.find(model.name());
    if (it != _bodies.end()) {
        return it->second;
    }
    Body body;
    body.retargeter = std::make_unique<JaRetargeter>(*_gla, model);
    body.sequencer = std::make_unique<JaSequencer>(*body.retargeter, _clips);
    return _bodies.insert({model.name(), std::move(body)}).first->second;
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
            {kJaFastStance, 0.5f, 0.1f}};
}

void JaConsole::load(const ConsoleArgs &args) {
    if (args.size() < 2) {
        throw std::runtime_error("Usage: jaload <folder with _humanoid.gla and animation.cfg>");
    }
    std::filesystem::path folder(joinArgs(args, 1));
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

    for (auto &[_, body] : _bodies) {
        for (auto &[_, animation] : body.animations) {
            _built.push_back(animation);
        }
    }
    _bodies.clear();
    _gla = reader.animation();
    _clips = std::move(clips);
    print(str(boost::format("jaload: %s, %d bones, %d frames, %d clips") %
              _gla->name() % _gla->numBones() % _gla->numFrames() % _clips.size()));
}

void JaConsole::play(const ConsoleArgs &args) {
    if (!_gla) {
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
    auto model = std::dynamic_pointer_cast<ModelSceneNode>(creature->sceneNode());
    if (!model) {
        throw std::runtime_error("The creature has no model");
    }
    Body &entry = body(model->model());
    auto sequence = steps(move, *entry.sequencer);
    auto &animation = entry.animations["ja_" + move];
    if (!animation) {
        animation = entry.sequencer->animation("ja_" + move, sequence, kTransitionTime);
        _built.push_back(animation);
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
    if (!_gla) {
        throw std::runtime_error("Run jaload <folder> first");
    }
    std::string filter(boost::to_upper_copy(joinArgs(args, 1)));
    int shown = 0;
    int matched = 0;
    for (auto &[name, clip] : _clips) {
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

void registerJaConsoleCommands(Game &game, IConsole &console) {
    auto tool = std::make_shared<JaConsole>(game, console);
    console.registerCommand("jaload", "load Jedi Academy animations: jaload <folder with _humanoid.gla and animation.cfg>",
                            [tool](const ConsoleArgs &args) { tool->load(args); });
    console.registerCommand("japlay", "play a Jedi Academy move on the selected creature: japlay swing|block|overhead|guard|stance|<clip> [speed]",
                            [tool](const ConsoleArgs &args) { tool->play(args); });
    console.registerCommand("jasaber", "equip and ignite a lightsaber on the selected creature: jasaber [off]",
                            [tool](const ConsoleArgs &args) { tool->saber(args); });
    console.registerCommand("jacam", "aim the free camera at the selected creature: jacam [distance] [angle] [height]",
                            [tool](const ConsoleArgs &args) { tool->camera(args); });
    console.registerCommand("jalist", "list Jedi Academy animations: jalist [text]",
                            [tool](const ConsoleArgs &args) { tool->list(args); });
}

} // namespace game

} // namespace reone
