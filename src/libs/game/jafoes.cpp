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

#include "reone/game/jafoes.h"

#include "reone/audio/di/services.h"
#include "reone/audio/mixer.h"
#include "reone/game/action.h"
#include "reone/game/action/attackobject.h"
#include "reone/game/action/usefeat.h"
#include "reone/game/animations.h"
#include "reone/game/combat.h"
#include "reone/game/di/services.h"
#include "reone/game/game.h"
#include "reone/game/jasaber.h"
#include "reone/game/object/area.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/item.h"
#include "reone/game/object/module.h"
#include "reone/graphics/animation.h"
#include "reone/graphics/model.h"
#include "reone/resource/di/services.h"
#include "reone/resource/provider/audioclips.h"
#include "reone/resource/provider/models.h"
#include "reone/scene/collision.h"
#include "reone/scene/di/services.h"
#include "reone/scene/graph.h"
#include "reone/scene/graphs.h"
#include "reone/scene/node/model.h"
#include "reone/scene/types.h"
#include "reone/system/cast.h"
#include "reone/system/logutil.h"

using namespace reone::graphics;
using namespace reone::resource;
using namespace reone::scene;

namespace reone {

namespace game {

namespace {

constexpr float kEpsilon = 1e-5f;

// KOTOR's sounds and impact models (weaponsounds.2da, visualeffects.2da).
constexpr const char *kDeflectSound = "cb_pa_blast1";
constexpr const char *kDeflectFlash = "v_blastdef_imp";      // VFX_COM_BLASTER_DEFLECTION
constexpr const char *kBlasterFlash = "v_blaster_imp";       // VFX_COM_BLASTER_IMPACT
constexpr const char *kClashFlash = "v_parrspr_imp";         // VFX_COM_SPARKS_PARRY_METAL
const std::array<const char *, 2> kClashSounds {"cb_ls_clash1", "cb_ls_clash2"};

// How long an impact model shows when it has no animation of its own, in seconds.
constexpr float kFlashFallback = 1.0f;

// Where a JA body's origin stands above its feet, in JA units: its box runs
// from 24 below it to 40 above (DEFAULT_MINS_2, DEFAULT_MAXS_2).
constexpr float kJaOriginHeight = 24.0f;

// Where a body without hooks is aimed at and shot from, in metres above its feet.
constexpr float kFallbackChest = 1.1f;

std::shared_ptr<ModelSceneNode> modelOf(const Creature &creature) {
    return std::dynamic_pointer_cast<ModelSceneNode>(creature.sceneNode());
}

std::optional<glm::vec3> nodePosition(ModelSceneNode &model, const std::string &name) {
    if (auto node = model.getNodeByName(name)) {
        return glm::vec3(node->absoluteTransform()[3]);
    }
    return std::nullopt;
}

// Where a bolt is aimed at a creature: its model's impact hook, as KOTOR's
// own projectiles aim (Projectile::fire), or its chest.
glm::vec3 aimPoint(const Creature &creature) {
    if (auto model = modelOf(creature)) {
        if (auto impact = nodePosition(*model, "impact")) {
            return *impact;
        }
    }
    return creature.position() + glm::vec3(0.0f, 0.0f, kFallbackChest);
}

// The head, for a bolt sent back at an enemy (CalcEntitySpot's SPOT_HEAD).
glm::vec3 headOf(const Creature &creature) {
    if (auto model = modelOf(creature)) {
        for (auto name : {"headhook", "head_g", "head"}) {
            if (auto head = nodePosition(*model, name)) {
                return *head;
            }
        }
    }
    return aimPoint(creature);
}

// Where a creature's bolt leaves its weapon: the weapon's bullet hook, as
// KOTOR's own projectiles leave it (determineProjectileOrigin in attack.cpp).
glm::vec3 muzzleOf(const Creature &creature, bool offhand) {
    auto model = modelOf(creature);
    if (!model) {
        return creature.position() + glm::vec3(0.0f, 0.0f, kFallbackChest);
    }
    if (auto weapon = static_cast<ModelSceneNode *>(model->getAttachment(offhand ? "lhand" : "rhand"))) {
        if (auto hook = weapon->getNodeByName("bullethook")) {
            return glm::vec3(hook->absoluteTransform()[3]);
        }
        return glm::vec3(weapon->absoluteTransform()[3]);
    }
    if (auto direct = nodePosition(*model, offhand ? "lbullet" : "rbullet")) {
        return *direct;
    }
    return aimPoint(creature);
}

// The weapon a creature's bolt comes from: the hand's, else the other's.
std::shared_ptr<Item> blasterOf(const Creature &creature, bool offhand) {
    auto weapon = creature.getEquippedItem(offhand ? InventorySlots::leftWeapon : InventorySlots::rightWeapon);
    if (!weapon) {
        weapon = creature.getEquippedItem(offhand ? InventorySlots::rightWeapon : InventorySlots::leftWeapon);
    }
    return weapon;
}

float animationLength(const Creature &creature, const std::string &name) {
    auto model = modelOf(creature);
    if (!model || name.empty()) {
        return 0.0f;
    }
    auto animation = model->model().getAnimation(name);
    return animation ? animation->length() : 0.0f;
}

// Whether \p attacker's current action is an attack on \p target, which
// JaFoes takes over: an attack's or a combat feat's.
bool attacking(const Creature &attacker, const Creature &target) {
    auto action = attacker.getCurrentAction();
    if (!action || action->isCompleted() || action->isCancelled()) {
        return false;
    }
    std::shared_ptr<Object> aimedAt;
    if (auto *attack = dyn_cast<AttackObjectAction>(action.get())) {
        aimedAt = attack->target();
    } else if (auto *feat = dyn_cast<UseFeatAction>(action.get())) {
        aimedAt = feat->target();
    }
    return aimedAt.get() == &target;
}

bool armed(const Creature &creature) {
    switch (creature.getWieldType()) {
    case CreatureWieldType::None:
    case CreatureWieldType::HandToHand:
    case CreatureWieldType::HandToHandComplex:
        return false;
    default:
        return true;
    }
}

} // namespace

JaFoes::JaFoes(Game &game, ServicesView &services, JaSaber &saber) :
    _game(game),
    _services(services),
    _saber(saber),
    _random(jaGameRandom()),
    _randomFloat(jaGameRandomFloat()) {
}

JaFoes::~JaFoes() {
    // Only the scene's models: the game's objects may be gone already.
    for (auto &bolt : _bolts) {
        removeBolt(bolt);
    }
    for (auto &flash : _flashes) {
        if (flash.model) {
            flash.model->graph().removeRoot(*flash.model);
        }
    }
}

void JaFoes::setEnabled(bool enabled) {
    _enabled = enabled;
    if (!enabled) {
        reset();
    }
}

void JaFoes::setRandom(JaRandom random, JaRandomFloat randomFloat) {
    _random = std::move(random);
    _randomFloat = std::move(randomFloat);
}

bool JaFoes::fightsInRealTime(const Creature &target) const {
    if (!_enabled) {
        return false;
    }
    auto character = _saber.character();
    return character && character.get() == &target;
}

bool JaFoes::busy(const Creature &attacker) const {
    auto it = _foes.find(attacker.id());
    return it != _foes.end() && it->second.phase != Foe::Phase::Idle;
}

float JaFoes::timeNow() const {
    return _saber.modeTime();
}

void JaFoes::log(const std::string &line) {
    info(line);
}

void JaFoes::reset() {
    for (auto &bolt : _bolts) {
        removeBolt(bolt);
    }
    _bolts.clear();
    for (auto &flash : _flashes) {
        if (flash.model) {
            flash.model->graph().removeRoot(*flash.model);
        }
    }
    _flashes.clear();
    for (auto &[id, foe] : _foes) {
        if (auto attacker = _game.getObjectById<Creature>(id)) {
            attacker->setMovementRestricted(false);
        }
    }
    _foes.clear();
    _counts = Counts();
}

void JaFoes::rest(Foe &foe, Creature &attacker, float low, float high) {
    foe.phase = Foe::Phase::Idle;
    foe.time = 0.0f;
    foe.shots.clear();
    foe.blow.reset();
    foe.wait = _randomFloat(low, high);
    attacker.setMovementRestricted(false);
}

JaFoes::Step JaFoes::attack(const std::shared_ptr<Action> &action, Creature &attacker, Creature &target, const JaFoeAttack &kind, float dt) {
    Foe &foe = _foes[attacker.id()];
    foe.action = action;
    // KOTOR's round goes on uncounted, for combat to start and end and for
    // the attacker's end-of-round script, its AI.
    const CombatRound &round = _game.combat().addAction(action, attacker);
    bool ranged = isRangedWieldType(attacker.getWieldType());

    switch (foe.phase) {
    case Foe::Phase::Idle: {
        if (round.state == CombatRound::Finished) {
            return Step::Finished;
        }
        foe.wait -= dt;
        if (foe.wait > 0.0f || target.isDead()) {
            return Step::Continue;
        }
        if (ranged) {
            startVolley(foe, attacker, target, kind);
        } else if (attacker.getDistanceTo(target) <= attacker.getAttackRange() + kReachSlack) {
            startSwing(foe, attacker, target, kind);
        }
        return Step::Continue;
    }
    case Foe::Phase::Volley: {
        foe.time += dt;
        for (auto &shot : foe.shots) {
            if (!shot.fired && foe.time >= shot.time) {
                shot.fired = true;
                fire(foe, shot, attacker, target);
            }
        }
        if (foe.time >= foe.length) {
            rest(foe, attacker, kMinVolleyPause, kMaxVolleyPause);
        }
        return Step::Continue;
    }
    case Foe::Phase::Swing: {
        foe.time += dt;
        if (!foe.struck && foe.time >= foe.impact) {
            foe.struck = true;
            strike(foe, attacker, target);
        }
        if (foe.time >= foe.length) {
            rest(foe, attacker, kMinSwingPause, kMaxSwingPause);
        }
        return Step::Continue;
    }
    }
    return Step::Continue;
}

void JaFoes::startVolley(Foe &foe, Creature &attacker, Creature &target, const JaFoeAttack &kind) {
    foe.phase = Foe::Phase::Volley;
    foe.time = 0.0f;
    foe.target = target.id();
    foe.animation = kind.animation ? kind.animation(attacker, target) : std::string();
    attacker.setMovementType(Creature::MovementType::None);
    attacker.setMovementRestricted(true);
    if (!foe.animation.empty()) {
        attacker.playAnimation(foe.animation, AnimationProperties::fromFlags(AnimationFlags::blend));
    }

    // KOTOR's roll: how many of its attacks would hit.
    AttackBuffer rolls;
    rolls.addPhysicalAttacks(attacker, target, kind.feat);
    rolls.resolve(attacker, target);
    int hits = static_cast<int>(rolls.successCount());

    // The shots, as weapondischarge.2da times them for the animation.
    std::vector<std::pair<float, int>> times;
    if (auto *spec = _services.game.projectiles.get(kind.projectiles, attacker.getWieldType(), attacker.appearance())) {
        times = spec->projectiles;
    }
    if (times.empty()) {
        // One shot, as far into the animation as a basic pistol's last.
        times.emplace_back(0.5f, 0);
    }
    foe.shots.clear();
    for (auto &[time, hand] : times) {
        Foe::Shot shot;
        shot.time = time;
        shot.offhand = hand != 0;
        foe.shots.push_back(shot);
    }
    // As many shots aimed as there were hits, at random among them.
    std::vector<size_t> order(foe.shots.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    for (size_t i = order.size(); i > 1; --i) {
        std::swap(order[i - 1], order[static_cast<size_t>(_random(0, static_cast<int>(i) - 1))]);
    }
    int aimed = std::min(hits, static_cast<int>(foe.shots.size()));
    for (int i = 0; i < aimed; ++i) {
        foe.shots[order[i]].aimed = true;
    }
    float lastShot = 0.0f;
    for (auto &shot : foe.shots) {
        lastShot = std::max(lastShot, shot.time);
    }
    foe.length = std::max(animationLength(attacker, foe.animation), lastShot + 0.1f);
    ++_counts.volleys;
    log(str(boost::format("jasaber: %s fires %d %s at %s, %d aimed, at %.3f s") %
            attacker.tag() % foe.shots.size() % (foe.shots.size() == 1 ? "bolt" : "bolts") %
            target.tag() % aimed % timeNow()));
}

void JaFoes::fire(Foe &foe, const Foe::Shot &shot, Creature &attacker, Creature &target) {
    Bolt bolt;
    bolt.shooter = attacker.id();
    bolt.weapon = blasterOf(attacker, shot.offhand);
    bolt.position = muzzleOf(attacker, shot.offhand);
    glm::vec3 aim(aimPoint(target));
    if (!shot.aimed) {
        // Wide of the character, square to the line of fire.
        glm::vec3 line(aim - bolt.position);
        if (glm::length(line) < kEpsilon) {
            line = glm::vec3(0.0f, 1.0f, 0.0f);
        }
        line = glm::normalize(line);
        glm::vec3 side(glm::cross(line, glm::vec3(0.0f, 0.0f, 1.0f)));
        if (glm::length(side) < kEpsilon) {
            side = glm::vec3(1.0f, 0.0f, 0.0f);
        }
        side = glm::normalize(side);
        glm::vec3 up(glm::normalize(glm::cross(side, line)));
        float angle = _randomFloat(0.0f, glm::two_pi<float>());
        aim += (side * std::cos(angle) + up * std::abs(std::sin(angle))) * kNearMiss;
    }
    glm::vec3 direction(aim - bolt.position);
    if (glm::length(direction) < kEpsilon) {
        direction = glm::vec3(jaAhead(attacker.getFacing()), 0.0f);
    }
    bolt.velocity = glm::normalize(direction) * kBoltSpeed;

    if (bolt.weapon) {
        if (auto ammunition = bolt.weapon->ammunitionType()) {
            auto &graph = _services.scene.graphs.get(kSceneMain);
            if (ammunition->model) {
                bolt.model = graph.newModel(*ammunition->model, ModelUsage::Projectile);
                bolt.model->signalEvent("detonate");
                graph.addRoot(bolt.model);
            }
            if (ammunition->muzzleFlash) {
                auto muzzle = graph.newModel(*ammunition->muzzleFlash, ModelUsage::Projectile);
                muzzle->setLocalTransform(glm::translate(bolt.position));
                muzzle->signalEvent("detonate");
                graph.addRoot(muzzle);
                _flashes.push_back(Flash {muzzle, kFlashFallback});
            }
        }
        bolt.weapon->playShotSound(0, bolt.position);
    }
    showBolt(bolt);
    _bolts.push_back(std::move(bolt));
    ++_counts.bolts;
    if (shot.aimed) {
        ++_counts.aimed;
    }
}

void JaFoes::startSwing(Foe &foe, Creature &attacker, Creature &target, const JaFoeAttack &kind) {
    foe.phase = Foe::Phase::Swing;
    foe.time = 0.0f;
    foe.struck = false;
    foe.target = target.id();
    foe.animation = kind.animation ? kind.animation(attacker, target) : std::string();
    attacker.setMovementType(Creature::MovementType::None);
    attacker.setMovementRestricted(true);
    if (!foe.animation.empty()) {
        attacker.playAnimation(foe.animation, AnimationProperties::fromFlags(AnimationFlags::blend));
    }
    // KOTOR's roll, made as the swing begins, as KOTOR makes it.
    foe.blow = std::make_shared<AttackBuffer>();
    foe.blow->addPhysicalAttacks(attacker, target, kind.feat);
    foe.blow->resolveMeleeSpecialAttack(kind.feat, attacker, target, _game);
    foe.blow->resolve(attacker, target);

    float length = animationLength(attacker, foe.animation);
    int impactMs = foe.animation.empty() ? 0 : _services.game.animations.getMeleeImpactTime(foe.animation, 0);
    foe.impact = impactMs > 0 ? static_cast<float>(impactMs) / 1000.0f : (length > 0.0f ? 0.4f * length : 0.5f);
    foe.length = std::max(length, foe.impact + 0.1f);
    ++_counts.swings;
    log(str(boost::format("jasaber: %s swings at %s (%s) at %.3f s") %
            attacker.tag() % target.tag() % (foe.animation.empty() ? "no animation" : foe.animation) % timeNow()));
}

void JaFoes::strike(Foe &foe, Creature &attacker, Creature &target) {
    if (!foe.blow || target.isDead() || _saber.character().get() != &target) {
        return;
    }
    // The blow lands only on a character still within reach, in front.
    bool inReach = attacker.getDistanceTo(target) <= attacker.getAttackRange() + kReachSlack &&
                   jaInFrontLevel(target.position(), attacker.position(), attacker.getFacing(), 0.0f);
    if (!inReach) {
        ++_counts.blowsMissed;
        log(str(boost::format("jasaber: %s's blow falls short at %.3f s") % attacker.tag() % timeNow()));
        return;
    }
    // Where the blow meets the character: its weapon hand, or its chest.
    glm::vec3 point(aimPoint(target));
    if (auto model = modelOf(attacker)) {
        if (auto hand = nodePosition(*model, "rhand")) {
            point = *hand;
        }
    }
    if (armed(attacker) && _saber.parryBlow(attacker, point)) {
        ++_counts.blowsParried;
        playFlash(kClashFlash, point);
        playSound(kClashSounds[_random(0, static_cast<int>(kClashSounds.size()) - 1)], point);
        log(str(boost::format("jasaber: parried %s's blow (%s) at %.3f s") % attacker.tag() % _saber.moveName() % timeNow()));
        return;
    }
    if (foe.blow->successCount() == 0) {
        ++_counts.blowsMissed;
        log(str(boost::format("jasaber: %s's blow misses at %.3f s") % attacker.tag() % timeNow()));
        return;
    }
    foe.blow->signal(_game, _services, attacker, target);
    ++_counts.blowsLanded;
    if (!target.isDead()) {
        target.playSound(_random(0, 1) == 0 ? SoundSetEntry::PainGrunt1 : SoundSetEntry::PainGrunt2);
    }
    log(str(boost::format("jasaber: %s's blow hits %s at %.3f s: %d hit points left%s") %
            attacker.tag() % target.tag() % timeNow() % target.currentHitPoints() % (target.isDead() ? ", dead" : "")));
}

void JaFoes::update(float dt) {
    // A volley or a swing whose action is gone ends, and lets the attacker
    // move again, unless KOTOR's AI put another attack on the character in
    // its place, as at a round's end, which takes it up.
    auto character = _saber.character();
    for (auto &[id, foe] : _foes) {
        auto action = foe.action.lock();
        if (foe.phase == Foe::Phase::Idle || (action && !action->isCompleted() && !action->isCancelled())) {
            continue;
        }
        auto attacker = _game.getObjectById<Creature>(id);
        if (!attacker) {
            foe.phase = Foe::Phase::Idle;
        } else if (!character || !attacking(*attacker, *character)) {
            rest(foe, *attacker, 0.0f, 0.0f);
        }
    }
    for (auto it = _flashes.begin(); it != _flashes.end();) {
        it->left -= dt;
        if (it->left <= 0.0f) {
            if (it->model) {
                it->model->graph().removeRoot(*it->model);
            }
            it = _flashes.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = _bolts.begin(); it != _bolts.end();) {
        if (moveBolt(*it, dt)) {
            showBolt(*it);
            ++it;
        } else {
            removeBolt(*it);
            it = _bolts.erase(it);
        }
    }
}

bool JaFoes::moveBolt(Bolt &bolt, float dt) {
    bolt.age += dt;
    if (bolt.age > kBoltLife) {
        return false;
    }
    glm::vec3 from(bolt.position);
    glm::vec3 to(from + bolt.velocity * dt);
    float travel = glm::distance(from, to);
    if (travel < kEpsilon) {
        return true;
    }

    // What stands in the way: a wall, first of all, from the area's walkmeshes.
    float wallTime = 2.0f;
    scene::Collision collision;
    auto &graph = _services.scene.graphs.get(kSceneMain);
    if (graph.testLineOfSight(from, to, collision)) {
        wallTime = glm::distance(from, collision.intersection) / travel;
    }

    auto character = _saber.character();
    if (!bolt.sentBack) {
        if (!character || character->isDead()) {
            bolt.position = to;
            return wallTime > 1.0f;
        }
        float scale = _saber.metresPerUnit();
        float facing = _saber.viewFacing();
        // The saber sees it coming and parries, before it arrives.
        if (!bolt.parryPrepared) {
            glm::vec3 toward(character->position() - from);
            toward.z = 0.0f;
            bool approaching = glm::dot(glm::vec2(bolt.velocity), glm::vec2(toward)) > 0.0f;
            float distance = glm::distance(from, aimPoint(*character));
            if (approaching && distance <= kBoltSpeed * kBoltWarning + travel &&
                jaInFront(from, _saber.eye(), facing, kJaReflectCone)) {
                bolt.parryPrepared = _saber.parryBolt(from);
            }
        }
        auto body = _saber.bodyParts();
        std::vector<JaBodyPart> guardParts;
        switch (_saber.guard()) {
        case JaGuard::Wide:
            guardParts = jaWideGuard(body, facing, scale);
            break;
        case JaGuard::Tight:
            guardParts = jaTightGuard(_saber.blades(), scale);
            break;
        default:
            break;
        }
        auto guardHit = jaSweepBall(from, to, kBoltRadius, guardParts);
        auto bodyHit = jaSweepBall(from, to, kBoltRadius, body);
        if (guardHit && guardHit->time <= wallTime && (!bodyHit || guardHit->time <= bodyHit->time)) {
            bool inFront = jaInFrontLevel(from, character->position(), facing, kJaReflectCone);
            if (jaBoltPassesGuard(_saber.defense(), inFront, _random)) {
                // Through the saber's box, to the body behind if it meets it.
                ++_counts.passedGuard;
                if (bodyHit && bodyHit->time <= wallTime) {
                    hitCharacter(bolt, bodyHit->point);
                    return false;
                }
            } else {
                // Stopped: sent back, or absorbed, by where it now is.
                if (jaInFrontLevel(guardHit->point, character->position(), facing, kJaReflectCone) &&
                    jaBoltSentBack(_saber.defense(), _random)) {
                    sendBack(bolt, guardHit->point);
                    return true;
                }
                absorb(bolt, guardHit->point);
                return false;
            }
        } else if (bodyHit && bodyHit->time <= wallTime) {
            hitCharacter(bolt, bodyHit->point);
            return false;
        }
    } else {
        // Sent back: it hits the first of the character's enemies it meets.
        auto module = _game.module();
        std::optional<JaPathContact> first;
        std::shared_ptr<Creature> struck;
        if (module && module->area()) {
            for (auto &object : module->area()->getObjectsByType(ObjectType::Creature)) {
                auto other = std::static_pointer_cast<Creature>(object);
                if (other == character || other->isDead() || !module->isHostileToPartyLeader(*other) ||
                    jaSparedBySaber(*other) || glm::distance(other->position(), from) > travel + 4.0f) {
                    continue;
                }
                auto model = modelOf(*other);
                if (!model) {
                    continue;
                }
                auto hit = jaSweepBall(from, to, kBoltRadius, jaBodyParts(*model));
                if (hit && hit->time <= wallTime && (!first || hit->time < first->time)) {
                    first = hit;
                    struck = other;
                }
            }
        }
        if (struck && character) {
            ++_counts.hitEnemies;
            AttackBuffer hit;
            hit.addContactHit(*character, *struck, bolt.weapon.get());
            hit.resolve(*character, *struck);
            hit.signal(_game, _services, *character, *struck);
            if (bolt.weapon) {
                bolt.weapon->playImpactSound(0, first->point);
            }
            playFlash(kBlasterFlash, first->point);
            if (!struck->isDead()) {
                struck->playAnimation(CombatAnimation::Damage, struck->getWieldType(), 1);
            }
            log(str(boost::format("jasaber: a bolt sent back hits %s at %.3f s: %d hit points left%s") %
                    struck->tag() % timeNow() % struck->currentHitPoints() % (struck->isDead() ? ", dead" : "")));
            return false;
        }
    }

    if (wallTime <= 1.0f) {
        playFlash(kBlasterFlash, collision.intersection);
        if (bolt.weapon) {
            bolt.weapon->playImpactSound(0, collision.intersection);
        }
        return false;
    }
    bolt.position = to;
    return true;
}

void JaFoes::hitCharacter(Bolt &bolt, const glm::vec3 &point) {
    auto character = _saber.character();
    auto shooter = _game.getObjectById<Creature>(bolt.shooter);
    if (!character || !shooter) {
        return;
    }
    ++_counts.hitCharacter;
    AttackBuffer hit;
    hit.addContactHit(*shooter, *character, bolt.weapon.get());
    hit.resolve(*shooter, *character);
    hit.signal(_game, _services, *shooter, *character);
    if (bolt.weapon) {
        bolt.weapon->playImpactSound(0, point);
    }
    playFlash(kBlasterFlash, point);
    if (!character->isDead()) {
        character->playSound(_random(0, 1) == 0 ? SoundSetEntry::PainGrunt1 : SoundSetEntry::PainGrunt2);
    }
    log(str(boost::format("jasaber: a bolt from %s hits %s at %.3f s: %d hit points left%s") %
            shooter->tag() % character->tag() % timeNow() % character->currentHitPoints() % (character->isDead() ? ", dead" : "")));
}

void JaFoes::sendBack(Bolt &bolt, const glm::vec3 &point) {
    auto character = _saber.character();
    auto shooter = _game.getObjectById<Creature>(bolt.shooter);
    JaReflectTargets targets;
    targets.shooter = shooter ? shooter->position() + glm::vec3(0.0f, 0.0f, kJaOriginHeight * _saber.metresPerUnit())
                              : point - bolt.velocity;
    if (shooter && !shooter->isDead()) {
        targets.currentEnemyHead = headOf(*shooter);
    }
    if (character) {
        // Another enemy in the view: the nearest the character sees, in its cone (Jedi_FindEnemyInCone's 0.3).
        for (auto &enemy : _saber.enemiesInSight(*character)) {
            if (enemy->id() != bolt.shooter && jaInFront(enemy->position(), character->position(), _saber.viewFacing(), 0.3f)) {
                targets.otherEnemyHead = headOf(*enemy);
                break;
            }
        }
    }
    const JaSaberMove *move = _saber.currentMove();
    JaReflectSteadiness steadiness = move ? jaReflectSteadiness(*move) : JaReflectSteadiness::Steady;
    glm::vec3 direction(jaReflectDirection(point, targets, _saber.defense(), steadiness, _saber.metresPerUnit(), _random, _randomFloat));
    float speed = glm::length(bolt.velocity);
    bolt.velocity = direction * (speed > kEpsilon ? speed : kBoltSpeed);
    bolt.position = point;
    bolt.sentBack = true;
    bolt.age = 0.0f;
    ++_counts.sentBack;
    playFlash(kDeflectFlash, point);
    playSound(kDeflectSound, point);
    log(str(boost::format("jasaber: deflected a bolt from %s back (%s) at %.3f s") %
            (shooter ? shooter->tag() : std::string("someone gone")) % _saber.moveName() % timeNow()));
}

void JaFoes::absorb(Bolt &bolt, const glm::vec3 &point) {
    auto shooter = _game.getObjectById<Creature>(bolt.shooter);
    ++_counts.absorbed;
    playFlash(kDeflectFlash, point);
    playSound(kDeflectSound, point);
    log(str(boost::format("jasaber: the saber stopped a bolt from %s (%s) at %.3f s") %
            (shooter ? shooter->tag() : std::string("someone gone")) % _saber.moveName() % timeNow()));
}

void JaFoes::showBolt(Bolt &bolt) {
    if (!bolt.model) {
        return;
    }
    // The bolt's model lies along its +X, as KOTOR's own (Projectile::update).
    glm::vec3 direction(glm::normalize(bolt.velocity));
    float yaw = std::atan2(direction.y, direction.x);
    float pitch = std::asin(glm::clamp(direction.z, -1.0f, 1.0f));
    glm::mat4 transform(glm::translate(bolt.position));
    transform *= glm::eulerAngleZ(yaw);
    transform *= glm::eulerAngleY(-pitch);
    bolt.model->setLocalTransform(transform);
}

void JaFoes::removeBolt(Bolt &bolt) {
    if (bolt.model) {
        bolt.model->graph().removeRoot(*bolt.model);
        bolt.model.reset();
    }
}

void JaFoes::playSound(const std::string &name, const glm::vec3 &point) {
    if (auto clip = _services.resource.audioClips.get(name)) {
        _services.audio.mixer.play(clip, audio::AudioType::Sound, 1.0f, false, point);
    }
}

void JaFoes::playFlash(const std::string &name, const glm::vec3 &point) {
    auto model = _services.resource.models.get(name);
    if (!model) {
        return;
    }
    auto &graph = _services.scene.graphs.get(kSceneMain);
    auto node = graph.newModel(*model, ModelUsage::Projectile);
    node->setLocalTransform(glm::translate(point));
    graph.addRoot(node);
    float left = kFlashFallback;
    if (auto impact = model->getAnimation("impact")) {
        node->playAnimation("impact");
        left = std::max(impact->length(), 0.1f);
    } else {
        node->signalEvent("detonate");
    }
    _flashes.push_back(Flash {std::move(node), left});
}

} // namespace game

} // namespace reone
