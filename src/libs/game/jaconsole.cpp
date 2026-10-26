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

#include "reone/game/action/attackobject.h"
#include "reone/game/action/castspellatobject.h"
#include "reone/game/console.h"
#include "reone/game/d20/spell.h"
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
    void engage(const ConsoleArgs &args);
    void creature(const ConsoleArgs &args);
    void turn(const ConsoleArgs &args);
    void mouse(const ConsoleArgs &args);
    void look(const ConsoleArgs &args);
    void view(const ConsoleArgs &args);
    void range(const ConsoleArgs &args);
    void click(const ConsoleArgs &args);
    void globals(const ConsoleArgs &args);
    void status(const ConsoleArgs &args);
    void fight(const ConsoleArgs &args);
    void use(const ConsoleArgs &args);
    void hitPoints(const ConsoleArgs &args);
    void defense(const ConsoleArgs &args);
    void enemies(const ConsoleArgs &args);

private:
    Game &_game;
    IConsole &_console;

    JaSaber &saberControl() { return _game.jaSaber(); }
    JaLibrary &library() { return _game.jaSaber().library(); }

    std::shared_ptr<Creature> partyLeader();
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

std::shared_ptr<Creature> JaConsole::partyLeader() {
    auto module = _game.module();
    if (!module || !module->area()) {
        throw std::runtime_error("Load a game first");
    }
    auto leader = _game.party().getLeader();
    if (!leader) {
        throw std::runtime_error("No party leader");
    }
    return leader;
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

void JaConsole::engage(const ConsoleArgs &args) {
    bool on = true;
    if (args.size() == 2 && boost::iequals(std::string(*args[1]), "off")) {
        on = false;
    } else if (args.size() > 2 || (args.size() == 2 && !boost::iequals(std::string(*args[1]), "on"))) {
        throw std::runtime_error("Usage: jaengage [on|off]");
    }
    saberControl().setEngage(on);
    if (on) {
        print("jaengage: going for the nearest enemy");
        return;
    }
    // What engaging leaves, for a scripted run to judge its end by.
    std::string line("jaengage: off, the keyboard and mouse again");
    auto module = _game.module();
    std::shared_ptr<Creature> leader = module && module->area() ? _game.party().getLeader() : nullptr;
    if (leader) {
        std::vector<std::string> enemies;
        for (auto &enemy : saberControl().enemiesInSight(*leader)) {
            float distance = glm::distance(glm::vec2(enemy->position()), glm::vec2(leader->position()));
            enemies.push_back(str(boost::format("%s %.2f m") % enemy->tag() % distance));
        }
        line += "; enemies in sight: " + (enemies.empty() ? std::string("none") : boost::join(enemies, ", "));
    }
    print(line);
}

void JaConsole::creature(const ConsoleArgs &args) {
    if (args.size() != 2) {
        throw std::runtime_error("Usage: jacreature <tag>");
    }
    auto leader = partyLeader();
    std::string tag(*args[1]);
    int found = 0;
    for (const std::shared_ptr<Object> &object : _game.module()->area()->getObjectsByType(ObjectType::Creature)) {
        if (!boost::iequals(object->tag(), tag)) {
            continue;
        }
        auto creature = std::static_pointer_cast<Creature>(object);
        print(str(boost::format("jacreature: %s %s, hit points %d of %d, %.2f m from the party leader") %
                  creature->tag() % (creature->isDead() ? "dead" : "alive") %
                  creature->currentHitPoints() % creature->maxHitPoints() % leader->getDistanceTo(*creature)));
        ++found;
    }
    if (found == 0) {
        print("jacreature: no creature tagged " + tag + " in the area");
    }
}

void JaConsole::defense(const ConsoleArgs &args) {
    auto &saber = saberControl();
    if (args.size() == 1) {
        print(str(boost::format("jadefense: saber defense rank %d, %s") % saber.defense() %
                  (saber.defenseFromFeats() ? "from the character's Jedi Defense feats" : "set by jadefense; jadefense auto goes back to the feats")));
        return;
    }
    if (args.size() == 2 && boost::iequals(std::string(*args[1]), "auto")) {
        saber.setDefenseFromFeats();
        return;
    }
    auto rank = args.get<int>(1);
    if (args.size() != 2 || !rank || *rank < 0 || *rank > kJaMaxDefense) {
        throw std::runtime_error("Usage: jadefense [0-3|auto]");
    }
    saber.setDefense(*rank);
}

void JaConsole::enemies(const ConsoleArgs &args) {
    auto &foes = saberControl().foes();
    if (args.size() == 2 && (boost::iequals(std::string(*args[1]), "on") || boost::iequals(std::string(*args[1]), "off"))) {
        foes.setEnabled(boost::iequals(std::string(*args[1]), "on"));
    } else if (args.size() != 1) {
        throw std::runtime_error("Usage: jaenemies [on|off]");
    }
    // What the real-time enemies did since saber mode last started, for a
    // scripted run to judge by.
    const auto &c = foes.counts();
    print(str(boost::format("jaenemies: %s; %d %s, %d %s (%d aimed): %d sent back, %d stopped, %d through the guard, "
                            "%d hit the character, %d hit enemies; %d %s: %d parried, %d hit, %d missed") %
              (foes.enabled() ? "enemies fight saber mode in real time" : "enemies fight by KOTOR's rounds") %
              c.volleys % (c.volleys == 1 ? "volley" : "volleys") % c.bolts % (c.bolts == 1 ? "bolt" : "bolts") % c.aimed %
              c.sentBack % c.absorbed % c.passedGuard % c.hitCharacter % c.hitEnemies %
              c.swings % (c.swings == 1 ? "swing" : "swings") % c.blowsParried % c.blowsLanded % c.blowsMissed));
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

void JaConsole::status(const ConsoleArgs &args) {
    if (args.size() < 2 || args.size() > 3) {
        throw std::runtime_error("Usage: jastatus <label> [item tag]");
    }
    auto leader = partyLeader();
    print(jaLeaderStatus(*leader, std::string(*args[1]), args.size() == 3 ? std::string(*args[2]) : std::string()));
}

void JaConsole::fight(const ConsoleArgs &args) {
    if (args.size() != 1) {
        throw std::runtime_error("Usage: jafight");
    }
    auto leader = partyLeader();
    auto module = _game.module();
    std::shared_ptr<Area> area = module->area();
    // A player can't click during a conversation.
    if (_game.isConversationActive()) {
        print("jafight: a conversation is on");
        return;
    }
    // A player leaves the leader to an attack on an enemy still standing.
    if (std::shared_ptr<Action> current = leader->getCurrentAction()) {
        if (auto *attack = dyn_cast<AttackObjectAction>(current.get())) {
            std::shared_ptr<Object> target = attack->target();
            auto enemy = target ? dyn_cast<Creature>(target) : nullptr;
            if (enemy && module->isHostileToPartyLeader(*enemy)) {
                print("jafight: still attacking " + enemy->tag());
                return;
            }
        }
    }
    // Otherwise, a click on the nearest enemy the leader sees.
    std::shared_ptr<Creature> nearest;
    float nearestDistance = std::numeric_limits<float>::max();
    for (const std::shared_ptr<Object> &object : area->getObjectsByType(ObjectType::Creature)) {
        auto creature = std::static_pointer_cast<Creature>(object);
        if (creature == leader ||
            creature->isTemporarilyDead() ||
            !module->isHostileToPartyLeader(*creature) ||
            !leader->perception().sees(creature->id()) ||
            !area->isObjectSeen(*leader, *creature)) {
            continue;
        }
        float distance = leader->getDistanceTo(*creature);
        if (distance < nearestDistance) {
            nearest = std::move(creature);
            nearestDistance = distance;
        }
    }
    if (!nearest) {
        print("jafight: no living enemy in sight");
        return;
    }
    area->selectObject(nearest, /*force=*/true);
    module->onObjectClick(nearest);
    print(str(boost::format("jafight: attacking %s, %.2f m away") % nearest->tag() % nearestDistance));
}

void JaConsole::use(const ConsoleArgs &args) {
    if (args.size() != 2) {
        throw std::runtime_error("Usage: jause <item tag>");
    }
    auto leader = partyLeader();
    std::string tag(*args[1]);
    std::shared_ptr<Item> item = jaUseHealingItem(_game, leader, tag);
    if (!item) {
        throw std::runtime_error("No healing item tagged " + tag + " on " + leader->tag());
    }
    const Spell &spell = *leader->itemAttributes().healingSpells().at(item);
    print(str(boost::format("jause: %s, %d in its stack, spell %d, on %s at %d of %d hit points") %
              item->tag() % item->stackSize() % static_cast<int>(spell.type) %
              leader->tag() % leader->currentHitPoints() % leader->maxHitPoints()));
}

void JaConsole::hitPoints(const ConsoleArgs &args) {
    if (args.size() != 2) {
        throw std::runtime_error("Usage: jahitpoints <points>");
    }
    auto leader = partyLeader();
    auto points = args.get<int>(1);
    if (!points || *points < 1 || *points > leader->maxHitPoints()) {
        throw std::runtime_error(str(boost::format("Hit points must be a whole number from 1 to %d") %
                                     leader->maxHitPoints()));
    }
    leader->setCurrentHitPoints(*points);
    print(str(boost::format("jahitpoints: %s hit points %d of %d") %
              leader->tag() % leader->currentHitPoints() % leader->maxHitPoints()));
}

std::string jaLeaderStatus(const Creature &leader, const std::string &label, const std::string &itemTag) {
    std::string line = str(boost::format("jastatus: %s: %s hit points %d of %d, combat %s, movement %s") %
                           label % leader.tag() % leader.currentHitPoints() % leader.maxHitPoints() %
                           (leader.isInCombat() ? "on" : "off") %
                           (leader.isMovementRestricted() ? "held" : "free"));
    if (!itemTag.empty()) {
        int count = 0;
        for (const auto &[item, spell] : leader.itemAttributes().healingSpells()) {
            if (boost::iequals(item->tag(), itemTag)) {
                count += item->stackSize();
            }
        }
        line += str(boost::format(", items %s %d") % itemTag % count);
    }
    return line;
}

std::shared_ptr<Item> jaUseHealingItem(Game &game, const std::shared_ptr<Creature> &creature, const std::string &itemTag) {
    for (const auto &[item, spell] : creature->itemAttributes().healingSpells()) {
        if (!boost::iequals(item->tag(), itemTag)) {
            continue;
        }
        // As ActionBar::handleMouseButtonDown does.
        std::shared_ptr<Action> action = game.newAction<CastSpellAtObjectAction>(
            spell, creature, std::optional<std::shared_ptr<Item>>(item));
        action->setUserAction(true);
        creature->addAction(std::move(action));
        return item;
    }
    return nullptr;
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
    console.registerCommand("jaengage", "in saber mode, go for the nearest living enemy in sight and swing at it, for scripted runs: jaengage [on|off]",
                            logged("jaengage", &JaConsole::engage));
    console.registerCommand("jacreature", "log whether each creature with a tag in the area is alive, and its hit points: jacreature <tag>",
                            logged("jacreature", &JaConsole::creature));
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
    console.registerCommand("jastatus", "log the party leader's hit points, combat, movement and healing items with a tag: jastatus <label> [item tag]",
                            logged("jastatus", &JaConsole::status));
    console.registerCommand("jafight", "attack the nearest living enemy the party leader sees, as a click on it does: jafight",
                            logged("jafight", &JaConsole::fight));
    console.registerCommand("jause", "use a healing item on the party leader, as a click on the action bar's slot does: jause <item tag>",
                            logged("jause", &JaConsole::use));
    console.registerCommand("jadefense", "show or set saber mode's defense rank, from 0, none, to 3, or leave it to the Jedi Defense feats: jadefense [0-3|auto]",
                            logged("jadefense", &JaConsole::defense));
    console.registerCommand("jaenemies", "show what real-time enemies did, or turn them on or off: jaenemies [on|off]",
                            logged("jaenemies", &JaConsole::enemies));
    console.registerCommand("jahitpoints", "set the party leader's hit points, without the scripts damage runs: jahitpoints <points>",
                            logged("jahitpoints", &JaConsole::hitPoints));
}

} // namespace game

} // namespace reone
