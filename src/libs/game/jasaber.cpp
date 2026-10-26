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

#include "reone/game/action.h"
#include "reone/game/attack.h"
#include "reone/game/console.h"
#include "reone/game/jafoes.h"
#include "reone/game/game.h"
#include "reone/game/object/area.h"
#include "reone/game/object/camera/thirdperson.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/item.h"
#include "reone/game/object/module.h"
#include "reone/game/party.h"
#include "reone/game/player.h"
#include "reone/graphics/animation.h"
#include "reone/graphics/format/glareader.h"
#include "reone/graphics/model.h"
#include "reone/graphics/types.h"
#include "reone/scene/animproperties.h"
#include "reone/scene/node/model.h"
#include "reone/scene/node/modelnode.h"
#include "reone/scene/types.h"
#include "reone/system/logutil.h"
#include "reone/system/randomutil.h"
#include "reone/system/stream/fileinput.h"

using namespace reone::graphics;
using namespace reone::scene;

namespace reone {

namespace game {

static constexpr const char *kWeaponNode = "rhand";

// JA's player stands 64 units tall: DEFAULT_MINS_2 -24 to DEFAULT_MAXS_2 40.
static constexpr float kJaPlayerHeight = 64.0f;

// pmbbl's camera hook stands this high, in metres.
static constexpr float kFallbackEyeHeight = 1.68f;

// The character's eyes above its feet: the camera hook reone's own camera
// aims at, or else JA's proportion of the model's height.
static float eyeHeight(const ModelSceneNode &model) {
    if (auto hook = model.model().getNodeByName("camerahook")) {
        float height = hook->absoluteTransform()[3].z;
        if (height > 0.1f) {
            return height;
        }
    }
    float top = model.model().aabb().max().z;
    if (top > 0.1f) {
        return top * JaCamera::kJaEyeHeight / kJaPlayerHeight;
    }
    return kFallbackEyeHeight;
}

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
    if (dropKotorAttacks(*leader)) {
        info("jasaber: dropped KOTOR's own attack, its swing begun, as saber mode started");
    }
    camera->setFacing(leader->getFacing());
    driver->start(jaCurrentPose(*body.retargeter, *model));

    // JA's camera glides in from KOTOR's view, tilted as KOTOR's looks.
    const ThirdPersonCamera::View &shown = camera->shownView();
    JaCamera view(eyeHeight(*model));
    view.start(shown.eye, shown.aim, camera->facing());
    glm::vec3 look(shown.aim - shown.eye);
    float pitch = 0.0f;
    if (glm::length(look) > 1e-4f) {
        pitch = -std::asin(glm::clamp(glm::normalize(look).z, -1.0f, 1.0f));
    }
    leader->holdAnimation(true);
    leader->setLightsabersOn(true);

    _creature = leader;
    _module = module;
    _model = model.get();
    _body = &body;
    _driver = std::move(driver);
    _keys = Keys();
    _modeTime = 0.0f;
    _pitch = glm::clamp(pitch, -JaCamera::kMaxViewPitch, JaCamera::kMaxViewPitch);
    _view = view;
    // The blades start over, as thick as JA's traces at this body's scale.
    _contacts.reset();
    _contacts.setBladeRadius(kJaBladeTraceUnits * body.retargeter->scale());
    _moveSerial = 0;
    _hitCount = 0;
    _litBlades = -1;
    _boltParryReady = 0.0f;
    _parryCount = 0;
    _foes.reset();
    _foes.resetCounts();
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
    // Enemies go back to KOTOR's rounds; their bolts in flight are gone.
    _foes.reset();
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
    // KOTOR's camera takes over again, facing the same way.
    if (auto *camera = thirdPersonCamera()) {
        camera->setView(std::nullopt);
    }
    _view.reset();
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
        // Like JA's own mouse look, not inverted: toward you looks down.
        turn(-_mouseTurn * event.motion.xrel);
        look(_mouseTurn * event.motion.yrel);
        return true;
    case input::EventType::MouseWheel: {
        // Rolling the wheel away brings the camera nearer, as in most games.
        float notches = event.wheel.direction == input::MouseWheelDirection::Flipped ? -event.wheel.y : event.wheel.y;
        if (notches != 0.0f) {
            setZoom(_zoom * std::pow(kWheelZoom, -notches));
        }
        return true;
    }
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
    case input::KeyCode::V:
        if (down && !event.repeat) {
            setShoulderView(!_shoulder);
        }
        return true;
    case input::KeyCode::Space:
        // Jedi Academy's jump, not yet in. KOTOR's pause on Space showed no
        // sign of itself and looked like the game freezing, so saber mode
        // keeps the key.
        if (down && !event.repeat) {
            print("jasaber: Space would jump, which saber mode doesn't do yet");
        }
        return true;
    default:
        return false;
    }
}

JaSaberInput JaSaber::input() const {
    JaSaberInput result;
    if (_scripted) {
        result = *_scripted;
    } else {
        result.attack = _keys.attack;
        result.forward = (_keys.forward ? 1 : 0) - (_keys.back ? 1 : 0);
        result.right = ((_keys.rightD || _keys.rightC) ? 1 : 0) - ((_keys.leftA || _keys.leftZ) ? 1 : 0);
    }
    // The view's tilt picks an attack too, as JA's view pitch does.
    result.pitch = glm::degrees(_pitch);
    return result;
}

bool JaSaber::walking() const {
    return !_scripted && (_keys.walk || _keys.shiftWalk);
}

void JaSaber::look(float radians) {
    _pitch = glm::clamp(_pitch + radians, -JaCamera::kMaxViewPitch, JaCamera::kMaxViewPitch);
}

void JaSaber::setShoulderView(bool shoulder) {
    if (_shoulder == shoulder) {
        return;
    }
    _shoulder = shoulder;
    print(shoulder ? "jasaber: camera over the right shoulder" : "jasaber: camera behind");
}

float JaSaber::kotorCameraDistance() const {
    auto module = _module.lock();
    if (module && module->area() && module->area()->camStyleDefault().distance > 0.0f) {
        return module->area()->camStyleDefault().distance;
    }
    return _view ? JaCamera::Settings::behind().range * _view->scale() : 0.0f;
}

float JaSaber::cameraRange() const {
    if (!_active || !_view) {
        return 0.0f;
    }
    return _zoom * kotorCameraDistance();
}

void JaSaber::setZoom(float zoom) {
    if (!std::isfinite(zoom)) {
        return;
    }
    _zoom = glm::clamp(zoom, kMinZoom, kMaxZoom);
    if (_active && _view) {
        float range = cameraRange();
        float shoulder = range * JaCamera::Settings::shoulder().range / JaCamera::Settings::behind().range;
        print(str(boost::format("jasaber: camera range %.2f m behind, %.2f m over the shoulder") % range % shoulder));
    }
}

ThirdPersonCamera *JaSaber::thirdPersonCamera() const {
    auto module = _module.lock();
    if (!module || !module->area()) {
        return nullptr;
    }
    return module->area()->getCamera<ThirdPersonCamera>(CameraType::ThirdPerson);
}

void JaSaber::updateCamera(float dt) {
    auto *camera = thirdPersonCamera();
    auto creature = _creature.lock();
    if (!camera || !creature || !_view) {
        return;
    }
    // Damping carries the camera from one view to the other when V switches
    // them, and to a new range when the wheel turns. Both views keep JA's
    // proportions at the range KOTOR's camera stands at, times the zoom.
    float range = cameraRange() / _view->scale();
    const JaCamera::Settings behind(JaCamera::Settings::behind());
    const JaCamera::Settings settings(_shoulder
                                          ? JaCamera::Settings::shoulder().atRange(range * JaCamera::Settings::shoulder().range / behind.range)
                                          : behind.atRange(range));
    _view->update(dt, creature->position(), camera->facing(), _pitch, settings,
                  [camera](const glm::vec3 &from, const glm::vec3 &to) { return camera->obstruction(from, to); });
    camera->setView(ThirdPersonCamera::View {_view->eye(), _view->aim()});
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

std::vector<JaBlade> JaSaber::blades() const {
    std::vector<JaBlade> result;
    if (!_active || !_model || !_body) {
        return result;
    }
    auto weapon = _model->getAttachment(kWeaponNode);
    auto hand = _model->getNodeByName(kWeaponNode);
    int handIndex = _body->retargeter->nodeIndex(kWeaponNode);
    if (!weapon || weapon->type() != SceneNodeType::Model || !hand || handIndex < 0) {
        return result;
    }
    // The hand where the pose shown now holds it, and the blades where the
    // lightsaber's model holds them in the hand.
    glm::mat4 handNow(_model->absoluteTransform() * _body->retargeter->transforms(_driver->pose())[handIndex]);
    for (auto &blade : jaSaberBlades(static_cast<ModelSceneNode &>(*weapon), *hand)) {
        result.push_back(jaTransformBlade(handNow, blade));
    }
    return result;
}

void JaSaber::updateContacts(const std::shared_ptr<Creature> &creature) {
    auto module = _game.module();
    auto area = module->area();
    std::vector<JaContactTarget> targets;
    for (auto &object : area->getObjectsByType(ObjectType::Creature)) {
        auto other = std::static_pointer_cast<Creature>(object);
        if (other == creature || other->isDead() || !module->isHostileToPartyLeader(*other) || jaSparedBySaber(*other) ||
            glm::distance2(other->position(), creature->position()) > kHitReach * kHitReach) {
            continue;
        }
        auto model = std::dynamic_pointer_cast<ModelSceneNode>(other->sceneNode());
        if (!model) {
            continue;
        }
        JaContactTarget target;
        target.id = other->id();
        target.parts = jaBodyParts(*model);
        if (!target.parts.empty()) {
            targets.push_back(std::move(target));
        }
    }
    const auto &machine = _driver->machine();
    bool hits = jaMoveHits(machine.moves().move(machine.move()));
    auto lit = blades();
    if (static_cast<int>(lit.size()) != _litBlades) {
        _litBlades = static_cast<int>(lit.size());
        info(str(boost::format("jasaber: %d %s lit at %.3f s") % _litBlades % (_litBlades == 1 ? "blade" : "blades") % _modeTime));
    }
    for (auto &hit : _contacts.update(_moveSerial, hits, lit, targets)) {
        auto target = _game.getObjectById<Creature>(hit.target);
        if (target && !target->isDead()) {
            strike(*creature, *target, hit);
        }
    }
}

void JaSaber::setEngage(bool engage) {
    _engage = engage;
    _engaged = 0;
    _scripted.reset();
}

std::vector<std::shared_ptr<Creature>> JaSaber::enemiesInSight(const Creature &creature) const {
    auto module = _game.module();
    if (!module || !module->area()) {
        return {};
    }
    auto area = module->area();
    std::vector<std::pair<float, std::shared_ptr<Creature>>> found;
    for (auto &object : area->getObjectsByType(ObjectType::Creature)) {
        auto other = std::static_pointer_cast<Creature>(object);
        // As jafight finds one: an enemy the character sees, by its own
        // perception and in a line of sight, not one elsewhere in the area.
        if (other.get() == &creature || other->isDead() || !module->isHostileToPartyLeader(*other) || jaSparedBySaber(*other) ||
            !creature.perception().sees(other->id()) || !area->isObjectSeen(creature, *other)) {
            continue;
        }
        found.emplace_back(glm::distance(glm::vec2(other->position()), glm::vec2(creature.position())), other);
    }
    // Of two as near, the one the area lists first.
    std::stable_sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<std::shared_ptr<Creature>> enemies;
    for (auto &[distance, other] : found) {
        enemies.push_back(other);
    }
    return enemies;
}

void JaSaber::engage(const Creature &creature) {
    auto enemies = enemiesInSight(creature);
    JaSaberInput held;
    if (!enemies.empty()) {
        const auto &nearest = enemies.front();
        float nearestDistance = glm::distance(glm::vec2(nearest->position()), glm::vec2(creature.position()));
        // Facing f looks along (-sin f, cos f), as update() moves the character.
        glm::vec2 toward(glm::vec2(nearest->position()) - glm::vec2(creature.position()));
        auto camera = thirdPersonCamera();
        if (camera) {
            camera->setFacing(glm::mod(std::atan2(-toward.x, toward.y), glm::two_pi<float>()));
        }
        held.forward = 1;
        if (nearestDistance <= kEngageRange) {
            held.attack = true;
            held.right = 1;
        }
        if (nearest->id() != _engaged) {
            _engaged = nearest->id();
            info(str(boost::format("jasaber: engaging %s, %.2f m away, at %.3f s") % nearest->tag() % nearestDistance % _modeTime));
        }
    } else if (_engaged) {
        _engaged = 0;
        // Whether any is left at all, out of sight, this can't tell.
        info(str(boost::format("jasaber: no enemy in sight to engage, at %.3f s") % _modeTime));
    }
    _scripted = held;
}

bool JaSaber::dropKotorAttacks(Creature &creature) {
    auto attacking = [&creature]() {
        const auto &actions = creature.actions();
        return std::any_of(actions.begin(), actions.end(), [](const std::shared_ptr<Action> &action) {
            return action && action->type() == ActionType::AttackObject;
        });
    };
    if (!attacking()) {
        return false;
    }
    // An ordinary clear leaves alone what can't stop now, as a medpac's cast
    // mid-way; a swing already begun is one, so it goes by force, which
    // discards its blow (AttackObjectAction::cancel).
    creature.clearAllActions();
    if (attacking()) {
        creature.clearAllActions(true);
    }
    return true;
}

void JaSaber::strike(Creature &attacker, Creature &target, const JaSaberHit &hit) {
    // By the user's rule: one that can't block a lightsaber dies to the hit,
    // given its hit points on top of the hit's own damage; one that can, or
    // is too big to cut down, takes the hit's damage, with no attack roll
    // either way.
    const char *standing = "can't block";
    if (jaCanBlockSaber(target)) {
        standing = "can block";
    } else if (jaTooBigToCut(target)) {
        standing = "too big to cut";
    }
    auto weapon = attacker.getEquippedItem(InventorySlots::rightWeapon);
    AttackBuffer attack;
    attack.addContactHit(attacker, target, weapon.get(), jaSaberHitExtraDamage(target));
    attack.resolve(attacker, target);
    attack.signal(_game, _services, attacker, target);
    ++_hitCount;
    // KOTOR's lightsaber hit, or its clash, with sparks, on a weapon that
    // stands up to it; and one that lives through the hit flinches.
    if (jaCanBlockSaber(target)) {
        saberSound({"cb_ls_clash1", "cb_ls_clash2"}, hit.point);
        _foes.playFlash("v_parrspr_imp", hit.point);
    } else {
        saberSound({"cb_ls_hitsoft1", "cb_ls_hitsoft2"}, hit.point);
    }
    if (!target.isDead()) {
        target.playAnimation(CombatAnimation::Damage, target.getWieldType(), 1);
        target.playSound(randomInt(0, 1) == 0 ? resource::SoundSetEntry::PainGrunt1 : resource::SoundSetEntry::PainGrunt2);
    }
    info(str(boost::format("jasaber: hit %s (%s, %s) in %s at %.3f s: %d hit points left%s") %
             target.tag() % jaBodyRegionName(hit.region) % standing %
             moveName() % _modeTime % target.currentHitPoints() % (target.isDead() ? ", dead" : "")));
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
    if (dropKotorAttacks(*creature)) {
        info(str(boost::format("jasaber: dropped KOTOR's own attack at %.3f s; in saber mode the blade attacks") % _modeTime));
    }
    if (_engage) {
        engage(*creature);
    }
    JaSaberInput in(input());
    bool walk = walking();
    float speed = walk ? creature->walkSpeed() : creature->runSpeed();
    auto step = _driver->step(dt, in, walk, speed);
    _moveSerial += static_cast<int>(step.started.size());
    const auto &moves = _driver->machine().moves();
    for (int id : step.started) {
        const auto &move = moves.move(id);
        info(str(boost::format("jasaber: %s (%s) at %.3f s") % move.name % move.clip % _modeTime));
        // KOTOR's lightsaber swings (weaponsounds.2da): a spin twirls.
        if (move.spin) {
            saberSound({"cb_ls_twirl1", "cb_ls_twirl2"}, creature->position());
        } else if (move.kind == JaMoveKind::Attack) {
            saberSound({"cb_ls_swingshrt1", "cb_ls_swingshrt2", "cb_ls_swingshrt3"}, creature->position());
        }
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
        // The step's share of the speed: less in a spin, backwards in an
        // attack, and running backwards.
        area->moveCreature(creature, direction, !walk, dt, speed * step.speedScale * dt);
    }
    creature->setFacing(view + _driver->legsYaw());
    showPose();
    // Where the character now stands, with the pose it now shows.
    updateContacts(creature);
    // The bolts in flight, against the guard and the body as they now stand.
    _foes.update(dt);
    // After the move, so the camera sees where the character is now.
    updateCamera(dt);
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

void JaSaber::saberSound(const std::vector<std::string> &names, const glm::vec3 &point) {
    if (!names.empty()) {
        _foes.playSound(names[static_cast<size_t>(randomInt(0, static_cast<int>(names.size()) - 1))], point);
    }
}

std::shared_ptr<Creature> JaSaber::character() const {
    return _active ? _creature.lock() : nullptr;
}

float JaSaber::viewFacing() const {
    if (auto *camera = thirdPersonCamera()) {
        return camera->facing();
    }
    auto creature = _creature.lock();
    return creature ? creature->getFacing() : 0.0f;
}

const JaSaberMove *JaSaber::currentMove() const {
    if (!_active || !_driver) {
        return nullptr;
    }
    const auto &machine = _driver->machine();
    return &machine.moves().move(machine.move());
}

float JaSaber::metresPerUnit() const {
    if (_body && _body->retargeter) {
        return _body->retargeter->scale();
    }
    return kJaBladeRadius / kJaBladeTraceUnits;
}

glm::vec3 JaSaber::eye() const {
    auto creature = _creature.lock();
    if (!creature || !_model) {
        return glm::vec3(0.0f);
    }
    return creature->position() + glm::vec3(0.0f, 0.0f, eyeHeight(*_model));
}

std::vector<JaBodyPart> JaSaber::bodyParts() const {
    if (!_active || !_model) {
        return {};
    }
    return jaBodyParts(*_model);
}

JaGuard JaSaber::guard() const {
    const JaSaberMove *move = currentMove();
    if (!move || _litBlades <= 0 || defense() < 1) {
        return JaGuard::None;
    }
    return jaGuardOf(*move);
}

int jaDefenseFromFeats(const Creature &creature) {
    const auto &attributes = creature.attributes();
    if (attributes.hasFeat(FeatType::MasterJediDefense)) {
        return 3;
    }
    if (attributes.hasFeat(FeatType::AdvancedJediDefense)) {
        return 2;
    }
    return attributes.hasFeat(FeatType::JediDefense) ? 1 : 0;
}

int JaSaber::defense() const {
    if (_defenseSet) {
        return *_defenseSet;
    }
    auto creature = character();
    if (!creature) {
        creature = _game.party().getLeader();
    }
    return creature ? jaDefenseFromFeats(*creature) : 0;
}

void JaSaber::setDefense(int defense) {
    _defenseSet = glm::clamp(defense, 0, kJaMaxDefense);
    print(str(boost::format("jasaber: saber defense rank %d%s") % *_defenseSet % (*_defenseSet == 0 ? ", no defense" : "")));
}

void JaSaber::setDefenseFromFeats() {
    _defenseSet.reset();
    int rank = defense();
    print(str(boost::format("jasaber: saber defense rank %d, from the character's feats%s") % rank % (rank == 0 ? ", no defense" : "")));
}

bool JaSaber::parryBolt(const glm::vec3 &boltPosition) {
    int rank = defense();
    if (!_active || !_driver || rank < 1 || _litBlades <= 0 || input().attack || _modeTime < _boltParryReady ||
        !_driver->machine().canBlock()) {
        return false;
    }
    JaBlock block = jaBlockAt(boltPosition, eye(), viewFacing(), metresPerUnit());
    int move = _driver->machine().moves().reflect(block);
    if (!_driver->block(move, jaParryWeaponTimeMs(rank))) {
        return false;
    }
    _boltParryReady = _modeTime + static_cast<float>(jaBoltParryDebounceMs(rank)) / 1000.0f;
    ++_parryCount;
    info(str(boost::format("jasaber: the saber sees a bolt coming, %s, at %.3f s") % jaBlockName(block) % _modeTime));
    return true;
}

bool JaSaber::parryBlow(const Creature &attacker, const glm::vec3 &point) {
    auto creature = character();
    if (!creature || guard() != JaGuard::Wide ||
        !jaInFrontLevel(attacker.position(), creature->position(), viewFacing(), JaFoes::kGuardArc)) {
        return false;
    }
    JaBlock block = jaBlockAt(point, eye(), viewFacing(), metresPerUnit());
    if (_driver->block(_driver->machine().moves().parry(block), jaParryWeaponTimeMs(defense()))) {
        ++_parryCount;
    }
    return true;
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
