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

#include "reone/game/game.h"
#include "reone/game/netsession.h"
#include "reone/game/object/area.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/module.h"
#include "reone/game/party.h"
#include "reone/game/script/runner.h"

using namespace reone;
using namespace reone::game;
using namespace testing;

namespace {

// One side of a session: a game with a PC, a companion at NPC 0 and a
// hostile creature, made in the same order on both sides, as loading one
// save makes them.
struct Side {
    explicit Side(TestEngine &engine) :
        game(resource::GameID::KotOR, "", engine.options(), engine.services(), console) {
        game.initLocalServices();
        auto area = game.newArea();
        area->initCameras(glm::vec3(0.0f), 0.0f);
        TestGameModule::setActiveModuleArea(game, area);
        TestGameModule::loadModulePlayer(*game.module());
        pc = game.newCreature();
        pc->setTag("player");
        pc->setMaxHitPoints(30);
        pc->setCurrentHitPoints(30);
        companion = game.newCreature();
        companion->setTag("end_trask");
        companion->setMaxHitPoints(30);
        companion->setCurrentHitPoints(30);
        companion->setPosition(glm::vec3(1.0f, 0.0f, 0.0f));
        enemy = game.newCreature();
        enemy->setTag("end_sith2");
        enemy->setFaction(Faction::Hostile1);
        enemy->setMaxHitPoints(20);
        enemy->setCurrentHitPoints(20);
        enemy->setPosition(glm::vec3(0.0f, 5.0f, 0.0f));
        game.party().addMember(kNpcPlayer, pc);
        game.party().addMember(0, companion);
        game.party().setPlayer(pc);
        area->add(pc);
        area->add(companion);
        area->add(enemy);
    }

    NetSession &net() { return game.netSession(); }

    StubConsole console;
    Game game;
    std::shared_ptr<Creature> pc;
    std::shared_ptr<Creature> companion;
    std::shared_ptr<Creature> enemy;
};

// A clock both sides share, moved on by hand.
struct Clock {
    double time {100.0};
    std::function<double()> reader() {
        return [this]() { return time; };
    }
};

class NetSessionTest : public Test {
protected:
    void SetUp() override {
        EXPECT_CALL(reputes(), getIsEnemy(An<const Creature &>(), An<const Creature &>()))
            .Times(AnyNumber())
            .WillRepeatedly(Invoke([](const Creature &source, const Creature &target) {
                return source.faction() == Faction::Hostile1 || target.faction() == Faction::Hostile1;
            }));
        host.net().setClock(clock.reader());
        guest.net().setClock(clock.reader());
    }

    void TearDown() override {
        guest.net().stop();
        host.net().stop();
        Mock::VerifyAndClearExpectations(&reputes());
    }

    MockReputes &reputes() {
        return static_cast<MockReputes &>(engine.services().game.reputes);
    }

    // Both sides' updates, \p frames times, 60 a second of the shared clock.
    void exchange(int frames = 6) {
        for (int i = 0; i < frames; ++i) {
            clock.time += 1.0 / 60.0;
            host.net().update();
            guest.net().update();
        }
    }

    TestEngine &engine {testEngine()};
    Clock clock;
    Side host {engine};
    Side guest {engine};
    uint16_t hostPort {0};
};

} // namespace

TEST(NetSocket, should_carry_a_datagram_between_two_sockets_on_this_machine) {
    NetSocket a;
    NetSocket b;
    a.open(0);
    b.open(0);
    ASSERT_NE(0, a.localPort());
    auto to = NetSocket::resolve("127.0.0.1", b.localPort());
    ASSERT_TRUE(to);

    a.send(*to, {1, 2, 3});
    std::optional<NetSocket::Datagram> got;
    for (int i = 0; i < 1000 && !got; ++i) {
        got = b.receive();
    }

    ASSERT_TRUE(got);
    EXPECT_EQ(std::vector<uint8_t>({1, 2, 3}), got->bytes);
    EXPECT_EQ(a.localPort(), got->from.port);
    EXPECT_FALSE(b.receive());
}

TEST(NetSocket, should_refuse_an_unknown_host_and_a_taken_port) {
    EXPECT_FALSE(NetSocket::resolve("no.such.host.invalid", 1));
    NetSocket a;
    a.open(0);
    NetSocket b;
    EXPECT_THROW(b.open(a.localPort()), std::runtime_error);
}

TEST_F(NetSessionTest, should_connect_and_find_the_two_sides_objects_match) {
    // given: the same objects on both sides, by runtime ID, tag and type
    EXPECT_EQ(host.pc->id(), guest.pc->id());
    EXPECT_EQ(host.net().manifestHash(), guest.net().manifestHash());

    // when
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);
    exchange(60);

    // then
    EXPECT_TRUE(host.net().connected());
    EXPECT_TRUE(guest.net().connected());
    EXPECT_EQ(std::optional<bool>(true), host.net().manifestsMatch());
    EXPECT_EQ(std::optional<bool>(true), guest.net().manifestsMatch());
    EXPECT_EQ(NetRole::Host, host.net().role());
    EXPECT_EQ(NetRole::Guest, guest.net().role());
    // the guest plays the companion, its own party leader now; the host's leader is its PC
    EXPECT_EQ(guest.companion, guest.game.party().getLeader());
    EXPECT_EQ(host.pc, host.game.party().getLeader());
}

TEST_F(NetSessionTest, should_show_each_side_the_others_hero_where_it_moves) {
    // given
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);
    exchange(60);

    // when: the host's PC and the guest's companion move
    host.pc->setPosition(glm::vec3(3.0f, 4.0f, 0.0f));
    host.pc->setFacing(1.0f);
    host.pc->setMovementType(Creature::MovementType::Run);
    guest.companion->setPosition(glm::vec3(-2.0f, 1.0f, 0.0f));
    guest.companion->setFacing(-0.5f);
    exchange(12);

    // then: each side shows the other's where it went
    EXPECT_NEAR(3.0f, guest.pc->position().x, 1e-5f);
    EXPECT_NEAR(4.0f, guest.pc->position().y, 1e-5f);
    EXPECT_NEAR(1.0f, guest.pc->getFacing(), 1e-4f);
    EXPECT_EQ(Creature::MovementType::Run, guest.pc->movementType());
    EXPECT_NEAR(-2.0f, host.companion->position().x, 1e-5f);
    EXPECT_NEAR(1.0f, host.companion->position().y, 1e-5f);
    EXPECT_NEAR(-0.5f, host.companion->getFacing(), 1e-4f);
    // and the guest's own hero stays where the guest has it
    EXPECT_NEAR(-2.0f, guest.companion->position().x, 1e-5f);
}

TEST_F(NetSessionTest, should_send_the_guests_saber_hits_to_the_host_which_decides_the_damage) {
    // given
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);
    exchange(60);

    // when: the guest's saber hits the enemy, which can't block: the hit kills
    EXPECT_TRUE(guest.net().forwardSaberHit(*guest.enemy, JaBodyRegion::Torso));
    exchange(12);

    // then: the host applied it, and the guest sees the enemy dead
    EXPECT_EQ(1, host.net().counts().hitsApplied);
    EXPECT_TRUE(host.enemy->isDead());
    EXPECT_TRUE(guest.enemy->isDead());
    // and the host's own hits stay its own
    EXPECT_FALSE(host.net().forwardSaberHit(*host.enemy, JaBodyRegion::Torso));
}

TEST_F(NetSessionTest, should_have_the_guest_follow_the_host_and_let_go_when_it_ends) {
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);

    EXPECT_TRUE(guest.game.followingHost());
    EXPECT_TRUE(guest.game.scriptRunner().suspended());
    EXPECT_FALSE(host.game.followingHost());
    EXPECT_TRUE(guest.game.keepsRunningUnfocused());
    EXPECT_TRUE(host.game.keepsRunningUnfocused());

    guest.net().stop("the test is over");
    EXPECT_FALSE(guest.game.followingHost());
    EXPECT_FALSE(guest.game.scriptRunner().suspended());
}

TEST_F(NetSessionTest, should_lose_the_guest_after_5_seconds_without_word) {
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);
    exchange(60);
    ASSERT_TRUE(host.net().connected());

    // when: the guest goes quiet
    guest.net().stop();
    for (int i = 0; i < 6 * 60; ++i) {
        clock.time += 1.0 / 60.0;
        host.net().update();
    }

    EXPECT_FALSE(host.net().connected());
}

TEST_F(NetSessionTest, should_refuse_a_member_not_in_the_party) {
    EXPECT_THROW(host.net().host(0, 5), std::runtime_error);
    EXPECT_THROW(guest.net().join("127.0.0.1", 7777, 5), std::runtime_error);
    EXPECT_EQ(NetRole::None, guest.net().role());
}
