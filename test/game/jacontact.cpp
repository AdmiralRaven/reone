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

#include "reone/game/attack.h"
#include "reone/game/effect/forceshield.h"
#include "reone/game/game.h"
#include "reone/game/jaanimator.h"
#include "reone/game/jacontact.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/item.h"
#include "reone/graphics/format/glareader.h"
#include "reone/graphics/format/mdlmdxreader.h"
#include "reone/graphics/mesh.h"
#include "reone/graphics/options.h"
#include "reone/graphics/statistic.h"
#include "reone/resource/2da.h"
#include "reone/resource/gff.h"
#include "reone/scene/graphs.h"
#include "reone/scene/node/model.h"
#include "reone/scene/node/modelnode.h"
#include "reone/system/stream/fileinput.h"

using namespace reone;
using namespace reone::audio;
using namespace reone::game;
using namespace reone::graphics;
using namespace reone::resource;
using namespace reone::scene;
using namespace reone::test;
using namespace testing;

namespace {

// An upright body 1.5 m tall and 0.2 m round, standing at the origin.
std::vector<JaBodyPart> postAtOrigin(float radius = 0.2f) {
    JaBodyPart part;
    part.region = JaBodyRegion::Body;
    part.a = glm::vec3(0.0f, 0.0f, 0.0f);
    part.b = glm::vec3(0.0f, 0.0f, 1.5f);
    part.radius = radius;
    return {part};
}

// A blade held level at 1 m, pointing along +Y from y = -1 to y = 0.5, its
// base at \p x: moved from x = -1 to x = 1, it cuts through the post.
JaBlade levelBlade(float x) {
    return JaBlade {glm::vec3(x, -1.0f, 1.0f), glm::vec3(x, 0.5f, 1.0f)};
}

JaContactTarget target(uint32_t id, std::vector<JaBodyPart> parts) {
    JaContactTarget result;
    result.id = id;
    result.parts = std::move(parts);
    return result;
}

std::map<std::string, glm::vec3> humanoidNodes() {
    return {
        {"pelvis_g", {0.0f, 0.0f, 1.0f}},
        {"torsoupr_g", {0.0f, 0.0f, 1.4f}},
        {"headhook", {0.0f, 0.0f, 1.6f}},
        {"lcollar_g", {-0.05f, 0.0f, 1.52f}},
        {"rcollar_g", {0.05f, 0.0f, 1.52f}},
        {"lcfngrt_g", {-0.82f, 0.0f, 1.45f}},
        {"rcfngrt_g", {0.82f, 0.0f, 1.45f}},
        {"lfoott_g", {-0.1f, 0.15f, 0.03f}},
        {"rfoott_g", {0.1f, 0.15f, 0.03f}},
        {"lbicep_g", {-0.2f, 0.0f, 1.45f}},
        {"lforearm_g", {-0.45f, 0.0f, 1.45f}},
        {"lhand_g", {-0.7f, 0.0f, 1.45f}},
        {"rbicep_g", {0.2f, 0.0f, 1.45f}},
        {"rforearm_g", {0.45f, 0.0f, 1.45f}},
        {"rhand_g", {0.7f, 0.0f, 1.45f}},
        {"lthigh_g", {-0.1f, 0.0f, 0.95f}},
        {"lshin_g", {-0.1f, 0.0f, 0.5f}},
        {"lfoot_g", {-0.1f, 0.0f, 0.08f}},
        {"rthigh_g", {0.1f, 0.0f, 0.95f}},
        {"rshin_g", {0.1f, 0.0f, 0.5f}},
        {"rfoot_g", {0.1f, 0.0f, 0.08f}}};
}

std::function<std::optional<glm::vec3>(const std::string &)> lookUp(std::map<std::string, glm::vec3> nodes) {
    return [nodes = std::move(nodes)](const std::string &name) -> std::optional<glm::vec3> {
        auto it = nodes.find(name);
        if (it == nodes.end()) {
            return std::nullopt;
        }
        return it->second;
    };
}

void expectNear(const glm::vec3 &expected, const glm::vec3 &actual) {
    EXPECT_NEAR(expected.x, actual.x, 1e-4f);
    EXPECT_NEAR(expected.y, actual.y, 1e-4f);
    EXPECT_NEAR(expected.z, actual.z, 1e-4f);
}

// KOTOR's base items as the tests need them, at their retail rows: a
// quarterstaff, a stun baton, a long sword, a vibrosword, a vibroblade, a
// vibro double-blade, a lightsaber (2d8) and a blaster pistol.
std::shared_ptr<TwoDA> weaponTable() {
    TwoDA::Builder builder;
    builder.columns({"maxattackrange", "crithitmult", "critthreat", "damageflags", "dietoroll",
                     "equipableslots", "itemclass", "numdice", "weapontype", "weaponwield",
                     "ammunitiontype", "bodyvar"});
    for (int i = 0; i <= 12; ++i) {
        if (i == 0) {
            builder.row({"1.5", "2", "2", "1", "6", "48", "w_qtrstaff", "1", "1", "3", "-1", ""});
        } else if (i == 1) {
            builder.row({"1.5", "2", "2", "2", "4", "48", "w_stunbaton", "1", "1", "1", "-1", ""});
        } else if (i == 2) {
            builder.row({"1.5", "2", "1", "4", "12", "48", "w_lngswrd", "1", "1", "2", "-1", ""});
        } else if (i == 3) {
            builder.row({"1.5", "2", "2", "4", "6", "48", "w_vbroswrd", "2", "1", "2", "-1", ""});
        } else if (i == 5) {
            builder.row({"1.5", "2", "2", "2", "10", "48", "w_vbroshort", "1", "1", "2", "-1", ""});
        } else if (i == 7) {
            builder.row({"1.5", "2", "1", "4", "8", "48", "w_vbrdblswd", "2", "1", "3", "-1", ""});
        } else if (i == 8) {
            builder.row({"1.5", "2", "2", "2", "8", "48", "w_lghtsbr", "2", "1", "2", "-1", ""});
        } else if (i == 12) {
            builder.row({"23", "2", "2", "2", "6", "48", "w_blstrpstl", "1", "4", "4", "-1", ""});
        } else {
            builder.row({"", "", "", "", "", "", "", "", "", "", "", ""});
        }
    }
    return builder.build();
}

std::shared_ptr<Item> weapon(Game &game, int baseItem) {
    auto gff = Gff::Builder()
                   .field(Gff::Field::newCExoString("Tag", "test_weapon_" + std::to_string(baseItem)))
                   .field(Gff::Field::newInt("BaseItem", baseItem))
                   .field(Gff::Field::newWord("StackSize", 1))
                   .build();
    auto item = game.newItem();
    item->deserialize(*gff, SerializedIdentityContext::templateResource());
    return item;
}

constexpr int kQuarterstaff = 0;
constexpr int kStunBaton = 1;
constexpr int kLongSword = 2;
constexpr int kVibrosword = 3;
constexpr int kVibroblade = 5;
constexpr int kVibroDoubleBlade = 7;
constexpr int kLightsaber = 8;
constexpr int kBlasterPistol = 12;

} // namespace

// Sweeping

TEST(JaSweepBlade, should_find_the_blade_cutting_through_a_part_between_two_ticks) {
    // when: the level blade crosses the post, from 1 m to its left to 1 m to its right
    auto contact = jaSweepBlade(levelBlade(-1.0f), levelBlade(1.0f), postAtOrigin());

    // then: it touches it once its base is within the post's and the blade's
    // radii of the post, a quarter of a metre: 3/8 of the way, to a step
    ASSERT_TRUE(contact);
    EXPECT_EQ(0u, contact->part);
    EXPECT_GE(contact->time, 0.375f - 1e-4f);
    EXPECT_LE(contact->time, 0.5f);
    // at the point of the blade nearest the post's axis
    EXPECT_NEAR(0.0f, contact->point.y, 1e-4f);
    EXPECT_NEAR(1.0f, contact->point.z, 1e-4f);
}

TEST(JaSweepBlade, should_miss_a_part_the_blade_passes_beside) {
    // given: the post a metre beyond the blade's base, 0.4 m past its tip
    auto parts = postAtOrigin();
    parts[0].a.y = parts[0].b.y = 0.9f;

    // when: the blade crosses in front of it
    // then: nothing is hit
    EXPECT_FALSE(jaSweepBlade(levelBlade(-1.0f), levelBlade(1.0f), parts));
}

TEST(JaSweepBlade, should_count_the_blades_thickness) {
    // given: the post's axis 0.22 m past the blade's tip, so the post's own
    // 0.2 m doesn't reach it
    auto parts = postAtOrigin();
    parts[0].a.y = parts[0].b.y = 0.72f;

    // then: a blade 5 cm thick touches it, and one with no thickness misses it
    EXPECT_TRUE(jaSweepBlade(levelBlade(-1.0f), levelBlade(1.0f), parts, 0.05f));
    EXPECT_FALSE(jaSweepBlade(levelBlade(-1.0f), levelBlade(1.0f), parts, 0.0f));
}

TEST(JaSweepBlade, should_not_let_a_fast_turn_skip_a_thin_part) {
    // given: a 1 m blade turning a quarter of a turn about its base in one
    // tick, from pointing along +X to pointing along +Y, and an arm 3 cm round
    // standing in the way, 0.8 m out at 45 degrees. A straight line between
    // the two tips passes 0.71 m out, missing it.
    JaBlade from {glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 1.0f)};
    JaBlade to {glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 1.0f)};
    float out = 0.8f * std::sqrt(0.5f);
    JaBodyPart arm;
    arm.region = JaBodyRegion::LeftArm;
    arm.a = glm::vec3(out, out, 0.5f);
    arm.b = glm::vec3(out, out, 1.5f);
    arm.radius = 0.03f;

    // when
    auto contact = jaSweepBlade(from, to, {arm});

    // then: the blade, turning, cuts the arm halfway through the turn
    ASSERT_TRUE(contact);
    EXPECT_NEAR(0.5f, contact->time, 0.1f);
    EXPECT_NEAR(0.8f, glm::length(glm::vec2(contact->point)), 0.06f);
}

TEST(JaSweepBlade, should_find_a_blade_already_inside_a_part) {
    // given: a blade standing still, through the post
    auto blade = levelBlade(0.0f);

    // then: it touches the post at once
    auto contact = jaSweepBlade(blade, blade, postAtOrigin());
    ASSERT_TRUE(contact);
    EXPECT_FLOAT_EQ(0.0f, contact->time);
}

TEST(JaSweepBlade, should_take_the_part_it_reaches_first) {
    // given: one post in the blade's way halfway, and another just ahead of it
    auto parts = postAtOrigin(0.1f);
    JaBodyPart nearer = parts[0];
    nearer.region = JaBodyRegion::Head;
    nearer.a.x = nearer.b.x = -0.6f;
    parts.push_back(nearer);

    // when: the blade sweeps through both
    auto contact = jaSweepBlade(levelBlade(-1.0f), levelBlade(1.0f), parts);

    // then: the first contact is with the nearer post
    ASSERT_TRUE(contact);
    EXPECT_EQ(1u, contact->part);
    EXPECT_LT(contact->time, 0.3f);
}

TEST(JaSweepBlade, should_find_nothing_without_parts) {
    EXPECT_FALSE(jaSweepBlade(levelBlade(-1.0f), levelBlade(1.0f), {}));
}

// Bodies

TEST(JaBodyParts, should_build_a_humanoid_from_its_skeleton) {
    // when
    auto parts = jaHumanoidParts(lookUp(humanoidNodes()));

    // then: sixteen capsules, sized by the pelvis to the head's hook, 0.6 m
    ASSERT_EQ(16u, parts.size());
    // the torso from the pelvis to between the collars
    const auto &torso = parts[0];
    EXPECT_EQ(JaBodyRegion::Torso, torso.region);
    expectNear({0.0f, 0.0f, 1.0f}, torso.a);
    expectNear({0.0f, 0.0f, 1.52f}, torso.b);
    EXPECT_NEAR(0.34f * 0.6f, torso.radius, 1e-5f);
    // the head on its hook, along the neck
    const auto &head = parts[1];
    EXPECT_EQ(JaBodyRegion::Head, head.region);
    expectNear({0.0f, 0.0f, 1.6f}, head.a);
    expectNear({0.0f, 0.0f, 1.6f + 0.45f * 0.6f}, head.b);
    // the shoulders and the hips across
    expectNear({-0.2f, 0.0f, 1.45f}, parts[2].a);
    expectNear({0.2f, 0.0f, 1.45f}, parts[2].b);
    EXPECT_EQ(JaBodyRegion::Torso, parts[3].region);
    expectNear({-0.1f, 0.0f, 0.95f}, parts[3].a);
    // the left hand to its middle fingertip, and the left foot to its toe
    const auto &leftHand = parts[6];
    EXPECT_EQ(JaBodyRegion::LeftArm, leftHand.region);
    expectNear({-0.7f, 0.0f, 1.45f}, leftHand.a);
    expectNear({-0.82f, 0.0f, 1.45f}, leftHand.b);
    const auto &leftFoot = parts[9];
    EXPECT_EQ(JaBodyRegion::LeftLeg, leftFoot.region);
    expectNear({-0.1f, 0.0f, 0.08f}, leftFoot.a);
    expectNear({-0.1f, 0.15f, 0.03f}, leftFoot.b);
    std::map<JaBodyRegion, int> regions;
    for (auto &part : parts) {
        ++regions[part.region];
    }
    std::map<JaBodyRegion, int> expected {
        {JaBodyRegion::Torso, 3}, {JaBodyRegion::Head, 1}, {JaBodyRegion::LeftArm, 3}, {JaBodyRegion::RightArm, 3}, {JaBodyRegion::LeftLeg, 3}, {JaBodyRegion::RightLeg, 3}};
    EXPECT_EQ(expected, regions);
}

TEST(JaBodyParts, should_reach_past_the_wrist_and_leave_the_feet_out_without_fingertips_and_toes) {
    auto nodes = humanoidNodes();
    for (const char *name : {"lcfngrt_g", "rcfngrt_g", "lfoott_g", "rfoott_g"}) {
        nodes.erase(name);
    }

    auto parts = jaHumanoidParts(lookUp(nodes));

    ASSERT_EQ(14u, parts.size());
    const auto &leftHand = parts[6];
    expectNear({-0.7f, 0.0f, 1.45f}, leftHand.a);
    expectNear({-0.7f - 0.3f * 0.6f, 0.0f, 1.45f}, leftHand.b);
}

TEST(JaBodyParts, should_have_no_humanoid_without_all_its_main_nodes) {
    auto nodes = humanoidNodes();
    nodes.erase("lfoot_g");
    EXPECT_TRUE(jaHumanoidParts(lookUp(nodes)).empty());
}

TEST(JaBodyParts, should_stand_a_capsule_in_a_box) {
    // given: a box 0.6 m by 0.4 m and 1.8 m tall, moved 5 m along X
    AABB box(glm::vec3(-0.3f, -0.2f, 0.0f), glm::vec3(0.3f, 0.2f, 1.8f));

    // when
    auto part = jaUprightBody(box, glm::translate(glm::vec3(5.0f, 0.0f, 0.0f)));

    // then: as round as the narrower side, and inside the box
    EXPECT_EQ(JaBodyRegion::Body, part.region);
    EXPECT_NEAR(0.2f, part.radius, 1e-5f);
    expectNear({5.0f, 0.0f, 0.2f}, part.a);
    expectNear({5.0f, 0.0f, 1.6f}, part.b);
}

// Which moves hit

TEST(JaSaberContacts, should_hit_with_attacks_and_spins_only) {
    const auto &moves = JaSaberMoves::fast();
    auto hits = [&](const char *name) { return jaMoveHits(moves.move(moves.find(name))); };
    EXPECT_TRUE(hits("LS_A_TL2BR"));
    EXPECT_TRUE(hits("LS_A_T2B"));
    EXPECT_TRUE(hits("LS_T1_BR_TL")); // a spin
    EXPECT_FALSE(hits("LS_T1_BR__R"));
    EXPECT_FALSE(hits("LS_S_TL2BR"));
    EXPECT_FALSE(hits("LS_R_TL2BR"));
    EXPECT_FALSE(hits("LS_READY"));
}

TEST(JaSaberContacts, should_hit_a_creature_once_a_move) {
    JaSaberContacts contacts;
    std::vector<JaContactTarget> targets {target(7, postAtOrigin())};

    // given: an attack, the blade to the post's left
    EXPECT_TRUE(contacts.update(1, true, {levelBlade(-1.0f)}, targets).empty());

    // when: it sweeps through
    auto hits = contacts.update(1, true, {levelBlade(1.0f)}, targets);

    // then: the post is hit
    ASSERT_EQ(1u, hits.size());
    EXPECT_EQ(7u, hits[0].target);
    EXPECT_EQ(JaBodyRegion::Body, hits[0].region);

    // and sweeping back through in the same move hits it no more
    EXPECT_TRUE(contacts.update(1, true, {levelBlade(-1.0f)}, targets).empty());

    // but the next move hits it again
    EXPECT_EQ(1u, contacts.update(2, true, {levelBlade(1.0f)}, targets).size());
}

TEST(JaSaberContacts, should_hit_nothing_in_a_move_that_doesnt_hit) {
    JaSaberContacts contacts;
    std::vector<JaContactTarget> targets {target(7, postAtOrigin())};
    contacts.update(1, false, {levelBlade(-1.0f)}, targets);

    // when: a return sweeps through the post
    // then: it passes through
    EXPECT_TRUE(contacts.update(1, false, {levelBlade(1.0f)}, targets).empty());

    // and the attack that follows sweeps on from where the return left the blade
    EXPECT_EQ(1u, contacts.update(2, true, {levelBlade(-1.0f)}, targets).size());
}

TEST(JaSaberContacts, should_not_sweep_from_a_blade_before_a_reset_or_a_jump) {
    JaSaberContacts contacts;
    std::vector<JaContactTarget> targets {target(7, postAtOrigin())};

    // given: the blade left of the post, then saber mode starting over
    contacts.update(1, true, {levelBlade(-1.0f)}, targets);
    contacts.reset();

    // when: the blade is next seen right of it
    // then: nothing was swept, so nothing is hit
    EXPECT_TRUE(contacts.update(1, true, {levelBlade(1.0f)}, targets).empty());

    // when: the character is moved somewhere else at once, so that the
    // blade jumps from right of the post, across it, to 20 m left of it, and
    // then most of the way back
    // then: no blade sweeps the space between, either way
    EXPECT_TRUE(contacts.update(2, true, {levelBlade(-20.0f)}, targets).empty());
    EXPECT_TRUE(contacts.update(2, true, {levelBlade(-1.0f)}, targets).empty());
    // and from there, the blade sweeps again: across the post, it hits
    EXPECT_EQ(1u, contacts.update(2, true, {levelBlade(1.0f)}, targets).size());
}

TEST(JaSaberContacts, should_hit_with_either_blade) {
    JaSaberContacts contacts;
    std::vector<JaContactTarget> targets {target(7, postAtOrigin())};
    JaBlade high {glm::vec3(-1.0f, -1.0f, 3.0f), glm::vec3(-1.0f, 0.5f, 3.0f)};
    JaBlade highAfter {glm::vec3(1.0f, -1.0f, 3.0f), glm::vec3(1.0f, 0.5f, 3.0f)};

    // given: a double blade, one above the post, the other level with it
    contacts.update(1, true, {high, levelBlade(-1.0f)}, targets);

    // when: both sweep across
    auto hits = contacts.update(1, true, {highAfter, levelBlade(1.0f)}, targets);

    // then: the lower one hits, once
    EXPECT_EQ(1u, hits.size());
}

TEST(JaSaberContacts, should_hit_each_creature_it_reaches) {
    JaSaberContacts contacts;
    auto beside = postAtOrigin();
    beside[0].a.y = beside[0].b.y = -0.6f;
    std::vector<JaContactTarget> targets {target(7, postAtOrigin()), target(8, beside), target(9, {})};
    contacts.update(1, true, {levelBlade(-1.0f)}, targets);

    auto hits = contacts.update(1, true, {levelBlade(1.0f)}, targets);

    ASSERT_EQ(2u, hits.size());
    EXPECT_EQ(7u, hits[0].target);
    EXPECT_EQ(8u, hits[1].target);
}

// Who can block, and the hit's damage

TEST(JaSaberDamage, should_count_vibroblades_lightsabers_and_energy_shields_as_blocking) {
    TestEngine &engine = testEngine();
    StubConsole console;
    Game game(GameID::KotOR, "", engine.options(), engine.services(), console);
    EXPECT_CALL(engine.resourceModule().twoDas(), get("baseitems"))
        .Times(AnyNumber())
        .WillRepeatedly(Return(weaponTable()));

    auto unarmed = game.newCreature();
    EXPECT_FALSE(jaCanBlockSaber(*unarmed));

    auto saber = game.newCreature();
    ASSERT_TRUE(saber->equip(InventorySlots::rightWeapon, weapon(game, kLightsaber)));
    EXPECT_TRUE(jaCanBlockSaber(*saber));

    for (int vibro : {kVibrosword, kVibroblade, kVibroDoubleBlade}) {
        auto creature = game.newCreature();
        ASSERT_TRUE(creature->equip(InventorySlots::rightWeapon, weapon(game, vibro)));
        EXPECT_TRUE(jaCanBlockSaber(*creature)) << vibro;
    }

    // a lightsaber cuts through a stun baton or a staff (the user,
    // 2026-10-20), and other weapons that aren't vibro weapons, by the
    // words of the rule (2026-10-19)
    for (int cut : {kStunBaton, kQuarterstaff, kLongSword}) {
        auto creature = game.newCreature();
        ASSERT_TRUE(creature->equip(InventorySlots::rightWeapon, weapon(game, cut)));
        EXPECT_FALSE(jaCanBlockSaber(*creature)) << cut;
    }

    // either hand counts
    auto leftHand = game.newCreature();
    ASSERT_TRUE(leftHand->equip(InventorySlots::rightWeapon, weapon(game, kStunBaton)));
    ASSERT_TRUE(leftHand->equip(InventorySlots::leftWeapon, weapon(game, kVibroblade)));
    EXPECT_TRUE(jaCanBlockSaber(*leftHand));

    auto blaster = game.newCreature();
    ASSERT_TRUE(blaster->equip(InventorySlots::rightWeapon, weapon(game, kBlasterPistol)));
    EXPECT_FALSE(jaCanBlockSaber(*blaster));

    auto shielded = game.newCreature();
    ASSERT_TRUE(shielded->equip(InventorySlots::rightWeapon, weapon(game, kBlasterPistol)));
    shielded->applyEffect(game.newEffect<ForceShieldEffect>(0), DurationType::Temporary, 30.0f);
    EXPECT_TRUE(jaCanBlockSaber(*shielded));
}

TEST(JaSaberDamage, should_spare_kotors_friendly_factions_though_hostile) {
    TestEngine &engine = testEngine();
    StubConsole console;
    Game game(GameID::KotOR, "", engine.options(), engine.services(), console);
    auto creature = game.newCreature();
    for (auto [faction, spared] : std::vector<std::pair<Faction, bool>> {
             {Faction::Friendly1, true}, {Faction::Friendly2, true}, {Faction::Hostile1, false}, {Faction::Hostile2, false}, {Faction::Neutral, false}, {Faction::Invalid, false}}) {
        creature->setFaction(faction);
        EXPECT_EQ(spared, jaSparedBySaber(*creature)) << static_cast<int>(faction);
    }
}

TEST(JaSaberDamage, should_put_large_and_huge_creatures_on_a_health_bar) {
    TestEngine &engine = testEngine();
    StubConsole console;
    Game game(GameID::KotOR, "", engine.options(), engine.services(), console);
    EXPECT_CALL(engine.resourceModule().twoDas(), get("appearance"))
        .Times(AnyNumber())
        .WillRepeatedly(Return(sizedAppearances()));

    // for each row of sizedAppearances, whether a saber's hit cuts the
    // creature down: small and medium ones yes, large and huge ones no
    for (auto [row, cut] : std::vector<std::pair<int, bool>> {{0, true}, {1, true}, {2, false}, {3, false}}) {
        auto creature = game.newCreature();
        creature->setAppearance(row);
        creature->loadAppearance();
        creature->setMaxHitPoints(300);
        creature->setCurrentHitPoints(300);
        EXPECT_EQ(!cut, jaTooBigToCut(*creature)) << row;
        EXPECT_EQ(cut ? 300 : 0, jaSaberHitExtraDamage(*creature)) << row;
    }

    // a creature whose appearance never loaded is cut down, as before
    auto unloaded = game.newCreature();
    EXPECT_FALSE(jaTooBigToCut(*unloaded));

    // The engine's mocks are shared by every test: later ones find no
    // appearance table, as before this one.
    EXPECT_CALL(engine.resourceModule().twoDas(), get("appearance")).Times(AnyNumber());
}

TEST(JaSaberDamage, should_add_the_hit_points_of_a_creature_that_cant_block) {
    TestEngine &engine = testEngine();
    StubConsole console;
    Game game(GameID::KotOR, "", engine.options(), engine.services(), console);
    EXPECT_CALL(engine.resourceModule().twoDas(), get("baseitems"))
        .Times(AnyNumber())
        .WillRepeatedly(Return(weaponTable()));

    auto blaster = game.newCreature();
    ASSERT_TRUE(blaster->equip(InventorySlots::rightWeapon, weapon(game, kBlasterPistol)));
    blaster->setMaxHitPoints(30);
    blaster->setCurrentHitPoints(30);
    EXPECT_EQ(30, jaSaberHitExtraDamage(*blaster));

    auto saber = game.newCreature();
    ASSERT_TRUE(saber->equip(InventorySlots::rightWeapon, weapon(game, kLightsaber)));
    saber->setMaxHitPoints(30);
    saber->setCurrentHitPoints(30);
    EXPECT_EQ(0, jaSaberHitExtraDamage(*saber));

    auto shielded = game.newCreature();
    shielded->setMaxHitPoints(30);
    shielded->setCurrentHitPoints(30);
    shielded->applyEffect(game.newEffect<ForceShieldEffect>(0), DurationType::Temporary, 30.0f);
    EXPECT_EQ(0, jaSaberHitExtraDamage(*shielded));
}

TEST(JaSaberDamage, should_hit_with_the_weapons_damage_and_no_attack_roll) {
    TestEngine &engine = testEngine();
    StubConsole console;
    Game game(GameID::KotOR, "", engine.options(), engine.services(), console);
    EXPECT_CALL(engine.resourceModule().twoDas(), get("baseitems"))
        .Times(AnyNumber())
        .WillRepeatedly(Return(weaponTable()));
    auto attacker = game.newCreature();
    // no strength bonus or malus: reone's default score, 8, takes 1 off
    attacker->attributes().setAbilityScore(Ability::Strength, 10);
    auto saber = weapon(game, kLightsaber);
    ASSERT_TRUE(attacker->equip(InventorySlots::rightWeapon, saber));
    auto target = game.newCreature();
    target->setMaxHitPoints(100);
    target->setCurrentHitPoints(100);
    // a defence no roll would get through
    target->attributes().setAbilityScore(Ability::Dexterity, 100);

    // when: hit, twenty times
    std::set<int> damages;
    for (int i = 0; i < 20; ++i) {
        int before = target->currentHitPoints();
        AttackBuffer attack;
        attack.addContactHit(*attacker, *target, saber.get());
        EXPECT_EQ(AttackResultType::HitSuccessful, attack.result());
        attack.resolve(*attacker, *target);
        attack.signal(game, engine.services(), *attacker, *target);
        damages.insert(before - target->currentHitPoints());
        target->setCurrentHitPoints(100);
    }

    // then: every hit lands, for the lightsaber's 2d8
    EXPECT_GE(*damages.begin(), 2);
    EXPECT_LE(*damages.rbegin(), 16);
    EXPECT_GT(damages.size(), 1u);
    EXPECT_EQ(attacker->id(), target->getLastDamager());
}

TEST(JaSaberDamage, should_kill_with_the_targets_hit_points_on_top) {
    TestEngine &engine = testEngine();
    StubConsole console;
    Game game(GameID::KotOR, "", engine.options(), engine.services(), console);
    EXPECT_CALL(engine.resourceModule().twoDas(), get("baseitems"))
        .Times(AnyNumber())
        .WillRepeatedly(Return(weaponTable()));
    auto attacker = game.newCreature();
    auto saber = weapon(game, kLightsaber);
    ASSERT_TRUE(attacker->equip(InventorySlots::rightWeapon, saber));
    auto target = game.newCreature();
    target->setMaxHitPoints(60);
    target->setCurrentHitPoints(60);

    // when: a hit that adds the target's hit points, as on one that can't block
    AttackBuffer attack;
    attack.addContactHit(*attacker, *target, saber.get(), target->currentHitPoints());
    attack.resolve(*attacker, *target);
    attack.signal(game, engine.services(), *attacker, *target);

    // then: it dies
    EXPECT_TRUE(target->isDead());
}

// Blades and bodies in a scene

TEST(JaSaberBlades, should_find_a_saber_models_blade_along_its_z) {
    auto blades = jaSaberBlades(*syntheticSaber());

    ASSERT_EQ(1u, blades.size());
    expectNear({0.0f, 0.0f, 0.05f}, blades[0].base);
    expectNear({0.0f, 0.0f, 1.05f}, blades[0].tip);
}

TEST(JaSaberBlades, should_find_one_blade_in_two_crossed_planes) {
    auto blades = jaSaberBlades(*syntheticSaber(2));

    ASSERT_EQ(1u, blades.size());
    expectNear({0.0f, 0.0f, 1.05f}, blades[0].tip);
}

TEST(JaSaberBlades, should_find_the_blade_in_the_hand_and_the_body_where_its_nodes_are) {
    // given: a KOTOR-like body 5 m along X, holding the saber in its weapon node
    auto graphicsOpt = GraphicsOptions();
    auto pipelineFactory = MockRenderPipelineFactory();
    auto graphicsModule = TestGraphicsModule();
    graphicsModule.init();
    auto audioModule = TestAudioModule();
    audioModule.init();
    auto resourceModule = TestResourceModule();
    resourceModule.init();
    auto scene = std::make_unique<SceneGraph>("test", pipelineFactory, graphicsOpt, graphicsModule.services(), audioModule.services(), resourceModule.services());
    auto bodyModel = syntheticKotorBody(false);
    auto saberModel = syntheticSaber();
    auto body = std::make_shared<ModelSceneNode>(*bodyModel, ModelUsage::Creature, *scene, graphicsModule.services(), audioModule.services(), resourceModule.services());
    body->init();
    auto saber = std::make_shared<ModelSceneNode>(*saberModel, ModelUsage::Equipment, *scene, graphicsModule.services(), audioModule.services(), resourceModule.services());
    saber->init();
    body->attach("rhand", *saber);
    body->setLocalTransform(glm::translate(glm::vec3(5.0f, 0.0f, 0.0f)));
    auto hand = body->getNodeByName("rhand");
    ASSERT_TRUE(hand);

    // when
    auto blades = jaSaberBlades(*saber, *hand);
    auto parts = jaBodyParts(*body);

    // then: the blade, in the hand's space, runs along the weapon's +Z
    ASSERT_EQ(1u, blades.size());
    expectNear({0.0f, 0.0f, 0.05f}, blades[0].base);
    expectNear({0.0f, 0.0f, 1.05f}, blades[0].tip);
    // and in the world, the hand holds it there
    auto inWorld = jaTransformBlade(hand->absoluteTransform(), blades[0]);
    expectNear(glm::vec3(hand->absoluteTransform() * glm::vec4(0.0f, 0.0f, 1.05f, 1.0f)), inWorld.tip);
    // and the body's capsules stand where its nodes now are, in the world:
    // fourteen, as it has neither fingertips nor toes
    ASSERT_EQ(14u, parts.size());
    auto at = [&](const char *name) { return glm::vec3(body->getNodeByName(name)->absoluteTransform()[3]); };
    expectNear(at("pelvis_g"), parts[0].a);
    expectNear(0.5f * (at("lcollar_g") + at("rcollar_g")), parts[0].b);
    EXPECT_NEAR(5.0f, parts[0].a.x, 1e-4f);
}

TEST(JaSaberBlades, should_follow_a_blade_powering_up_and_leave_one_powered_down_out) {
    // given: the saber in a scene, its blades measured in its own space
    auto graphicsOpt = GraphicsOptions();
    auto pipelineFactory = MockRenderPipelineFactory();
    auto graphicsModule = TestGraphicsModule();
    graphicsModule.init();
    auto audioModule = TestAudioModule();
    audioModule.init();
    auto resourceModule = TestResourceModule();
    resourceModule.init();
    auto scene = std::make_unique<SceneGraph>("test", pipelineFactory, graphicsOpt, graphicsModule.services(), audioModule.services(), resourceModule.services());
    auto saberModel = syntheticSaber();
    auto saber = std::make_shared<ModelSceneNode>(*saberModel, ModelUsage::Equipment, *scene, graphicsModule.services(), audioModule.services(), resourceModule.services());
    saber->init();
    auto bladeMesh = saber->getNodeByNumber(1);
    ASSERT_TRUE(bladeMesh);

    // when: the blade's mesh is scaled to half its length, as a power
    // animation scales it on the way
    bladeMesh->setLocalTransform(glm::translate(glm::vec3(0.0f, 0.0f, 0.05f)) * glm::scale(glm::vec3(1.0f, 1.0f, 0.5f)));
    auto half = jaSaberBlades(*saber, *saber);
    // and to half a centimetre
    bladeMesh->setLocalTransform(glm::translate(glm::vec3(0.0f, 0.0f, 0.05f)) * glm::scale(glm::vec3(1.0f, 1.0f, 0.005f)));
    auto down = jaSaberBlades(*saber, *saber);

    // then: the blade is as long as its mesh shows, and gone once under 1 cm
    ASSERT_EQ(1u, half.size());
    expectNear({0.0f, 0.0f, 0.05f}, half[0].base);
    expectNear({0.0f, 0.0f, 0.55f}, half[0].tip);
    EXPECT_TRUE(down.empty());
}

// Runs only with the player's own files:
//   REONE_TEST_JA_HUMANOID: folder with Jedi Academy's _humanoid.gla and animation.cfg
//   REONE_TEST_KOTOR_BODY: path of a KOTOR body model without extension (for example .../pmbbl)
//   REONE_TEST_KOTOR_SABER: path of a KOTOR lightsaber model without extension (for example .../w_lghtsbr_001)
//   REONE_TEST_KOTOR_HEAD (optional): path of a KOTOR head model without extension (for example .../pmhc01)
TEST(JaSaberContactsOnGameFiles, should_hit_a_body_ahead_with_attacks_and_spins_and_nothing_out_of_reach) {
    const char *humanoidDir = std::getenv("REONE_TEST_JA_HUMANOID");
    const char *bodyPath = std::getenv("REONE_TEST_KOTOR_BODY");
    const char *saberPath = std::getenv("REONE_TEST_KOTOR_SABER");
    const char *headPath = std::getenv("REONE_TEST_KOTOR_HEAD");
    if (headPath && !*headPath) {
        headPath = nullptr;
    }
    if (!humanoidDir || !bodyPath || !saberPath) {
        GTEST_SKIP() << "Set REONE_TEST_JA_HUMANOID, REONE_TEST_KOTOR_BODY and REONE_TEST_KOTOR_SABER to run against game files";
    }
    std::filesystem::path humanoid(humanoidDir);
    auto glaStream = FileInputStream(humanoid / "_humanoid.gla");
    auto glaReader = GlaReader(glaStream);
    glaReader.load();
    auto gla = glaReader.animation();
    std::ifstream cfgFile(humanoid / "animation.cfg");
    std::stringstream cfgText;
    cfgText << cfgFile.rdbuf();
    auto clips = parseGhoulAnimationConfig(cfgText.str());
    Statistic statistic;
    auto loadModel = [&statistic](const std::string &path) {
        auto mdl = FileInputStream(path + ".mdl");
        auto mdx = FileInputStream(path + ".mdx");
        auto reader = MdlMdxReader(mdl, mdx, statistic);
        reader.load();
        return reader.model();
    };
    auto body = loadModel(bodyPath);
    auto saber = loadModel(saberPath);
    auto retargeter = JaRetargeter(*gla, *body);
    auto sequencer = JaSequencer(retargeter, clips, 60.0f);
    const auto &moves = JaSaberMoves::fast();
    float bladeRadius = kJaBladeTraceUnits * retargeter.scale();

    // The lightsaber's blade, in the weapon's space.
    auto bladesInHand = jaSaberBlades(*saber);
    ASSERT_EQ(1u, bladesInHand.size());
    const JaBlade &blade = bladesInHand[0];
    float bladeLength = glm::distance(blade.base, blade.tip);
    std::cout << "blade from (" << blade.base.x << ", " << blade.base.y << ", " << blade.base.z << ") to (" << blade.tip.x << ", "
              << blade.tip.y << ", " << blade.tip.z << "), " << bladeLength << " m; " << bladeRadius << " m thick either way" << std::endl;
    EXPECT_GT(bladeLength, 0.5f);
    EXPECT_LT(bladeLength, 2.0f);
    EXPECT_GT(blade.tip.z, blade.base.z);

    // A target: the same body at rest, \p distance metres from the attacker
    // at \p bearing degrees to the left of +Y, KOTOR's forward, facing back.
    auto targetAt = [&](float distance, float bearing = 0.0f) {
        float angle = glm::radians(bearing);
        glm::mat4 toWorld(glm::translate(glm::vec3(-std::sin(angle) * distance, std::cos(angle) * distance, 0.0f)) *
                          glm::rotate(angle + glm::pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f)));
        return jaHumanoidParts([&](const std::string &name) -> std::optional<glm::vec3> {
            auto node = body->getNodeByName(name);
            if (!node) {
                return std::nullopt;
            }
            return glm::vec3(toWorld * node->absoluteTransform()[3]);
        });
    };
    auto atOrigin = targetAt(0.0f);
    ASSERT_EQ(16u, atOrigin.size()) << "the body lacks a humanoid's nodes, its fingertips or its toes";

    // How well the capsules fit the body at rest: how many of the skin's
    // vertices lie inside them, and inside them with the blade's thickness.
    std::vector<glm::vec3> skin;
    auto collectSkin = [&](const Model &model, const glm::mat4 &toBody) {
        std::function<void(const ModelNode &)> visit = [&](const ModelNode &node) {
            if (node.mesh() && node.mesh()->mesh && !node.isSaberMesh()) {
                for (auto &vertex : node.mesh()->mesh->vertices()) {
                    skin.push_back(glm::vec3(toBody * node.absoluteTransform() * glm::vec4(vertex.position, 1.0f)));
                }
            }
            for (auto &child : node.children()) {
                visit(*child);
            }
        };
        visit(*model.rootNode());
    };
    collectSkin(*body, glm::rotate(glm::pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f)));
    if (headPath) {
        auto head = loadModel(headPath);
        glm::mat4 hook(body->getNodeByName("headhook")->absoluteTransform());
        collectSkin(*head, glm::rotate(glm::pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f)) * hook);
    }
    ASSERT_FALSE(skin.empty());
    if (const char *dumpDir = std::getenv("REONE_TEST_JA_DUMP")) {
        std::ofstream skinFile(std::filesystem::path(dumpDir) / "skin.txt");
        for (auto &point : skin) {
            skinFile << point.x << " " << point.y << " " << point.z << "\n";
        }
        std::ofstream partsFile(std::filesystem::path(dumpDir) / "capsules.txt");
        for (auto &part : atOrigin) {
            partsFile << jaBodyRegionName(part.region) << ";" << part.a.x << " " << part.a.y << " " << part.a.z << " " << part.b.x << " "
                      << part.b.y << " " << part.b.z << " " << part.radius << "\n";
        }
        std::ofstream nodesFile(std::filesystem::path(dumpDir) / "nodes.txt");
        for (auto &node : retargeter.nodes()) {
            glm::vec3 at(glm::rotate(glm::pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f)) * node.bindTransform[3]);
            nodesFile << node.name << " " << at.x << " " << at.y << " " << at.z << "\n";
        }
    }
    auto outside = [&](const glm::vec3 &point) {
        float nearest = std::numeric_limits<float>::max();
        for (auto &part : atOrigin) {
            glm::vec3 along(part.b - part.a);
            float t = glm::dot(along, along) > 0.0f ? glm::clamp(glm::dot(point - part.a, along) / glm::dot(along, along), 0.0f, 1.0f) : 0.0f;
            nearest = std::min(nearest, glm::distance(point, part.a + along * t) - part.radius);
        }
        return nearest;
    };
    std::vector<float> distances;
    for (auto &point : skin) {
        distances.push_back(outside(point));
    }
    std::sort(distances.begin(), distances.end());
    auto share = [&](float within) {
        return static_cast<float>(std::upper_bound(distances.begin(), distances.end(), within) - distances.begin()) / distances.size();
    };
    std::cout << skin.size() << " skin vertices" << (headPath ? " with the head" : "") << ": " << 100.0f * share(0.0f) << "% inside the capsules, "
              << 100.0f * share(bladeRadius) << "% within the blade's thickness of them; the farthest out "
              << distances.back() << " m, the 95th percentile " << distances[distances.size() * 95 / 100] << " m" << std::endl;
    // and how far the capsules stand out of the skin: for points on their
    // surfaces, the distance to the nearest vertex
    std::vector<float> standOff;
    for (auto &part : atOrigin) {
        glm::vec3 axis(part.b - part.a);
        glm::vec3 up(glm::length(axis) > 1e-4f ? glm::normalize(axis) : glm::vec3(0.0f, 0.0f, 1.0f));
        glm::vec3 side(glm::normalize(glm::cross(up, std::abs(up.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f))));
        glm::vec3 other(glm::cross(up, side));
        for (int i = 0; i <= 4; ++i) {
            for (int j = 0; j < 12; ++j) {
                float angle = glm::two_pi<float>() * j / 12.0f;
                glm::vec3 point(part.a + axis * (i / 4.0f) + part.radius * (std::cos(angle) * side + std::sin(angle) * other));
                float nearest = std::numeric_limits<float>::max();
                for (auto &vertex : skin) {
                    nearest = std::min(nearest, glm::distance(point, vertex));
                }
                standOff.push_back(nearest);
            }
        }
    }
    std::sort(standOff.begin(), standOff.end());
    std::cout << "capsule surface to the nearest skin vertex: median " << standOff[standOff.size() / 2] << " m, 90th percentile "
              << standOff[standOff.size() * 9 / 10] << " m" << std::endl;

    // Swings, as a player presses the keys, 60 ticks a second, the attacker
    // at the origin facing +Y, as JaSaber turns it: the legs' yaw.
    struct Scenario {
        std::string name;
        float seconds;
        std::function<JaSaberInput(float)> input;
    };
    auto tap = [](int forward, int right) {
        return [forward, right](float t) {
            JaSaberInput input;
            if (t >= 0.5f && t < 0.5f + 1.5f / 60.0f) {
                input.attack = true;
                input.forward = forward;
                input.right = right;
            }
            return input;
        };
    };
    std::vector<Scenario> scenarios {
        {"tap_forward_right", 1.6f, tap(1, 1)},
        {"tap_right", 1.6f, tap(0, 1)},
        {"tap_back_right", 1.6f, tap(-1, 1)},
        {"tap_forward_left", 1.6f, tap(1, -1)},
        {"tap_left", 1.6f, tap(0, -1)},
        {"tap_back_left", 1.6f, tap(-1, -1)},
        {"tap_forward", 1.6f, tap(1, 0)},
        {"tap", 1.6f, tap(0, 0)},
        {"hold_forward_right", 2.0f, [](float t) {
             JaSaberInput input;
             input.attack = t >= 0.5f;
             input.forward = input.attack ? 1 : 0;
             input.right = input.attack ? 1 : 0;
             return input;
         }},
        {"stance", 2.0f, [](float) { return JaSaberInput(); }}};

    int weapon = retargeter.nodeIndex("rhand");
    JaRetargeter::Pose bind;
    for (auto &node : retargeter.nodes()) {
        bind.orientations.push_back(node.restOrientation);
    }
    bind.rootPosition = retargeter.nodes()[retargeter.rootNodeIndex()].restPosition;
    // Runs a scenario against \p targets, \p rate ticks a second; returns
    // each hit as "move at time, target, part".
    auto run = [&](const Scenario &scenario, const std::vector<JaContactTarget> &targets, float &reach, float rate = 60.0f) {
        std::vector<std::pair<uint32_t, std::string>> hits;
        JaSaberDriver driver(sequencer, retargeter, clips);
        driver.start(bind);
        JaSaberContacts contacts;
        contacts.setBladeRadius(bladeRadius);
        int serial = 0;
        reach = 0.0f;
        int ticks = static_cast<int>(std::lround(scenario.seconds * rate));
        for (int tick = 0; tick <= ticks; ++tick) {
            float t = tick / rate;
            auto step = driver.step(tick == 0 ? 0.0f : 1.0f / rate, scenario.input(t), false, 3.96f);
            serial += static_cast<int>(step.started.size());
            glm::mat4 facing(glm::rotate(driver.legsYaw(), glm::vec3(0.0f, 0.0f, 1.0f)));
            glm::mat4 hand(facing * retargeter.transforms(driver.pose())[weapon]);
            JaBlade now(jaTransformBlade(hand, blade));
            reach = std::max(reach, glm::length(glm::vec2(now.tip)));
            const auto &playing = moves.move(driver.machine().move());
            for (auto &hit : contacts.update(serial, jaMoveHits(playing), {now}, targets)) {
                hits.push_back({hit.target, str(boost::format("%s at %.3f s, %s") % playing.name % t % jaBodyRegionName(hit.region))});
            }
        }
        return hits;
    };

    // Each swing against one body ahead, nearer and farther.
    std::map<std::string, size_t> hitsNear;
    for (auto &scenario : scenarios) {
        for (float distance : {0.8f, 1.2f, 1.6f, 3.5f}) {
            float reach;
            auto hits = run(scenario, {{7, targetAt(distance)}}, reach);
            std::vector<std::string> lines;
            for (auto &hit : hits) {
                lines.push_back(hit.second);
            }
            std::cout << scenario.name << ", target " << distance << " m ahead: " << hits.size() << " hits"
                      << (hits.empty() ? "" : ": " + boost::join(lines, "; ")) << "; the tip reaches " << reach << " m out" << std::endl;
            if (distance == 1.2f) {
                hitsNear[scenario.name] = hits.size();
            }
            if (distance == 3.5f) {
                EXPECT_TRUE(hits.empty()) << scenario.name << " hit a body out of reach";
            }
        }
    }
    // then: 1.2 m ahead, every swing hits once, the chain hits in both its
    // attacks, and the stance hits nothing
    for (auto &scenario : scenarios) {
        if (scenario.name == "stance") {
            EXPECT_EQ(0u, hitsNear[scenario.name]);
        } else if (scenario.name == "hold_forward_right") {
            EXPECT_EQ(2u, hitsNear[scenario.name]);
        } else {
            EXPECT_EQ(1u, hitsNear[scenario.name]) << scenario.name;
        }
    }

    // The same swings 1.2 m ahead at other tick rates: the sweep covers the
    // blade's whole path between two ticks, so they hit as at 60 Hz.
    for (float rate : {30.0f, 144.0f}) {
        for (auto &scenario : scenarios) {
            float reach;
            auto hits = run(scenario, {{7, targetAt(1.2f)}}, reach, rate);
            std::vector<std::string> lines;
            for (auto &hit : hits) {
                lines.push_back(hit.second);
            }
            std::cout << scenario.name << " at " << rate << " Hz, target 1.2 m ahead: " << hits.size() << " hits"
                      << (hits.empty() ? "" : ": " + boost::join(lines, "; ")) << std::endl;
            EXPECT_EQ(hitsNear[scenario.name], hits.size()) << scenario.name << " at " << rate << " Hz";
        }
    }

    // The chain against eight bodies around the attacker, 1.2 m away: the
    // spins, which turn the whole body around, sweep the blade round it.
    std::vector<JaContactTarget> ring;
    for (int i = 0; i < 8; ++i) {
        ring.push_back({static_cast<uint32_t>(i), targetAt(1.2f, 45.0f * i)});
    }
    float reach;
    auto hits = run(scenarios[8], ring, reach);
    int spinHits = 0;
    for (auto &hit : hits) {
        std::cout << "hold_forward_right, ring of bodies 1.2 m away: the body at " << 45 * hit.first << " degrees to the left: " << hit.second << std::endl;
        spinHits += hit.second.find("LS_T1_BR_TL") != std::string::npos ? 1 : 0;
    }
    // then: the spins hit bodies around the attacker too
    EXPECT_GT(spinHits, 0);
}

// Runs only with the player's own files:
//   REONE_TEST_KOTOR_SABER: path of a KOTOR lightsaber model without extension (for example .../w_lghtsbr_001)
TEST(JaSaberBladesOnGameFiles, should_follow_a_kotor_sabers_power_animations) {
    const char *saberPath = std::getenv("REONE_TEST_KOTOR_SABER");
    if (!saberPath) {
        GTEST_SKIP() << "Set REONE_TEST_KOTOR_SABER to run against game files";
    }
    Statistic statistic;
    auto mdl = FileInputStream(std::string(saberPath) + ".mdl");
    auto mdx = FileInputStream(std::string(saberPath) + ".mdx");
    auto reader = MdlMdxReader(mdl, mdx, statistic);
    reader.load();
    auto saberModel = reader.model();
    auto graphicsOpt = GraphicsOptions();
    auto pipelineFactory = MockRenderPipelineFactory();
    auto graphicsModule = TestGraphicsModule();
    graphicsModule.init();
    auto audioModule = TestAudioModule();
    audioModule.init();
    auto resourceModule = TestResourceModule();
    resourceModule.init();
    auto scene = std::make_unique<SceneGraph>("test", pipelineFactory, graphicsOpt, graphicsModule.services(), audioModule.services(), resourceModule.services());
    auto saber = std::make_shared<ModelSceneNode>(*saberModel, ModelUsage::Equipment, *scene, graphicsModule.services(), audioModule.services(), resourceModule.services());
    saber->init();
    auto length = [&]() {
        auto blades = jaSaberBlades(*saber, *saber);
        return blades.empty() ? 0.0f : glm::distance(blades[0].base, blades[0].tip);
    };
    // Plays \p name as the game does, 60 ticks a second for \p seconds;
    // returns the blade's length after each tick.
    auto play = [&](const char *name, float seconds) {
        std::vector<float> lengths;
        saber->playAnimation(name);
        int ticks = static_cast<int>(std::lround(seconds * 60.0f));
        for (int tick = 0; tick < ticks; ++tick) {
            saber->update(1.0f / 60.0f);
            lengths.push_back(length());
        }
        std::cout << name << ":";
        for (int tick = 0; tick < ticks; tick += 6) {
            std::cout << " " << lengths[tick];
        }
        std::cout << " ... " << lengths.back() << " m" << std::endl;
        return lengths;
    };
    for (auto &name : saberModel->getAnimationNames()) {
        std::cout << "animation " << name << ", " << saberModel->getAnimation(name)->length() << " s" << std::endl;
    }
    float atRest = length();
    std::cout << "at rest: " << atRest << " m" << std::endl;

    // when: the saber is off, as the game shows it out of combat, then
    // powers up, stays on and powers down
    auto off = play("off", 0.5f);
    auto up = play("powerup", 2.0f);
    auto on = play("powered", 1.0f);
    auto down = play("powerdown", 2.0f);

    // then: off, it has no blade; powering up, the blade grows to its whole
    // length and stays there; powered down, it is gone again
    EXPECT_GT(atRest, 0.5f);
    EXPECT_EQ(0.0f, off.back());
    EXPECT_TRUE(std::any_of(up.begin(), up.end(), [&](float l) { return l > 0.0f && l < 0.9f * atRest; }))
        << "no blade part of the way up";
    EXPECT_NEAR(atRest, up.back(), 0.01f);
    EXPECT_NEAR(atRest, on.back(), 0.01f);
    EXPECT_EQ(0.0f, down.back());
}
