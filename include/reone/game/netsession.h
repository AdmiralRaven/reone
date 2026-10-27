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

#pragma once

#include <set>

#include "reone/game/character.h"
#include "reone/game/jacontact.h"

namespace reone {

namespace game {

class Creature;
class Game;
class IClasses;
class IConsole;
struct ServicesView;

/**
 * A second hero as a session's guest makes one and sends it (#39): KOTOR's
 * character creation's choices, as text, one "key=value" a line:
 *
 *     reone-hero 1
 *     name=Revan
 *     gender=female
 *     appearance=136
 *     class=3:1
 *     abilities=14,14,14,12,12,10
 *     skills=3:2,6:4
 *     feats=55,42
 *     powers=
 *
 * Classes, skills, feats and powers by KOTOR's numbers (classes.2da,
 * skills.2da, feat.2da, spells.2da); a class with its levels; the abilities
 * in KOTOR's order, strength to charisma.
 */
std::string writeNetHero(const Character &character);

/** \throws std::runtime_error when \p text isn't a hero, or names a class KOTOR doesn't have */
Character readNetHero(const std::string &text, IClasses &classes);

/**
 * A hero as character creation's quick path makes one: \p clazz's defaults
 * from classes.2da and the first PC portrait's look for \p gender and the
 * class, as ClassSelection picks among them.
 */
Character quickNetHero(ServicesView &services, ClassType clazz, Gender gender, std::string name);

/**
 * Makes \p character a creature at \p id, as CharacterGeneration::finish makes
 * the PC, but for its scripts; none if the ID is taken.
 */
std::shared_ptr<Creature> makeNetHero(Game &game, uint32_t id, const Character &character);

/** A UDP socket that never blocks: Winsock on Windows, BSD sockets elsewhere. */
class NetSocket : boost::noncopyable {
public:
    struct Address {
        uint32_t ip {0}; /**< IPv4, in host order */
        uint16_t port {0};

        bool operator==(const Address &other) const { return ip == other.ip && port == other.port; }
        bool operator!=(const Address &other) const { return !(*this == other); }
        std::string str() const;
    };

    struct Datagram {
        Address from;
        std::vector<uint8_t> bytes;
    };

    ~NetSocket();

    /**
     * Binds to \p port on every address; 0 picks a free one.
     *
     * \throws std::runtime_error when the socket can't be made or bound
     */
    void open(uint16_t port);
    void close();
    bool isOpen() const;
    uint16_t localPort() const;

    void send(const Address &to, const std::vector<uint8_t> &bytes);

    /** The next datagram waiting, or none. */
    std::optional<Datagram> receive();

    /** An IPv4 address, dotted or a host name such as localhost. */
    static std::optional<Address> resolve(const std::string &host, uint16_t port);

    /** Receives that failed other than for nothing waiting or a reset. */
    int errors() const { return _errors; }

private:
    intptr_t _socket {-1};
    std::vector<uint8_t> _buffer;
    int _errors {0};
};

/** Which side of a two-player session this process is. */
enum class NetRole {
    None,
    Host, /**< runs the game: scripts, combat, saves, every creature but the guest's hero */
    Guest /**< shows the host's game and drives its own hero in it */
};

/** One creature as the host sends it. */
struct NetCreatureState {
    uint32_t id {0};
    glm::vec3 position {0.0f};
    float facing {0.0f};
    uint8_t movement {0}; /**< Creature::MovementType */
    bool dead {false};
    int16_t hitPoints {0};
};

/**
 * Two reone processes sharing one game, the first step (#39's spike): both
 * load the same save, with the same random seed, so that their objects have
 * the same runtime IDs (the session checks that rather than assume it). Then:
 *
 * - the host runs the game as ever, and 20 times a second sends the guest
 *   every creature in its area: where it stands and faces, whether it walks,
 *   runs or stands, its hit points and whether it is dead;
 * - the guest makes the party member it plays, the guest's hero, its own
 *   party leader, so its keyboard, mouse and saber mode drive it; it shows
 *   every other creature where the host has it, and sends the host where
 *   its hero is, which the host shows in turn;
 * - a hit of the guest's saber goes to the host as a request, which the host
 *   applies with its own rules (jaSaberHitExtraDamage), so that hit points
 *   and deaths come from the host alone;
 * - the guest runs no scripts, combat rounds, saves or area changes of its
 *   own, and its creatures do nothing but what the host sends;
 * - the host pausing while its party is in combat pauses the guest too, as
 *   the user asked (2026-10-26); out of combat each side pauses alone.
 *
 * Messages are little-endian, each starting with "RNET", the protocol
 * version and a type. Nothing is retried: a newer snapshot replaces a lost
 * one. The guest's hero stands in for a second hero: it is one of KOTOR's
 * party members, a fallback for the test, not a custom character.
 */
class NetSession : boost::noncopyable {
public:
    /** How often each side sends, in seconds of real time. */
    static constexpr float kSendInterval = 0.05f;

    /** How long without a message before the other side counts as gone, in seconds of real time. */
    static constexpr float kTimeout = 5.0f;

    static constexpr uint16_t kProtocol = 2; /**< 2: snapshots carry the host's pause in a fight */

    enum class MessageType : uint8_t {
        Hello = 1,
        Snapshot = 2,
        GuestState = 3,
        GuestHit = 4,
        HeroSpec = 5, /**< the guest's own hero, as writeNetHero has it */
        HeroReady = 6 /**< the host made it: its ID and where it stands */
    };

    NetSession(Game &game, ServicesView &services);

    NetRole role() const { return _role; }
    bool connected() const { return _connected; }

    /** The UDP port this side listens on; 0 with no session. */
    uint16_t localPort() const { return _socket.localPort(); }

    /**
     * The guest's hero, by its party NPC number; never the PC, kNpcPlayer,
     * which is the host's. kNpcSessionHero for a hero of the guest's own,
     * set with setHero before joining.
     */
    int guestNpc() const { return _guestNpc; }

    /** On the guest, before joining with kNpcSessionHero: the hero it brings. */
    void setHero(const Character &hero);
    const std::optional<Character> &hero() const { return _hero; }

    ServicesView &services() { return _services; }

    /** The second hero's ID on both sides, once the host made it; 0 before, or with a party member instead. */
    uint32_t heroId() const { return _heroId; }

    /**
     * Starts hosting on \p port, for a guest who plays party member \p guestNpc.
     *
     * \throws std::runtime_error when the socket can't be bound
     */
    void host(uint16_t port, int guestNpc);

    /**
     * Joins the host at \p host and \p port, playing party member
     * \p guestNpc, which becomes this side's party leader.
     *
     * \throws std::runtime_error when the address is unknown or the member isn't in the party
     */
    void join(const std::string &host, uint16_t port, int guestNpc);

    /** Ends the session; \p reason is logged. */
    void stop(const std::string &reason = std::string());

    /**
     * Every frame, after the module's update, paused or not, but not while
     * a movie plays (Game::update returns before it then): takes in what
     * arrived, applies it, and sends what is due, by real time.
     */
    void update();

    /** Whether this side leaves the game to the host: no scripts, combat rounds, saves or area changes. */
    bool followsHost() const { return _role == NetRole::Guest; }

    /**
     * On the guest, sends a hit of its hero's saber on \p target to the host
     * instead of applying it; on the host, or with no session, does nothing.
     *
     * \return whether it took the hit
     */
    bool forwardSaberHit(const Creature &target, JaBodyRegion region);

    /** The hash of the area's objects, by runtime ID, tag and type, that both sides compare. */
    uint64_t manifestHash() const;

    /** One line on the session: role, peer, counts, and whether the two sides' objects matched. */
    std::string status() const;

    /** Replaces the clock of real time, for tests: seconds since some start. */
    void setClock(std::function<double()> clock) { _clock = std::move(clock); }

    struct Counts {
        int sent {0};
        int received {0};
        int applied {0};      /**< creature states applied */
        int clearedActions {0}; /**< actions cleared from creatures the other side drives */
        int hitsForwarded {0};
        int hitsApplied {0};
        int rejected {0};     /**< datagrams that weren't this protocol's, or came from elsewhere */
        int unknown {0};      /**< creature states for IDs this side doesn't have */
        int stale {0};        /**< snapshots older than one applied */
        int held {0};         /**< the guest's states and hits the host held off while in a conversation, or hits while not in play */
    };
    const Counts &counts() const { return _counts; }

    /** On the guest, whether the host has paused in a fight, which pauses this side too. */
    bool hostPausedInFight() const { return _hostPausedInFight; }

    /** Whether the other side's objects matched this side's; none until its hello arrived. */
    std::optional<bool> manifestsMatch() const { return _manifestsMatch; }

private:
    Game &_game;
    ServicesView &_services;
    NetSocket _socket;
    NetRole _role {NetRole::None};
    NetSocket::Address _peer;
    bool _connected {false};
    int _guestNpc {0};
    uint32_t _sequence {0};
    uint32_t _lastReceived {0};
    double _lastHeard {0.0};
    double _lastUpdate {0.0};
    float _sinceSend {0.0f};
    float _sinceHello {0.0f};
    bool _holding {false};
    double _holdUntil {0.0};
    bool _warnedNoHero {false};
    bool _hostPausedInFight {false};
    std::optional<Character> _hero; /**< the guest's own hero, to send; the host's copy once received */
    uint32_t _heroId {0};
    float _sinceHeroSpec {0.0f};
    bool _pausedByHost {false}; /**< whether the guest was playing when the host's pause stopped it */
    std::set<uint32_t> _unknownIds;
    std::optional<bool> _manifestsMatch;
    NetSocket::Address _refused; /**< the last side refused for playing another member, warned of once */
    Counts _counts;
    std::function<double()> _clock;

    double now() const;
    std::shared_ptr<Creature> guestHero() const;
    bool partyInCombat() const;
    void followHostPause(bool pausedInFight);
    void sendHeroSpec();
    void sendHeroReady();
    void handleHeroSpec(const std::vector<uint8_t> &bytes);
    void handleHeroReady(const std::vector<uint8_t> &bytes);
    void removeHero();
    std::vector<uint8_t> header(MessageType type);
    void sendHello();
    void sendSnapshot();
    void sendGuestState();
    void handle(const NetSocket::Datagram &datagram);
    void apply(const NetCreatureState &state, bool position);
    void applyHit(uint32_t target, JaBodyRegion region);
};

/**
 * The session's console commands: nethost [port] [npc], netjoin <host>
 * [port] [npc], netstatus and netoff. The NPC is the party member the guest
 * plays, by default the first after the PC.
 */
void registerNetConsoleCommands(Game &game, IConsole &console);

} // namespace game

} // namespace reone
