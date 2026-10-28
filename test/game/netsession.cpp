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

#include <filesystem>
#include <fstream>
#include <sstream>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../fixtures/engine.h"

#include "reone/game/d20/class.h"
#include "reone/game/d20/classes.h"
#include "reone/game/game.h"
#include "reone/game/netsession.h"
#include "reone/game/object/area.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/module.h"
#include "reone/game/party.h"
#include "reone/game/savegame.h"
#include "reone/game/script/runner.h"
#include "reone/graphics/options.h"
#include "reone/resource/2da.h"
#include "reone/scene/graph.h"

using namespace reone;
using namespace reone::game;
using namespace reone::graphics;
using namespace reone::scene;
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
        // A companion joins the roster before the party, as KOTOR's do.
        EXPECT_TRUE(game.party().addAvailableMember(0, companion));
        EXPECT_TRUE(game.party().addMember(0, companion));
        game.party().setPlayer(pc);
        area->add(pc);
        area->add(companion);
        area->add(enemy);
        // In play: the host applies the guest's hits only then.
        game.openInGame();
    }

    NetSession &net() { return game.netSession(); }

    StubConsole console;
    Game game;
    std::shared_ptr<Creature> pc;
    std::shared_ptr<Creature> companion;
    std::shared_ptr<Creature> enemy;
};

// One scene graph for both sides, which the engine's scene graphs give out,
// set up before either side makes its area and cameras.
struct SharedScene {
    explicit SharedScene(TestEngine &engine) :
        scene(std::make_unique<SceneGraph>("netsession", pipelineFactory, graphicsOptions,
                                           engine.services().graphics, engine.services().audio, engine.services().resource)) {
        ON_CALL(engine.sceneModule().graphs(), get(_)).WillByDefault(ReturnRef(*scene));
    }

    NiceMock<MockRenderPipelineFactory> pipelineFactory;
    GraphicsOptions graphicsOptions;
    std::unique_ptr<SceneGraph> scene;
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
    SharedScene scene {engine};
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

TEST_F(NetSessionTest, should_refuse_the_pc_as_the_guests_hero) {
    // the PC is the host's: both sides would drive it
    EXPECT_THROW(host.net().host(0, kNpcPlayer), std::runtime_error);
    EXPECT_THROW(guest.net().join("127.0.0.1", 7777, kNpcPlayer), std::runtime_error);
    EXPECT_EQ(NetRole::None, host.net().role());
}

TEST_F(NetSessionTest, should_keep_each_sides_leader_while_the_session_runs) {
    // given
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);

    // when: Tab, a portrait or a script would change the leader
    host.game.party().switchLeader();
    host.game.party().setPartyLeaderByIndex(1);
    guest.game.party().setPartyLeader(kNpcPlayer);

    // then: each side keeps its hero
    EXPECT_EQ(host.pc, host.game.party().getLeader());
    EXPECT_EQ(guest.companion, guest.game.party().getLeader());

    // when: the session ends
    guest.net().stop("the test is over");
    host.net().stop("the test is over");

    // then: the guest's PC leads again, and the leader can change
    EXPECT_EQ(guest.pc, guest.game.party().getLeader());
    host.game.party().setPartyLeaderByIndex(1);
    EXPECT_EQ(host.companion, host.game.party().getLeader());
}

TEST_F(NetSessionTest, should_refuse_the_guests_saves_and_clicks_on_the_world) {
    // given
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);
    exchange(60);

    // when: the guest saves, and clicks the enemy to attack it
    auto save = guest.game.requestSave(SaveRequest {});
    guest.game.module()->onObjectClick(guest.enemy);

    // then: neither happens on the guest
    EXPECT_EQ(SaveStatus::NotAllowed, save.status);
    EXPECT_TRUE(guest.companion->actions().empty());

    // and the host's click is its own, as ever
    host.game.module()->onObjectClick(host.enemy);
    EXPECT_FALSE(host.pc->actions().empty());
}

TEST_F(NetSessionTest, should_count_creatures_the_guest_lacks_and_compare_the_objects_again) {
    // given: joined, the objects matching
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);
    exchange(60);
    ASSERT_EQ(std::optional<bool>(true), guest.net().manifestsMatch());

    // when: a script spawns a creature on the host
    auto spawned = host.game.newCreature();
    spawned->setTag("end_sith4");
    host.game.module()->area()->add(spawned);
    exchange(150);

    // then: the guest counts its states, and both sides see the objects differ
    EXPECT_GE(guest.net().counts().unknown, 1);
    EXPECT_EQ(std::optional<bool>(false), guest.net().manifestsMatch());
    EXPECT_EQ(std::optional<bool>(false), host.net().manifestsMatch());
}

TEST_F(NetSessionTest, should_bring_back_a_creature_the_host_has_alive_again) {
    // given: the enemy dead on both sides
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);
    exchange(60);
    host.enemy->setCurrentHitPoints(0);
    exchange(12);
    ASSERT_TRUE(guest.enemy->isDead());

    // when: the host has it alive again
    host.enemy->setCurrentHitPoints(10);
    exchange(12);

    // then
    EXPECT_FALSE(guest.enemy->isDead());
    EXPECT_EQ(10, guest.enemy->currentHitPoints());
}

TEST_F(NetSessionTest, should_run_from_each_games_own_loop) {
    // given
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);

    // when: the games run, each updating its own side of the session
    host.pc->setPosition(glm::vec3(2.0f, 3.0f, 0.0f));
    for (int i = 0; i < 60; ++i) {
        clock.time += 1.0 / 60.0;
        host.game.update(1.0f / 60.0f);
        guest.game.update(1.0f / 60.0f);
    }

    // then
    EXPECT_TRUE(host.net().connected());
    EXPECT_TRUE(guest.net().connected());
    EXPECT_NEAR(2.0f, guest.pc->position().x, 1e-5f);
    EXPECT_NEAR(3.0f, guest.pc->position().y, 1e-5f);
}

TEST_F(NetSessionTest, should_have_the_hosts_pc_lead_whichever_member_the_save_had_leading) {
    // given: a save with the companion leading on the host
    host.game.party().setPartyLeader(0);
    ASSERT_EQ(host.companion, host.game.party().getLeader());

    // when
    host.net().host(0, 0);

    // then: the host plays the PC, and the guest the companion
    EXPECT_EQ(host.pc, host.game.party().getLeader());
}

TEST_F(NetSessionTest, should_keep_its_guest_when_a_stray_hello_comes_from_elsewhere) {
    // given: joined
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);
    exchange(60);
    ASSERT_TRUE(host.net().connected());

    // when: hellos from another socket, one too short, and a whole one of a
    // session of its own, begun later, whose guest plays another member
    NetSocket stray;
    stray.open(0);
    auto to = NetSocket::resolve("127.0.0.1", hostPort);
    ASSERT_TRUE(to);
    auto put = [](std::vector<uint8_t> &out, uint64_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            out.push_back(static_cast<uint8_t>(value >> (8 * i)));
        }
    };
    std::vector<uint8_t> shortHello {'R', 'N', 'E', 'T'};
    put(shortHello, NetSession::kProtocol, 2);
    put(shortHello, static_cast<uint8_t>(NetSession::MessageType::Hello), 1);
    put(shortHello, 1, 4); // sequence
    auto session = host.net().peerSession();
    int rejected = host.net().counts().rejected;
    stray.send(*to, shortHello);
    std::vector<uint8_t> otherMember(shortHello);
    put(otherMember, 0x5eed, 4);                  // its session
    put(otherMember, static_cast<uint8_t>(NetRole::Guest), 1);
    put(otherMember, 5, 4);                       // party member 5
    put(otherMember, 0, 8);                       // objects' hash
    put(otherMember, 0, 4);                       // objects
    put(otherMember, 0xffffffff00000000ull, 8);   // begun after any other
    stray.send(*to, otherMember);
    guest.companion->setPosition(glm::vec3(-4.0f, 2.0f, 0.0f));
    exchange(12);

    // then: the host refused both, still has its guest, and shows its hero where it went
    EXPECT_TRUE(host.net().connected());
    EXPECT_EQ(session, host.net().peerSession());
    EXPECT_EQ(rejected + 2, host.net().counts().rejected);
    EXPECT_NEAR(-4.0f, host.companion->position().x, 1e-5f);
}

TEST_F(NetSessionTest, should_end_the_session_when_a_save_loads_or_the_main_menu_opens) {
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);

    guest.game.openMainMenu();

    EXPECT_EQ(NetRole::None, guest.net().role());
    EXPECT_FALSE(guest.game.followingHost());
    EXPECT_FALSE(guest.game.party().leaderLocked());
}

TEST_F(NetSessionTest, should_pause_the_guest_while_the_host_pauses_in_a_fight) {
    // given: joined, the host's party in a fight
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);
    exchange(60);
    host.pc->activateCombat();

    // when: the host pauses
    host.game.setPaused(true);
    exchange(12);

    // then: so does the guest, and its own pause key doesn't let it go
    EXPECT_TRUE(guest.net().hostPausedInFight());
    EXPECT_TRUE(guest.game.isPaused());
    guest.game.setPaused(false);
    exchange(12);
    EXPECT_TRUE(guest.game.isPaused());

    // when: the host plays on
    host.game.setPaused(false);
    exchange(12);

    // then: so does the guest
    EXPECT_FALSE(guest.net().hostPausedInFight());
    EXPECT_FALSE(guest.game.isPaused());
}

TEST_F(NetSessionTest, should_leave_the_guest_playing_when_the_host_pauses_out_of_combat) {
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);
    exchange(60);

    host.game.setPaused(true);
    exchange(12);

    EXPECT_FALSE(guest.net().hostPausedInFight());
    EXPECT_FALSE(guest.game.isPaused());
}

TEST_F(NetSessionTest, should_keep_the_guests_own_pause_when_the_host_plays_on) {
    // given: the guest paused on its own, then the host in a fight
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);
    exchange(60);
    guest.game.setPaused(true);
    host.pc->activateCombat();
    host.game.setPaused(true);
    exchange(12);

    // when
    host.game.setPaused(false);
    exchange(12);

    // then: the guest's own pause stays
    EXPECT_TRUE(guest.game.isPaused());
}

TEST_F(NetSessionTest, should_pause_the_host_while_the_guest_pauses_in_a_fight) {
    // given: joined, the host's party in a fight
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);
    exchange(60);
    host.pc->activateCombat();

    // when: the guest pauses
    guest.game.setPaused(true);
    exchange(12);

    // then: so does the host, and its own pause key doesn't let it go
    EXPECT_TRUE(host.net().guestPausedInFight());
    EXPECT_TRUE(host.game.isPaused());
    host.game.setPaused(false);
    exchange(12);
    EXPECT_TRUE(host.game.isPaused());
    // and: the guest isn't held in turn, only paused by its own player
    EXPECT_FALSE(guest.net().hostPausedInFight());

    // when: the guest plays on
    guest.game.setPaused(false);
    exchange(12);

    // then: so do both
    EXPECT_FALSE(host.net().guestPausedInFight());
    EXPECT_FALSE(host.game.isPaused());
    EXPECT_FALSE(guest.game.isPaused());
}

TEST_F(NetSessionTest, should_leave_the_host_playing_when_the_guest_pauses_out_of_combat) {
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);
    exchange(60);

    guest.game.setPaused(true);
    exchange(12);

    EXPECT_FALSE(host.net().guestPausedInFight());
    EXPECT_FALSE(host.game.isPaused());
    EXPECT_TRUE(guest.game.isPaused());
}

TEST_F(NetSessionTest, should_let_both_play_on_once_both_pauses_in_a_fight_end) {
    // given: in a fight, the host paused, then the guest too
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);
    exchange(60);
    host.pc->activateCombat();
    host.game.setPaused(true);
    exchange(12);
    guest.game.setPaused(true);
    exchange(12);

    // when: the host plays on
    host.game.setPaused(false);
    exchange(12);

    // then: both still wait, for the guest
    EXPECT_TRUE(host.game.isPaused());
    EXPECT_TRUE(guest.game.isPaused());

    // when: the guest plays on too
    guest.game.setPaused(false);
    exchange(12);

    // then: both play
    EXPECT_FALSE(host.game.isPaused());
    EXPECT_FALSE(guest.game.isPaused());
}

TEST_F(NetSessionTest, should_let_the_host_go_when_a_guest_that_paused_goes_quiet) {
    // given: the guest paused in a fight, holding the host
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);
    exchange(60);
    host.pc->activateCombat();
    guest.game.setPaused(true);
    exchange(12);
    ASSERT_TRUE(host.game.isPaused());

    // when: the guest's window closes, and 5 s pass
    guest.net().stop("the test closes the guest");
    for (int i = 0; i < 360; ++i) {
        clock.time += 1.0 / 60.0;
        host.net().update();
    }

    // then: the host plays on
    EXPECT_FALSE(host.net().guestPausedInFight());
    EXPECT_FALSE(host.game.isPaused());
}

TEST_F(NetSessionTest, should_keep_a_pause_its_player_took_while_held) {
    // given: in a fight, the guest paused, holding the host
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);
    exchange(60);
    host.pc->activateCombat();
    guest.game.setPaused(true);
    exchange(12);
    ASSERT_TRUE(host.game.isPaused());

    // when: the host's player presses the pause key too, then the guest plays on
    auto space = input::Event::newKeyDown(input::KeyEvent(true, input::KeyCode::Space, 0, false));
    host.game.module()->handle(space);
    exchange(12);
    guest.game.setPaused(false);
    exchange(12);

    // then: the host stays paused by its own player, and holds the guest
    EXPECT_TRUE(host.game.isPausedByPlayer());
    EXPECT_TRUE(host.game.isPaused());
    EXPECT_TRUE(guest.game.isPaused());

    // when: the host's player presses it again
    host.game.module()->handle(space);
    exchange(12);

    // then: both play
    EXPECT_FALSE(host.game.isPaused());
    EXPECT_FALSE(guest.game.isPaused());
}

TEST_F(NetSessionTest, should_hold_the_guests_saber_hits_while_the_host_is_paused) {
    host.net().host(0, 0);
    hostPort = host.net().localPort();
    guest.net().join("127.0.0.1", hostPort, 0);
    exchange(60);

    host.game.setPaused(true);
    EXPECT_TRUE(guest.net().forwardSaberHit(*guest.enemy, JaBodyRegion::Torso));
    exchange(12);

    EXPECT_EQ(0, host.net().counts().hitsApplied);
    EXPECT_LE(1, host.net().counts().held);
    EXPECT_FALSE(host.enemy->isDead());
}

// A hero of the guest's own

namespace {

// A soldier class, outside the game's data, as the snapshot tests make one.
struct HeroClasses {
    NiceMock<resource::MockStrings> strings;
    NiceMock<resource::MockTwoDAs> twoDas;
    Classes classes {strings, twoDas};
    std::shared_ptr<CreatureClass> soldier {std::make_shared<CreatureClass>(ClassType::Soldier, classes, strings, twoDas)};

    Character hero(const std::string &name) {
        Character character;
        character.name = name;
        character.gender = Gender::Female;
        character.appearance = 91;
        character.attributes.addClassLevels(soldier.get(), 2);
        int scores[] {16, 14, 14, 10, 12, 8};
        for (int i = 0; i < 6; ++i) {
            character.attributes.setAbilityScore(static_cast<Ability>(i), scores[i]);
        }
        character.attributes.setSkillRank(SkillType::Awareness, 2);
        character.attributes.setSkillRank(SkillType::Security, 4);
        character.attributes.addFeat(FeatType::JediDefense);
        character.attributes.addFeat(FeatType::PowerAttack);
        return character;
    }
};

} // namespace

TEST(NetHero, should_travel_as_text_and_come_back_the_same) {
    HeroClasses classes;
    NiceMock<MockClasses> lookup;
    ON_CALL(lookup, get(ClassType::Soldier)).WillByDefault(Return(classes.soldier));
    auto hero = classes.hero("Shan Varra");

    auto text = writeNetHero(hero);
    auto back = readNetHero(text, lookup);

    EXPECT_EQ(0u, text.rfind("reone-hero 1\n", 0));
    EXPECT_EQ("Shan Varra", back.name);
    EXPECT_EQ(Gender::Female, back.gender);
    EXPECT_EQ(91, back.appearance);
    EXPECT_EQ(2, back.attributes.getClassLevel(ClassType::Soldier));
    EXPECT_EQ(16, back.attributes.strength());
    EXPECT_EQ(8, back.attributes.charisma());
    EXPECT_EQ(4, back.attributes.getSkillRank(SkillType::Security));
    EXPECT_TRUE(back.attributes.hasFeat(FeatType::JediDefense));
    EXPECT_TRUE(back.attributes.hasFeat(FeatType::PowerAttack));
    EXPECT_EQ(writeNetHero(hero), writeNetHero(back));
}

TEST_F(NetSessionTest, should_make_a_hero_only_from_the_main_menu) {
    // a game is loaded: character creation would drop it
    EXPECT_THROW(host.game.makeSessionHero("hero.txt"), std::runtime_error);
    EXPECT_FALSE(host.game.sessionHeroFile());
    // and a command file isn't held for it
    EXPECT_FALSE(host.game.holdsScriptedCommands());
}

TEST(NetHero, should_keep_a_name_of_64_bytes_on_both_sides) {
    HeroClasses classes;
    NiceMock<MockClasses> lookup;
    ON_CALL(lookup, get(ClassType::Soldier)).WillByDefault(Return(classes.soldier));
    auto hero = classes.hero("  " + std::string(100, 'a') + "  ");

    auto text = writeNetHero(hero);
    auto back = readNetHero(text, lookup);

    EXPECT_NE(std::string::npos, text.find("name=" + std::string(64, 'a') + "\n"));
    EXPECT_EQ(std::string(64, 'a'), back.name);
    EXPECT_EQ(text, writeNetHero(back));
}

TEST(NetHero, should_refuse_what_isnt_a_hero) {
    NiceMock<MockClasses> lookup;
    EXPECT_THROW(readNetHero("name=Bastila\n", lookup), std::runtime_error);
    EXPECT_THROW(readNetHero("reone-hero 1\nname=Bastila\n", lookup), std::runtime_error);
    EXPECT_THROW(readNetHero("reone-hero 1\nname=Bastila\nclass=0:1\n", lookup), std::runtime_error); // no such class here
    EXPECT_THROW(readNetHero("reone-hero 1\nname=Bastila\ngender=droid\n", lookup), std::runtime_error);
}

TEST_F(NetSessionTest, should_bring_the_guests_own_hero_into_both_games_and_take_it_away_after) {
    // given: a hero of the guest's own, and a host expecting one
    HeroClasses classes;
    EXPECT_CALL(engine.gameModule().classes(), get(_)).WillRepeatedly(Return(classes.soldier));
    host.net().host(0, kNpcSessionHero);
    hostPort = host.net().localPort();
    EXPECT_THROW(guest.net().join("127.0.0.1", hostPort, kNpcSessionHero), std::runtime_error);
    guest.net().setHero(classes.hero("Shan Varra"));

    // when
    guest.net().join("127.0.0.1", hostPort, kNpcSessionHero);
    exchange(60);

    // then: both games have the hero, at one ID, out of saves and of the object check
    uint32_t id = host.net().heroId();
    ASSERT_NE(0u, id);
    EXPECT_EQ(id, guest.net().heroId());
    auto onHost = host.game.getObjectById<Creature>(id);
    auto onGuest = guest.game.getObjectById<Creature>(id);
    ASSERT_TRUE(onHost);
    ASSERT_TRUE(onGuest);
    EXPECT_EQ("Shan Varra", onHost->name());
    EXPECT_EQ(2, onGuest->attributes().getClassLevel(ClassType::Soldier));
    EXPECT_TRUE(host.game.isSessionOnly(id));
    EXPECT_TRUE(guest.game.isSessionOnly(id));
    EXPECT_EQ(std::optional<bool>(true), host.net().manifestsMatch());
    EXPECT_EQ(std::optional<bool>(true), guest.net().manifestsMatch());
    // beside the host's PC, a metre off
    EXPECT_NEAR(1.0f, glm::distance(onHost->position(), host.pc->position()), 1e-4f);
    // the guest plays it; on the host it is no party member
    EXPECT_EQ(onGuest, guest.game.party().getLeader());
    EXPECT_FALSE(host.game.party().isMember(*onHost));
    EXPECT_EQ(host.pc, host.game.party().getLeader());

    // when: the guest moves its hero
    onGuest->setPosition(glm::vec3(-3.0f, 2.0f, 0.0f));
    exchange(12);

    // then: the host shows it there, and the guest's stays its own
    EXPECT_NEAR(-3.0f, onHost->position().x, 1e-5f);
    EXPECT_NEAR(-3.0f, onGuest->position().x, 1e-5f);

    // when: the session ends
    guest.net().stop("the test is over");
    host.net().stop("the test is over");

    // then: the hero goes with it, and the guest's PC leads again
    EXPECT_EQ(0u, guest.net().heroId());
    EXPECT_EQ(0u, host.net().heroId());
    // out of saves and the object check until the area has destroyed it, and
    // after: no object takes its ID again before a load
    EXPECT_TRUE(host.game.isSessionOnly(id));
    EXPECT_EQ(guest.pc, guest.game.party().getLeader());
    EXPECT_FALSE(guest.game.party().isMember(*onGuest));
    // the guest's hero stays set, for the next session
    ASSERT_TRUE(guest.net().hero());
    EXPECT_EQ("Shan Varra", guest.net().hero()->name);
}

TEST_F(NetSessionTest, should_let_the_guest_join_again_with_another_hero) {
    // given: the guest has played a hero, and left
    HeroClasses classes;
    EXPECT_CALL(engine.gameModule().classes(), get(_)).WillRepeatedly(Return(classes.soldier));
    host.net().host(0, kNpcSessionHero);
    hostPort = host.net().localPort();
    guest.net().setHero(classes.hero("Shan Varra"));
    guest.net().join("127.0.0.1", hostPort, kNpcSessionHero);
    exchange(60);
    uint32_t first = host.net().heroId();
    ASSERT_NE(0u, first);
    ASSERT_EQ(first, guest.net().heroId());
    guest.net().stop("the test leaves");

    // when: it joins again, with another hero
    guest.net().setHero(classes.hero("Kel Dorn"));
    guest.net().join("127.0.0.1", hostPort, kNpcSessionHero);
    exchange(60);

    // then: both sides have the new hero, at a new ID, and it plays
    uint32_t second = host.net().heroId();
    ASSERT_NE(0u, second);
    EXPECT_NE(first, second);
    EXPECT_EQ(second, guest.net().heroId());
    auto onHost = host.game.getObjectById<Creature>(second);
    auto onGuest = guest.game.getObjectById<Creature>(second);
    ASSERT_TRUE(onHost);
    ASSERT_TRUE(onGuest);
    EXPECT_EQ("Kel Dorn", onHost->name());
    EXPECT_EQ(onGuest, guest.game.party().getLeader());
    onGuest->setPosition(glm::vec3(-3.0f, 2.0f, 0.0f));
    exchange(12);
    EXPECT_NEAR(-3.0f, onHost->position().x, 1e-5f);
    // and: the first is on its way out on the host, its ID out of the object check
    EXPECT_TRUE(host.game.isSessionOnly(first));
    EXPECT_EQ(std::optional<bool>(true), host.net().manifestsMatch());
}

TEST_F(NetSessionTest, should_bring_the_hero_again_when_the_host_starts_over) {
    // given: a guest playing its hero
    HeroClasses classes;
    EXPECT_CALL(engine.gameModule().classes(), get(_)).WillRepeatedly(Return(classes.soldier));
    host.net().host(0, kNpcSessionHero);
    hostPort = host.net().localPort();
    guest.net().setHero(classes.hero("Shan Varra"));
    guest.net().join("127.0.0.1", hostPort, kNpcSessionHero);
    exchange(60);
    uint32_t first = guest.net().heroId();
    ASSERT_NE(0u, first);

    // when: the host ends its session and hosts again, on the same port,
    // while the guest stays; the guest says hello every 2 s
    host.net().stop("the test starts over");
    host.net().host(hostPort, kNpcSessionHero);
    exchange(200);

    // then: the hero is made again, at a new ID, and the guest plays it
    uint32_t second = host.net().heroId();
    ASSERT_NE(0u, second);
    EXPECT_NE(first, second);
    EXPECT_EQ(second, guest.net().heroId());
    auto onHost = host.game.getObjectById<Creature>(second);
    auto onGuest = guest.game.getObjectById<Creature>(second);
    ASSERT_TRUE(onHost);
    ASSERT_TRUE(onGuest);
    EXPECT_EQ(onGuest, guest.game.party().getLeader());
    onGuest->setPosition(glm::vec3(-3.0f, 2.0f, 0.0f));
    exchange(12);
    EXPECT_NEAR(-3.0f, onHost->position().x, 1e-5f);
}

TEST_F(NetSessionTest, should_hold_a_command_file_until_the_guests_hero_is_here) {
    // given
    HeroClasses classes;
    EXPECT_CALL(engine.gameModule().classes(), get(_)).WillRepeatedly(Return(classes.soldier));
    EXPECT_THROW(guest.net().waitForPeer(60.0f), std::runtime_error); // no session
    host.net().host(0, kNpcSessionHero);
    hostPort = host.net().localPort();
    guest.net().setHero(classes.hero("Shan Varra"));
    guest.net().join("127.0.0.1", hostPort, kNpcSessionHero);

    // when
    guest.net().waitForPeer(60.0f);
    host.net().waitForPeer(60.0f);

    // then: both hold until the hero is here, the guest's leading
    EXPECT_TRUE(guest.game.holdsScriptedCommands());
    EXPECT_TRUE(host.game.holdsScriptedCommands());
    for (int frame = 0; frame < 120 && guest.game.holdsScriptedCommands(); ++frame) {
        exchange(1);
        if (guest.game.holdsScriptedCommands() && guest.net().heroId()) {
            // made, but not yet leading: still holding
            EXPECT_NE(guest.game.party().getLeader(), guest.game.getObjectById<Creature>(guest.net().heroId()));
        }
    }
    EXPECT_FALSE(guest.game.holdsScriptedCommands());
    ASSERT_NE(0u, guest.net().heroId());
    EXPECT_EQ(guest.game.getObjectById<Creature>(guest.net().heroId()), guest.game.party().getLeader());
    exchange(6);
    EXPECT_FALSE(host.game.holdsScriptedCommands());
}

TEST_F(NetSessionTest, should_stop_holding_a_command_file_after_its_time) {
    // given: a host no guest joins
    host.net().host(0, kNpcSessionHero);

    // when
    host.net().waitForPeer(1.0f);
    exchange(30);

    // then: half the time, still holding; after it, going on
    EXPECT_TRUE(host.game.holdsScriptedCommands());
    exchange(40);
    EXPECT_FALSE(host.game.holdsScriptedCommands());
}

namespace {

// Stands between the guest and the host, so that the host hears the guest at
// one address whichever socket the guest uses, as behind a router. It keeps a
// copy of everything it carries, and can hold back either way, to deliver a
// message late.
struct Relay {
    using Bytes = std::vector<uint8_t>;

    NetSocket guestSide;
    NetSocket hostSide;
    NetSocket::Address host;
    std::optional<NetSocket::Address> guest;
    bool holdToHost {false};
    bool holdToGuest {false};
    std::function<void(Bytes &)> rewriteToHost; // changes what it carries to the host
    std::vector<Bytes> toHost;      // everything from the guest, carried or held
    std::vector<Bytes> toGuest;     // everything from the host, carried or held
    std::vector<Bytes> heldToHost;
    std::vector<Bytes> heldToGuest;

    void open(uint16_t hostPort) {
        guestSide.open(0);
        hostSide.open(0);
        host = *NetSocket::resolve("127.0.0.1", hostPort);
    }

    static NetSession::MessageType type(const Bytes &bytes) {
        return static_cast<NetSession::MessageType>(bytes.size() > 6 ? bytes[6] : 0);
    }

    static Bytes first(const std::vector<Bytes> &list, NetSession::MessageType wanted) {
        for (auto &bytes : list) {
            if (type(bytes) == wanted) {
                return bytes;
            }
        }
        ADD_FAILURE() << "no message of type " << static_cast<int>(wanted);
        return Bytes();
    }

    static Bytes last(const std::vector<Bytes> &list, NetSession::MessageType wanted) {
        for (auto it = list.rbegin(); it != list.rend(); ++it) {
            if (type(*it) == wanted) {
                return *it;
            }
        }
        ADD_FAILURE() << "no message of type " << static_cast<int>(wanted);
        return Bytes();
    }

    void pump() {
        while (auto datagram = guestSide.receive()) {
            guest = datagram->from;
            if (rewriteToHost) {
                rewriteToHost(datagram->bytes);
            }
            toHost.push_back(datagram->bytes);
            if (holdToHost) {
                heldToHost.push_back(datagram->bytes);
            } else {
                hostSide.send(host, datagram->bytes);
            }
        }
        while (auto datagram = hostSide.receive()) {
            toGuest.push_back(datagram->bytes);
            if (holdToGuest || !guest) {
                heldToGuest.push_back(datagram->bytes);
            } else {
                guestSide.send(*guest, datagram->bytes);
            }
        }
    }

    // Delivers a message now, late.
    void late(const Bytes &bytes, bool toTheHost) {
        if (toTheHost) {
            hostSide.send(host, bytes);
        } else {
            guestSide.send(*guest, bytes);
        }
    }
};

class NetSessionRelayTest : public NetSessionTest {
protected:
    void SetUp() override {
        NetSessionTest::SetUp();
        EXPECT_CALL(engine.gameModule().classes(), get(_)).WillRepeatedly(Return(classes.soldier));
    }

    void relayed(int frames) {
        for (int i = 0; i < frames; ++i) {
            clock.time += 1.0 / 60.0;
            host.net().update();
            relay.pump();
            guest.net().update();
            relay.pump();
        }
    }

    void hostOnRelay() {
        host.net().host(hostPort, kNpcSessionHero);
        hostPort = host.net().localPort();
    }

    void joinByRelay(const std::string &name) {
        guest.net().setHero(classes.hero(name));
        guest.net().join("127.0.0.1", relay.guestSide.localPort(), kNpcSessionHero);
    }

    // Both sides have the hero at one ID, it leads on the guest, and the host
    // shows it where the guest moves it.
    void expectOneHeroPlaying(uint32_t id) {
        ASSERT_NE(0u, id);
        EXPECT_EQ(NetRole::Guest, guest.net().role());
        EXPECT_EQ(id, host.net().heroId());
        EXPECT_EQ(id, guest.net().heroId());
        auto onHost = host.game.getObjectById<Creature>(id);
        auto onGuest = guest.game.getObjectById<Creature>(id);
        ASSERT_TRUE(onHost);
        ASSERT_TRUE(onGuest);
        EXPECT_EQ(onGuest, guest.game.party().getLeader());
        float x = onGuest->position().x - 2.0f;
        onGuest->setPosition(glm::vec3(x, 2.0f, 0.0f));
        relayed(12);
        EXPECT_NEAR(x, onHost->position().x, 1e-5f);
    }

    HeroClasses classes;
    Relay relay;
};

} // namespace

TEST_F(NetSessionRelayTest, should_let_a_guest_join_again_from_the_same_address_and_drop_its_earlier_hero_spec) {
    // given: a guest that played one hero, left, and joined again with
    // another, both times by the relay
    hostOnRelay();
    relay.open(hostPort);
    joinByRelay("Shan Varra");
    relayed(60);
    uint32_t first = host.net().heroId();
    ASSERT_NE(0u, first);
    auto earlierSpec = Relay::first(relay.toHost, NetSession::MessageType::HeroSpec);
    guest.net().stop("the test leaves");
    joinByRelay("Kel Dorn");
    relayed(60);
    uint32_t second = host.net().heroId();
    ASSERT_NE(first, second);

    // when: the first session's spec comes again, late
    relay.late(earlierSpec, true);
    relayed(30);

    // then: the host keeps the hero the guest plays
    expectOneHeroPlaying(second);
    EXPECT_EQ("Kel Dorn", host.game.getObjectById<Creature>(second)->name());
}

TEST_F(NetSessionRelayTest, should_keep_the_guests_hero_when_a_hello_from_the_hosts_earlier_session_comes_late) {
    // given: a host that started over while the guest stayed, and the hero
    // made again
    hostOnRelay();
    relay.open(hostPort);
    joinByRelay("Shan Varra");
    relayed(60);
    auto earlierHello = Relay::first(relay.toGuest, NetSession::MessageType::Hello);
    host.net().stop("the test starts over");
    hostOnRelay();
    relayed(200);
    uint32_t hero = host.net().heroId();
    ASSERT_NE(0u, hero);

    // when: a hello from the host's first session comes, late
    auto current = guest.net().peerSession();
    relay.late(earlierHello, false);
    relayed(2);
    EXPECT_EQ(current, guest.net().peerSession());
    relayed(300);

    // then: the guest still plays the same hero, in the same session
    expectOneHeroPlaying(hero);
    EXPECT_LE(1, guest.net().counts().stale);
}

TEST_F(NetSessionRelayTest, should_keep_the_guests_hero_when_a_hello_comes_late_from_a_host_session_it_never_heard) {
    // given: a host that started over twice, the guest hearing nothing of
    // the middle session, and the hero made again in the third
    hostOnRelay();
    relay.open(hostPort);
    joinByRelay("Shan Varra");
    relayed(60);
    host.net().stop("the test starts over");
    relay.holdToGuest = true;
    hostOnRelay();
    relayed(150);
    host.net().stop("the test starts over again");
    relay.holdToGuest = false;
    hostOnRelay();
    relayed(200);
    uint32_t hero = host.net().heroId();
    ASSERT_NE(0u, hero);

    // when: a hello from the middle session comes, late
    auto current = guest.net().peerSession();
    relay.late(Relay::first(relay.heldToGuest, NetSession::MessageType::Hello), false);
    relayed(2);
    EXPECT_EQ(current, guest.net().peerSession());
    relayed(300);

    // then: it began before the third, so the guest keeps its hero
    expectOneHeroPlaying(hero);
}

TEST_F(NetSessionRelayTest, should_keep_the_guests_hero_when_a_hello_comes_late_from_a_guest_session_the_host_never_heard) {
    // given: a guest that joined three times by the relay, the host hearing
    // nothing of the second, and the third's hero made
    hostOnRelay();
    relay.open(hostPort);
    joinByRelay("Shan Varra");
    relayed(60);
    guest.net().stop("the test leaves");
    relay.holdToHost = true;
    joinByRelay("Kel Dorn");
    relayed(60);
    guest.net().stop("the test leaves again");
    relay.holdToHost = false;
    joinByRelay("Bastila");
    relayed(60);
    uint32_t hero = host.net().heroId();
    ASSERT_NE(0u, hero);

    // when: a hello from the second session comes, late
    auto current = host.net().peerSession();
    relay.late(Relay::first(relay.heldToHost, NetSession::MessageType::Hello), true);
    relayed(2);
    EXPECT_EQ(current, host.net().peerSession());
    relayed(60);

    // then: the host keeps the third's hero
    expectOneHeroPlaying(hero);
    EXPECT_EQ("Bastila", host.game.getObjectById<Creature>(hero)->name());
}

TEST_F(NetSessionRelayTest, should_take_a_session_begun_earlier_by_the_clock_once_the_last_is_quiet) {
    // given: a guest that played, left, and joined again with its clock
    // behind, as after an engine restart: its hellos say it began at 1 ms
    hostOnRelay();
    relay.open(hostPort);
    joinByRelay("Shan Varra");
    relayed(60);
    auto first = host.net().peerSession();
    ASSERT_TRUE(first);
    guest.net().stop("the test restarts");
    relay.rewriteToHost = [](Relay::Bytes &bytes) {
        constexpr size_t kStartAt = 15 + 1 + 4 + 8 + 4;
        if (Relay::type(bytes) == NetSession::MessageType::Hello && bytes.size() >= kStartAt + 8) {
            std::fill(bytes.begin() + kStartAt, bytes.begin() + kStartAt + 8, 0);
            bytes[kStartAt] = 1;
        }
    };
    joinByRelay("Kel Dorn");

    // when: the first session is still heard of, within 5 s
    relayed(60);

    // then: the host keeps it
    EXPECT_EQ(first, host.net().peerSession());

    // when: it has been quiet for 5 s
    relayed(360);

    // then: the host takes the new session, and the new hero plays
    EXPECT_NE(first, host.net().peerSession());
    uint32_t hero = host.net().heroId();
    expectOneHeroPlaying(hero);
    EXPECT_EQ("Kel Dorn", host.game.getObjectById<Creature>(hero)->name());
}

TEST_F(NetSessionRelayTest, should_drop_a_late_guest_state_from_the_guests_earlier_session) {
    // given: a guest that played five seconds, then joined again
    hostOnRelay();
    relay.open(hostPort);
    joinByRelay("Shan Varra");
    relayed(300);
    auto earlierState = Relay::last(relay.toHost, NetSession::MessageType::GuestState);
    guest.net().stop("the test leaves");
    joinByRelay("Shan Varra");
    relayed(60);
    uint32_t hero = host.net().heroId();

    // when: the first session's last state comes, late, counted higher than
    // any of the second's
    relay.late(earlierState, true);
    relayed(2);

    // then: the host goes on following the guest at once
    expectOneHeroPlaying(hero);
}

TEST_F(NetSessionRelayTest, should_drop_a_late_snapshot_from_the_hosts_earlier_session) {
    // given: a host that played five seconds, then started over
    hostOnRelay();
    relay.open(hostPort);
    joinByRelay("Shan Varra");
    relayed(300);
    auto earlierSnapshot = Relay::last(relay.toGuest, NetSession::MessageType::Snapshot);
    host.net().stop("the test starts over");
    hostOnRelay();
    relayed(200);
    ASSERT_NE(0u, guest.net().heroId());

    // when: the first session's last snapshot comes, late; then the host
    // moves an enemy
    relay.late(earlierSnapshot, false);
    relayed(2);
    host.enemy->setPosition(glm::vec3(7.0f, 7.0f, 0.0f));
    relayed(12);

    // then: the guest shows it where the host has it now
    EXPECT_NEAR(7.0f, guest.enemy->position().x, 1e-5f);
    EXPECT_LE(1, guest.net().counts().stale);
}

TEST_F(NetSessionTest, should_share_the_partys_experience_with_the_guests_hero) {
    // given: the guest's hero in play, level 2, and the host's party with
    // the experience for level 4
    HeroClasses classes;
    EXPECT_CALL(engine.gameModule().classes(), get(_)).WillRepeatedly(Return(classes.soldier));
    host.net().host(0, kNpcSessionHero);
    hostPort = host.net().localPort();
    guest.net().setHero(classes.hero("Shan Varra"));
    guest.net().join("127.0.0.1", hostPort, kNpcSessionHero);
    exchange(60);
    uint32_t id = host.net().heroId();
    ASSERT_NE(0u, id);

    // when: the host's party gains experience
    host.game.party().setXP(6000);
    exchange(12);

    // then: the hero has it on both sides, and a level-up waits on the guest's
    auto onHost = host.game.getObjectById<Creature>(id);
    auto onGuest = guest.game.getObjectById<Creature>(id);
    ASSERT_TRUE(onHost);
    ASSERT_TRUE(onGuest);
    EXPECT_EQ(6000, onGuest->xp());
    EXPECT_EQ(6000, onHost->xp());
    EXPECT_EQ(6000, guest.game.party().xp());
    EXPECT_TRUE(onGuest->isLevelUpPending());
}

TEST_F(NetSessionTest, should_take_the_guests_level_up_on_the_host_and_keep_it_in_the_hero_file) {
    // given: the guest's hero, from a file, in play at level 2
    HeroClasses classes;
    EXPECT_CALL(engine.gameModule().classes(), get(_)).WillRepeatedly(Return(classes.soldier));
    auto file = std::filesystem::temp_directory_path() / "reone-netsession-hero.txt";
    std::filesystem::remove(file);
    host.net().host(0, kNpcSessionHero);
    hostPort = host.net().localPort();
    guest.net().setHero(classes.hero("Shan Varra"), file.string());
    guest.net().join("127.0.0.1", hostPort, kNpcSessionHero);
    exchange(60);
    uint32_t id = host.net().heroId();
    auto onHost = host.game.getObjectById<Creature>(id);
    auto onGuest = guest.game.getObjectById<Creature>(id);
    ASSERT_TRUE(onHost);
    ASSERT_TRUE(onGuest);
    int baseBefore = onHost->hitPoints();

    // when: the guest levels its hero up, as CharacterGeneration::finish does
    onGuest->attributes().addClassLevels(classes.soldier.get(), 1);
    onGuest->setMaxHitPoints(onGuest->hitPoints() + classes.soldier->hitdie());
    exchange(60);

    // then: the host's hero is level 3 too (the test's class has no hit die
    // from the game's data, so its hit points can't show the new level's)
    EXPECT_EQ(3, onHost->attributes().getClassLevel(ClassType::Soldier));
    EXPECT_EQ(baseBefore + classes.soldier->hitdie(), onHost->hitPoints());
    EXPECT_EQ(1u, guest.net().heroVersion());
    EXPECT_EQ(1u, host.net().heroVersion());
    // and: the file keeps the level, for the next session
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    EXPECT_NE(std::string::npos, text.str().find("class=" + std::to_string(static_cast<int>(ClassType::Soldier)) + ":3"));
    // and: the guest brings the leveled hero next time
    EXPECT_EQ(3, guest.net().hero()->attributes.getClassLevel(ClassType::Soldier));
    std::filesystem::remove(file);
}

TEST_F(NetSessionTest, should_let_the_host_go_when_the_guest_that_held_it_joins_again) {
    // given: the guest's hero in a fight, the guest paused, holding the host
    HeroClasses classes;
    EXPECT_CALL(engine.gameModule().classes(), get(_)).WillRepeatedly(Return(classes.soldier));
    host.net().host(0, kNpcSessionHero);
    hostPort = host.net().localPort();
    guest.net().setHero(classes.hero("Shan Varra"));
    guest.net().join("127.0.0.1", hostPort, kNpcSessionHero);
    exchange(60);
    host.pc->activateCombat();
    guest.game.setPaused(true);
    exchange(12);
    ASSERT_TRUE(host.game.isPaused());

    // when: it joins again, playing, with a hero the host refuses, so that
    // it sends no state for a hero
    guest.net().stop("the test leaves");
    guest.game.setPaused(false);
    guest.net().setHero(classes.hero(""));
    guest.net().join("127.0.0.1", hostPort, kNpcSessionHero);
    exchange(60);

    // then: the earlier session's hold went with it
    EXPECT_FALSE(host.net().guestPausedInFight());
    EXPECT_FALSE(host.game.isPaused());
}

TEST(NetHero, should_write_a_hero_file_whole_in_place_of_the_old) {
    HeroClasses classes;
    NiceMock<MockClasses> lookup;
    ON_CALL(lookup, get(ClassType::Soldier)).WillByDefault(Return(classes.soldier));
    auto file = std::filesystem::temp_directory_path() / "reone-netsession-hero-file.txt";
    std::filesystem::remove(file);

    ASSERT_TRUE(writeNetHeroFile(file.string(), classes.hero("Shan Varra")));
    ASSERT_TRUE(writeNetHeroFile(file.string(), classes.hero("Kel Dorn")));

    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    EXPECT_EQ("Kel Dorn", readNetHero(text.str(), lookup).name);
    EXPECT_FALSE(std::filesystem::exists(file.string() + ".new"));
    in.close();
    std::filesystem::remove(file);
}

namespace {

// KOTOR's xptable.2da, made up: each cell "row's level * 100 + column", so a
// test can tell which row and column a kill took.
std::shared_ptr<resource::TwoDA> madeUpXPTable() {
    resource::TwoDA::Builder builder;
    std::vector<std::string> columns {"level"};
    for (int c = 0; c <= 20; ++c) {
        columns.push_back("c" + std::to_string(c));
    }
    builder.columns(columns);
    for (int level = 1; level <= 20; ++level) {
        std::vector<std::string> row {std::to_string(level)};
        for (int c = 0; c <= 20; ++c) {
            row.push_back(std::to_string(level * 100 + c));
        }
        builder.row(row);
    }
    return builder.build();
}

// The shared engine's 2DAs give the made-up table for one test, and nothing
// after it, however the test ends.
struct XPTableForThisTest {
    explicit XPTableForThisTest(resource::MockTwoDAs &twoDas) :
        twoDas(twoDas) {
        EXPECT_CALL(twoDas, get(_)).Times(AnyNumber());
        EXPECT_CALL(twoDas, get("xptable")).WillRepeatedly(Return(table));
    }
    ~XPTableForThisTest() {
        Mock::VerifyAndClearExpectations(&twoDas);
    }
    resource::MockTwoDAs &twoDas;
    std::shared_ptr<resource::TwoDA> table {madeUpXPTable()};
};

} // namespace

TEST_F(NetSessionTest, should_give_the_party_kotors_experience_for_a_kill_by_the_guests_hero) {
    // given: the guest's hero in play, and the party at 10,000 experience,
    // experience level 5
    HeroClasses classes;
    EXPECT_CALL(engine.gameModule().classes(), get(_)).WillRepeatedly(Return(classes.soldier));
    XPTableForThisTest xptable(engine.resourceModule().twoDas());
    host.game.party().setXP(10000);
    guest.game.party().setXP(10000);
    host.net().host(0, kNpcSessionHero);
    hostPort = host.net().localPort();
    guest.net().setHero(classes.hero("Shan Varra"));
    guest.net().join("127.0.0.1", hostPort, kNpcSessionHero);
    exchange(60);
    ASSERT_NE(0u, guest.net().heroId());

    // when: the guest's hero's saber kills the enemy, whose challenge rating is 0
    EXPECT_TRUE(guest.net().forwardSaberHit(*guest.enemy, JaBodyRegion::Torso));
    exchange(12);

    // then: the host's party gets row 5, column 0, once, and the guest's
    // hero has it from the host, not again from its own copy of the kill
    ASSERT_TRUE(host.enemy->isDead());
    EXPECT_EQ(10000 + 500, host.game.party().xp());
    EXPECT_EQ(10000 + 500, guest.game.party().xp());
    EXPECT_EQ(10000 + 500, guest.game.getObjectById<Creature>(guest.net().heroId())->xp());
}

TEST_F(NetSessionTest, should_give_no_experience_for_a_party_member_killed) {
    XPTableForThisTest xptable(engine.resourceModule().twoDas());

    host.companion->damage(1000, host.enemy);

    ASSERT_TRUE(host.companion->isDead());
    EXPECT_EQ(0, host.game.party().xp());
}

TEST_F(NetSessionTest, should_give_no_experience_for_an_enemy_an_enemy_killed) {
    XPTableForThisTest xptable(engine.resourceModule().twoDas());

    host.enemy->damage(1000, host.enemy);

    ASSERT_TRUE(host.enemy->isDead());
    EXPECT_EQ(0, host.game.party().xp());
}

TEST_F(NetSessionTest, should_give_the_party_experience_for_the_pcs_own_kill) {
    XPTableForThisTest xptable(engine.resourceModule().twoDas());

    host.enemy->damage(1000, host.pc);

    ASSERT_TRUE(host.enemy->isDead());
    EXPECT_EQ(100, host.game.party().xp()); // level 1, challenge rating 0
}

TEST_F(NetSessionTest, should_give_a_quick_heros_long_name_alike_to_both_sides) {
    // given: a name over 64 bytes, as nethero quick takes every word
    HeroClasses classes;
    EXPECT_CALL(engine.gameModule().classes(), get(_)).WillRepeatedly(Return(classes.soldier));
    host.net().host(0, kNpcSessionHero);
    hostPort = host.net().localPort();
    guest.net().setHero(classes.hero(" " + std::string(63, 'a') + " " + std::string(40, 'b')));

    // when
    guest.net().join("127.0.0.1", hostPort, kNpcSessionHero);
    exchange(60);

    // then: one name on both sides, cut at 64 bytes, with no space left at the cut
    ASSERT_NE(0u, host.net().heroId());
    auto onHost = host.game.getObjectById<Creature>(host.net().heroId());
    auto onGuest = guest.game.getObjectById<Creature>(guest.net().heroId());
    ASSERT_TRUE(onHost);
    ASSERT_TRUE(onGuest);
    EXPECT_EQ(std::string(63, 'a'), guest.net().hero()->name);
    EXPECT_EQ(onHost->name(), onGuest->name());
    EXPECT_EQ(std::string(63, 'a'), onHost->name());
}
