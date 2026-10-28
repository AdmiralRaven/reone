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

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
// As the Windows SDK's mstcpip.h has it; MinGW's may not.
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "reone/game/netsession.h"

#include "reone/game/attack.h"
#include "reone/game/console.h"
#include "reone/game/d20/class.h"
#include "reone/game/d20/classes.h"
#include "reone/game/di/services.h"
#include "reone/game/portraits.h"
#include "reone/game/effect/damage.h"
#include "reone/game/game.h"
#include "reone/game/object/area.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/item.h"
#include "reone/game/object/module.h"
#include "reone/game/party.h"
#include "reone/system/logutil.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace reone {

namespace game {

namespace {

#ifdef _WIN32
using Handle = SOCKET;
const Handle kNoSocket = INVALID_SOCKET;

void startSockets() {
    static bool started = false;
    if (!started) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("Windows sockets didn't start");
        }
        started = true;
    }
}

void closeHandle(Handle handle) {
    closesocket(handle);
}
#else
using Handle = int;
const Handle kNoSocket = -1;

void startSockets() {
}

void closeHandle(Handle handle) {
    ::close(handle);
}
#endif

Handle handleOf(intptr_t socket) {
    return socket == -1 ? kNoSocket : static_cast<Handle>(socket);
}

constexpr uint8_t kMagic[] {'R', 'N', 'E', 'T'};
// "RNET", protocol, type, sequence, and the sender's session number.
constexpr size_t kHeaderSize = 4 + 2 + 1 + 4 + 4;
constexpr float kHelloInterval = 0.5f;
constexpr float kManifestInterval = 2.0f;
constexpr double kHoldAfterConversation = 1.0;
constexpr uint8_t kFlagPausedInFight = 1; // in a snapshot: the host's player paused with its party in a fight
constexpr uint8_t kFlagPaused = 1;        // in a guest state: the guest's player paused
constexpr size_t kMaxDatagram = 1400;

// Little-endian writing and reading.

void putU8(std::vector<uint8_t> &out, uint8_t value) {
    out.push_back(value);
}

void putU16(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void putU32(std::vector<uint8_t> &out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xff));
    }
}

void putU64(std::vector<uint8_t> &out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xff));
    }
}

void putF32(std::vector<uint8_t> &out, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    putU32(out, bits);
}

class Reader {
public:
    explicit Reader(const std::vector<uint8_t> &bytes, size_t at = 0) :
        _bytes(bytes),
        _at(at) {
    }

    bool ok() const { return _ok; }

    uint8_t u8() {
        if (!need(1)) {
            return 0;
        }
        return _bytes[_at++];
    }

    uint16_t u16() {
        if (!need(2)) {
            return 0;
        }
        uint16_t value = static_cast<uint16_t>(_bytes[_at] | (_bytes[_at + 1] << 8));
        _at += 2;
        return value;
    }

    uint32_t u32() {
        if (!need(4)) {
            return 0;
        }
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            value |= static_cast<uint32_t>(_bytes[_at + i]) << (8 * i);
        }
        _at += 4;
        return value;
    }

    uint64_t u64() {
        uint64_t low = u32();
        uint64_t high = u32();
        return low | (high << 32);
    }

    float f32() {
        uint32_t bits = u32();
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

private:
    const std::vector<uint8_t> &_bytes;
    size_t _at;
    bool _ok {true};

    bool need(size_t count) {
        if (_at + count > _bytes.size()) {
            _ok = false;
            return false;
        }
        return true;
    }
};

void putState(std::vector<uint8_t> &out, const NetCreatureState &state) {
    putU32(out, state.id);
    putF32(out, state.position.x);
    putF32(out, state.position.y);
    putF32(out, state.position.z);
    putF32(out, state.facing);
    putU8(out, state.movement);
    putU8(out, state.dead ? 1 : 0);
    putU16(out, static_cast<uint16_t>(state.hitPoints));
}

NetCreatureState readState(Reader &in) {
    NetCreatureState state;
    state.id = in.u32();
    state.position.x = in.f32();
    state.position.y = in.f32();
    state.position.z = in.f32();
    state.facing = in.f32();
    state.movement = in.u8();
    state.dead = in.u8() != 0;
    state.hitPoints = static_cast<int16_t>(in.u16());
    return state;
}

NetCreatureState stateOf(const Creature &creature) {
    NetCreatureState state;
    state.id = creature.id();
    state.position = creature.position();
    state.facing = creature.getFacing();
    state.movement = static_cast<uint8_t>(creature.movementType());
    state.dead = creature.isDead();
    state.hitPoints = static_cast<int16_t>(glm::clamp(creature.currentHitPoints(), -32768, 32767));
    return state;
}

// FNV-1a, 64 bits.
void mix(uint64_t &hash, const void *data, size_t size) {
    auto bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
}

const char *roleName(NetRole role) {
    switch (role) {
    case NetRole::Host:
        return "host";
    case NetRole::Guest:
        return "guest";
    default:
        return "none";
    }
}

std::vector<std::string> splitList(const std::string &value) {
    std::vector<std::string> items;
    std::string item;
    std::istringstream in(value);
    while (std::getline(in, item, ',')) {
        boost::trim(item);
        if (!item.empty()) {
            items.push_back(item);
        }
    }
    return items;
}

int toInt(const std::string &value, const std::string &key) {
    try {
        size_t used = 0;
        int result = std::stoi(value, &used);
        if (used != value.size()) {
            throw std::invalid_argument(value);
        }
        return result;
    } catch (const std::exception &) {
        throw std::runtime_error("The hero's " + key + " isn't a number: " + value);
    }
}

std::pair<int, int> toPair(const std::string &value, const std::string &key) {
    auto colon = value.find(':');
    if (colon == std::string::npos) {
        throw std::runtime_error("The hero's " + key + " needs a number:number: " + value);
    }
    return {toInt(value.substr(0, colon), key), toInt(value.substr(colon + 1), key)};
}

constexpr uint32_t kFirstHeroId = 0x40000000;

// The longest name a hero keeps, in bytes, on both sides.
constexpr size_t kMaxHeroName = 64;

// A hero's name as both sides keep it: one line, cut to kMaxHeroName bytes,
// with no spaces at either end, so that it reads back the same.
std::string heroName(std::string name) {
    boost::replace_all(name, "\n", " ");
    boost::replace_all(name, "\r", " ");
    boost::trim(name);
    name = name.substr(0, kMaxHeroName);
    boost::trim(name);
    return name;
}

// A number no other session drew: not from the game's random numbers, which
// both sides seed alike, and which the host's rolls must keep to themselves.
uint32_t freshNumber() {
    static uint32_t drawn = 0;
    uint64_t x = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^
                 (static_cast<uint64_t>(++drawn) << 40) ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&drawn));
    // splitmix64's finish, so that close clocks give far numbers.
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    x ^= x >> 31;
    uint32_t number = static_cast<uint32_t>(x) ^ static_cast<uint32_t>(x >> 32);
    return number ? number : 1;
}

// When a session begins, in milliseconds of the wall clock, later than any
// this process gave before: the other side takes a session of this side's
// only if it began after the one it has, so that a late message from an
// earlier one, or a stray one, changes nothing.
uint64_t sessionStart() {
    static uint64_t last = 0;
    auto wall = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
    last = std::max(wall, last + 1);
    return last;
}

// This side's player's pause, which the other side follows in a fight: the
// pause key's, or a menu open over the game, as Escape's options menu is
// (user, 2026-10-28), which stops this side's world as a pause does.
bool pausedByPlayer(const Game &game) {
    return game.isPausedByPlayer() || game.isInPlayerMenu();
}

// "Revan, a level 1 Soldier", for the log.
std::string heroSummary(const Character &hero) {
    std::string classes;
    for (auto &[clazz, levels] : hero.attributes.classLevels()) {
        classes += (classes.empty() ? "" : " and ") + str(boost::format("level %d %s") % levels % clazz->name());
    }
    return hero.name + ", " + (classes.empty() ? std::string("no class") : classes);
}

} // namespace

// The second hero

std::string writeNetHero(const Character &character) {
    std::ostringstream out;
    // As readNetHero takes it, so that both sides show the same name.
    std::string name(heroName(character.name));
    out << "reone-hero 1\n";
    out << "name=" << name << "\n";
    out << "gender=" << (character.gender == Gender::Female ? "female" : "male") << "\n";
    out << "appearance=" << character.appearance << "\n";
    for (auto &[clazz, levels] : character.attributes.classLevels()) {
        out << "class=" << static_cast<int>(clazz->type()) << ":" << levels << "\n";
    }
    const auto &a = character.attributes;
    out << "abilities=" << a.strength() << "," << a.dexterity() << "," << a.constitution() << "," << a.intelligence()
        << "," << a.wisdom() << "," << a.charisma() << "\n";
    out << "skills=";
    bool first = true;
    for (auto &[skill, rank] : a.skillRanks()) {
        out << (first ? "" : ",") << static_cast<int>(skill) << ":" << rank;
        first = false;
    }
    out << "\nfeats=";
    first = true;
    for (auto feat : a.feats()) {
        out << (first ? "" : ",") << static_cast<int>(feat);
        first = false;
    }
    out << "\npowers=";
    first = true;
    for (auto power : a.spells()) {
        out << (first ? "" : ",") << static_cast<int>(power);
        first = false;
    }
    out << "\n";
    return out.str();
}

bool writeNetHeroFile(const std::string &path, const Character &character) {
    std::string temporary(path + ".new");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << writeNetHero(character);
        out.flush();
        if (!out) {
            return false;
        }
    }
    // One rename replaces the old file, on Windows (MSVC's runtime) and
    // elsewhere. If it can't, as with the file held open, the old file stays,
    // and the new one beside it.
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}

Character readNetHero(const std::string &text, IClasses &classes) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || boost::trim_copy(line) != "reone-hero 1") {
        throw std::runtime_error("Not a reone hero: it must start with \"reone-hero 1\"");
    }
    Character character;
    bool hasClass = false;
    while (std::getline(in, line)) {
        boost::trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        auto equals = line.find('=');
        if (equals == std::string::npos) {
            throw std::runtime_error("Not a hero's line, with no '=': " + line);
        }
        std::string key(boost::trim_copy(line.substr(0, equals)));
        std::string value(boost::trim_copy(line.substr(equals + 1)));
        if (key == "name") {
            character.name = heroName(value);
        } else if (key == "gender") {
            if (value != "male" && value != "female") {
                throw std::runtime_error("The hero's gender is male or female, not " + value);
            }
            character.gender = value == "female" ? Gender::Female : Gender::Male;
        } else if (key == "appearance") {
            character.appearance = toInt(value, key);
        } else if (key == "class") {
            auto [type, levels] = toPair(value, key);
            auto clazz = classes.get(static_cast<ClassType>(type));
            if (!clazz || levels < 1 || levels > 50) {
                throw std::runtime_error("No class " + std::to_string(type) + " with " + std::to_string(levels) + " levels for the hero");
            }
            character.attributes.addClassLevels(clazz.get(), levels);
            hasClass = true;
        } else if (key == "abilities") {
            auto items = splitList(value);
            if (items.size() != 6) {
                throw std::runtime_error("The hero needs six abilities, strength to charisma: " + value);
            }
            for (int i = 0; i < 6; ++i) {
                character.attributes.setAbilityScore(static_cast<Ability>(i), toInt(items[i], key));
            }
        } else if (key == "skills") {
            for (auto &item : splitList(value)) {
                auto [skill, rank] = toPair(item, key);
                character.attributes.setSkillRank(static_cast<SkillType>(skill), rank);
            }
        } else if (key == "feats") {
            for (auto &item : splitList(value)) {
                character.attributes.addFeat(static_cast<FeatType>(toInt(item, key)));
            }
        } else if (key == "powers") {
            for (auto &item : splitList(value)) {
                character.attributes.addSpell(static_cast<SpellType>(toInt(item, key)));
            }
        }
        // Other keys are a later version's: left alone.
    }
    if (character.name.empty() || !hasClass) {
        throw std::runtime_error("The hero needs a name and a class");
    }
    return character;
}

Character quickNetHero(ServicesView &services, ClassType clazz, Gender gender, std::string name) {
    auto creatureClass = services.game.classes.get(clazz);
    if (!creatureClass) {
        throw std::runtime_error("No class " + std::to_string(static_cast<int>(clazz)) + " in this game");
    }
    Character character;
    character.name = std::move(name);
    character.gender = gender;
    character.attributes = creatureClass->defaultAttributes();
    int sex = gender == Gender::Female ? 1 : 0;
    for (auto &portrait : services.game.portraits.portraits()) {
        if (!portrait.forPC || portrait.sex != sex) {
            continue;
        }
        // As ClassSelection::getRandomCharacterAppearance picks the body.
        switch (clazz) {
        case ClassType::Scoundrel:
        case ClassType::JediConsular:
            character.appearance = portrait.appearanceS;
            break;
        case ClassType::Soldier:
        case ClassType::JediGuardian:
            character.appearance = portrait.appearanceL;
            break;
        default:
            character.appearance = portrait.appearanceNumber;
            break;
        }
        break;
    }
    return character;
}

std::shared_ptr<Creature> makeNetHero(Game &game, uint32_t id, const Character &character) {
    std::vector<std::shared_ptr<Object>> noObsolete;
    std::shared_ptr<Creature> hero;
    game.replaceRuntimeObjectGraph(
        noObsolete,
        [&]() {
            hero = game.newCreatureAtId(id);
            if (!hero) {
                return;
            }
            // As CharacterGeneration::finish makes the PC.
            hero->setTag("coop_hero");
            hero->setName(character.name);
            hero->setGender(character.gender);
            hero->setAppearance(character.appearance);
            try {
                hero->loadAppearance();
            } catch (const std::exception &e) {
                // A hero file's look the game doesn't have: the hero comes
                // all the same, unseen, rather than stop the session.
                warn(str(boost::format("net: no look for the hero's appearance %d: %s") % character.appearance % e.what()));
            }
            hero->setFaction(Faction::Friendly1);
            hero->setImmortal(true);
            hero->attributes() = character.attributes;
            hero->initializeGeneratedVitality();
        },
        []() noexcept {});
    if (hero) {
        game.setSessionOnly(hero->id(), true);
    }
    return hero;
}

// NetSocket

std::string NetSocket::Address::str() const {
    return boost::str(boost::format("%d.%d.%d.%d:%d") % ((ip >> 24) & 0xff) % ((ip >> 16) & 0xff) % ((ip >> 8) & 0xff) % (ip & 0xff) % port);
}

NetSocket::~NetSocket() {
    close();
}

void NetSocket::open(uint16_t port) {
    close();
    startSockets();
    Handle handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == kNoSocket) {
        throw std::runtime_error("No UDP socket");
    }
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (::bind(handle, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        closeHandle(handle);
        throw std::runtime_error("UDP port " + std::to_string(port) + " is taken");
    }
#ifdef _WIN32
    u_long nonBlocking = 1;
    bool blocking = ioctlsocket(handle, FIONBIO, &nonBlocking) != 0;
    // A datagram to a port no one listens on, as to a guest gone, makes
    // Windows fail the next receive with WSAECONNRESET; have it not.
    BOOL reportResets = FALSE;
    DWORD returned = 0;
    if (WSAIoctl(handle, SIO_UDP_CONNRESET, &reportResets, sizeof(reportResets), nullptr, 0, &returned, nullptr, nullptr) != 0) {
        warn("net: Windows will report resets on this socket; receive skips them");
    }
#else
    int flags = fcntl(handle, F_GETFL, 0);
    bool blocking = flags < 0 || fcntl(handle, F_SETFL, flags | O_NONBLOCK) != 0;
#endif
    if (blocking) {
        // A blocking receive would stop the game every frame.
        closeHandle(handle);
        throw std::runtime_error("The UDP socket can't be made non-blocking");
    }
    _socket = static_cast<intptr_t>(handle);
}

void NetSocket::close() {
    if (_socket != -1) {
        closeHandle(handleOf(_socket));
        _socket = -1;
    }
}

bool NetSocket::isOpen() const {
    return _socket != -1;
}

uint16_t NetSocket::localPort() const {
    if (!isOpen()) {
        return 0;
    }
    sockaddr_in address {};
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (::getsockname(handleOf(_socket), reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        return 0;
    }
    return ntohs(address.sin_port);
}

void NetSocket::send(const Address &to, const std::vector<uint8_t> &bytes) {
    if (!isOpen()) {
        return;
    }
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(to.ip);
    address.sin_port = htons(to.port);
    ::sendto(handleOf(_socket), reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()), 0,
             reinterpret_cast<sockaddr *>(&address), sizeof(address));
}

std::optional<NetSocket::Datagram> NetSocket::receive() {
    if (!isOpen()) {
        return std::nullopt;
    }
    _buffer.resize(65536);
    // A reset, from a datagram sent to a port no one listens on, says nothing
    // of what is waiting: past it, a few times, to the next datagram.
    for (int attempt = 0; attempt < 8; ++attempt) {
        sockaddr_in address {};
#ifdef _WIN32
        int length = sizeof(address);
#else
        socklen_t length = sizeof(address);
#endif
        auto received = ::recvfrom(handleOf(_socket), reinterpret_cast<char *>(_buffer.data()), static_cast<int>(_buffer.size()), 0,
                                   reinterpret_cast<sockaddr *>(&address), &length);
        if (received >= 0) {
            Datagram datagram;
            datagram.from.ip = ntohl(address.sin_addr.s_addr);
            datagram.from.port = ntohs(address.sin_port);
            datagram.bytes.assign(_buffer.begin(), _buffer.begin() + received);
            return datagram;
        }
#ifdef _WIN32
        int failure = WSAGetLastError();
        if (failure == WSAECONNRESET) {
            continue;
        }
        if (failure != WSAEWOULDBLOCK) {
            ++_errors;
        }
#else
        int failure = errno;
        if (failure == ECONNREFUSED) {
            continue;
        }
        if (failure != EAGAIN && failure != EWOULDBLOCK) {
            ++_errors;
        }
#endif
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<NetSocket::Address> NetSocket::resolve(const std::string &host, uint16_t port) {
    startSockets();
    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *found = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found) {
        return std::nullopt;
    }
    Address address;
    address.ip = ntohl(reinterpret_cast<sockaddr_in *>(found->ai_addr)->sin_addr.s_addr);
    address.port = port;
    ::freeaddrinfo(found);
    return address;
}

// NetSession

NetSession::NetSession(Game &game, ServicesView &services) :
    _game(game),
    _services(services) {
}

double NetSession::now() const {
    if (_clock) {
        return _clock();
    }
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::shared_ptr<Creature> NetSession::guestHero() const {
    if (_guestNpc == kNpcSessionHero) {
        return _heroId ? _game.getObjectById<Creature>(_heroId) : nullptr;
    }
    for (auto &member : _game.party().members()) {
        if (member.npc == _guestNpc) {
            return member.creature;
        }
    }
    return nullptr;
}

bool NetSession::partyInCombat() const {
    for (auto &member : _game.party().members()) {
        if (member.creature && member.creature->isInCombat()) {
            return true;
        }
    }
    // The second hero, on the host no party member, counts as one.
    auto hero = guestHero();
    return hero && hero->isInCombat();
}

void NetSession::setHero(const Character &hero, std::optional<std::string> file) {
    if (_role != NetRole::None) {
        throw std::runtime_error("Set the hero before joining: netoff first");
    }
    _hero = hero;
    _heroFile = std::move(file);
    // As the host will have it, from writeNetHero.
    _hero->name = heroName(hero.name);
    info("net: the hero to bring is " + heroSummary(*_hero));
}

void NetSession::followPause(bool pausedInFight) {
    // The other side's player paused in a fight: this side is held paused,
    // whatever its own player does, and its own pause stays as it was under
    // it. Each side sends only its player's own pause, so that two holds
    // never keep each other up.
    if (pausedInFight == _otherPausedInFight) {
        return;
    }
    _otherPausedInFight = pausedInFight;
    _game.setPauseHeld(pausedInFight);
    const char *other = _role == NetRole::Host ? "guest" : "host";
    info(pausedInFight ? str(boost::format("net: the %s paused in a fight; this side waits for it") % other)
                       : str(boost::format("net: the %s plays on") % other));
}

void NetSession::host(uint16_t port, int guestNpc) {
    stop();
    if (!_game.module() || !_game.module()->area()) {
        throw std::runtime_error("Load a game first");
    }
    if (guestNpc == kNpcPlayer) {
        throw std::runtime_error("The guest plays a party member; the PC is the host's");
    }
    _guestNpc = guestNpc;
    if (guestNpc != kNpcSessionHero && !guestHero()) {
        throw std::runtime_error("No party member " + std::to_string(guestNpc) + " for the guest to play");
    }
    _socket.open(port);
    _role = NetRole::Host;
    _session = freshNumber();
    _sessionStart = sessionStart();
    _lastHeard = now();
    // The host plays the PC, whichever member the save had leading.
    _game.party().setPartyLeader(kNpcPlayer);
    _game.party().setLeaderLocked(true);
    if (guestNpc == kNpcSessionHero) {
        info(str(boost::format("net: hosting on UDP port %d for a guest bringing a hero of its own; objects %016x") %
                 _socket.localPort() % manifestHash()));
    } else {
        info(str(boost::format("net: hosting on UDP port %d for a guest playing party member %d (%s); objects %016x") %
                 _socket.localPort() % guestNpc % guestHero()->tag() % manifestHash()));
    }
}

void NetSession::join(const std::string &host, uint16_t port, int guestNpc) {
    stop();
    if (!_game.module() || !_game.module()->area()) {
        throw std::runtime_error("Load a game first");
    }
    auto address = NetSocket::resolve(host, port);
    if (!address) {
        throw std::runtime_error("Unknown host " + host);
    }
    if (guestNpc == kNpcPlayer) {
        throw std::runtime_error("The guest plays a party member; the PC is the host's");
    }
    _guestNpc = guestNpc;
    if (guestNpc == kNpcSessionHero && !_hero) {
        throw std::runtime_error("No hero of this side's own to bring: nethero first");
    }
    auto hero = guestHero();
    if (guestNpc != kNpcSessionHero && !hero) {
        throw std::runtime_error("No party member " + std::to_string(guestNpc) + " to play");
    }
    _socket.open(0);
    _peer = *address;
    _role = NetRole::Guest;
    _session = freshNumber();
    _sessionStart = sessionStart();
    _lastHeard = now();
    // The guest's controls, camera and saber mode drive its hero; a hero of
    // its own leads once the host has made it.
    if (hero) {
        _game.party().setPartyLeader(guestNpc);
    }
    _game.party().setLeaderLocked(true);
    _game.setFollowingHost(true);
    _sinceHeroSpec = kHelloInterval;
    if (hero) {
        info(str(boost::format("net: joining %s as party member %d (%s); objects %016x") %
                 _peer.str() % guestNpc % hero->tag() % manifestHash()));
    } else {
        info(str(boost::format("net: joining %s with a hero of this side's own, %s; objects %016x") %
                 _peer.str() % _hero->name % manifestHash()));
    }
    sendHello();
}

void NetSession::stop(const std::string &reason) {
    if (_role == NetRole::None) {
        return;
    }
    _game.party().setLeaderLocked(false);
    // The second hero lives only as long as the session.
    removeHero();
    if (_role == NetRole::Host) {
        _hero.reset();
    }
    if (_otherPausedInFight) {
        _game.setPauseHeld(false);
        _otherPausedInFight = false;
    }
    if (_role == NetRole::Guest) {
        _game.setFollowingHost(false);
        // The PC leads again.
        if (_game.module() && _game.module()->area()) {
            _game.party().setPartyLeader(kNpcPlayer);
        }
    }
    _socket.close();
    info("net: session over" + (reason.empty() ? std::string() : ", " + reason) + "; " + status());
    _role = NetRole::None;
    _connected = false;
    _manifestsMatch.reset();
    _peer = NetSocket::Address();
    _refused = NetSocket::Address();
    _sequence = 0;
    _lastReceived = 0;
    _sinceSend = 0.0f;
    _sinceHello = 0.0f;
    _lastUpdate = 0.0;
    _holding = false;
    _holdUntil = 0.0;
    _warnedNoHero = false;
    _session = 0;
    _sessionStart = 0;
    _peerSession.reset();
    _peerSessionStart = 0;
    _warnedEarlierSession = 0;
    _heroSession = 0;
    _refusedHeroSession = 0;
    _waitStarted = 0.0;
    _waitUntil = 0.0;
    _heroVersionApplied = 0;
    _heroVersionAcked = 0;
    _heroVersion = 0;
    _heroText.clear();
    _unknownIds.clear();
    _counts = Counts();
}

void NetSession::waitForPeer(float seconds) {
    if (_role == NetRole::None) {
        throw std::runtime_error("No session: nethost or netjoin first");
    }
    if (seconds <= 0.0f) {
        throw std::runtime_error("Wait a positive number of seconds");
    }
    _waitStarted = now();
    _waitUntil = _waitStarted + seconds;
    info(str(boost::format("net: waiting up to %.0f s for %s") % seconds %
             (_guestNpc == kNpcSessionHero ? "the guest's hero" : (_role == NetRole::Host ? "the guest" : "the host"))));
    checkWait();
}

bool NetSession::peerReady() const {
    if (!_connected) {
        return false;
    }
    if (_guestNpc != kNpcSessionHero) {
        return true;
    }
    auto hero = guestHero();
    // On the guest, the hero is in play once it leads, for the controls and saber mode.
    return hero && (_role == NetRole::Host || _game.party().getLeader() == hero);
}

void NetSession::checkWait() {
    if (_waitUntil <= 0.0) {
        return;
    }
    double time = now();
    if (peerReady()) {
        auto hero = guestHero();
        info(str(boost::format("net: waited %.1f s; %s") % (time - _waitStarted) %
                 (_guestNpc == kNpcSessionHero ? "the guest's hero is here, " + (hero ? hero->tag() : std::string()) +
                                                     (_role == NetRole::Guest ? ", leading this side" : "")
                                               : std::string("the other side is here"))));
        _waitUntil = 0.0;
    } else if (time >= _waitUntil) {
        warn(str(boost::format("net: waited %.0f s, and %s; going on") % (time - _waitStarted) %
                 (!_connected ? "the other side isn't here" : "the guest's hero isn't here")));
        _waitUntil = 0.0;
    }
}

uint64_t NetSession::manifestHash() const {
    uint64_t hash = 1469598103934665603ull;
    auto module = _game.module();
    if (!module || !module->area()) {
        return hash;
    }
    std::vector<std::tuple<uint32_t, int, std::string>> objects;
    for (auto &object : module->area()->objects()) {
        // The second hero, which the guest makes once the host has, apart.
        if (_game.isSessionOnly(object->id())) {
            continue;
        }
        objects.emplace_back(object->id(), static_cast<int>(object->type()), boost::to_lower_copy(object->tag()));
    }
    std::sort(objects.begin(), objects.end());
    for (auto &[id, type, tag] : objects) {
        mix(hash, &id, sizeof(id));
        mix(hash, &type, sizeof(type));
        mix(hash, tag.data(), tag.size());
    }
    return hash;
}

std::vector<uint8_t> NetSession::header(MessageType type) {
    std::vector<uint8_t> out(kMagic, kMagic + 4);
    putU16(out, kProtocol);
    putU8(out, static_cast<uint8_t>(type));
    putU32(out, ++_sequence);
    putU32(out, _session);
    return out;
}

void NetSession::sendHello() {
    auto out = header(MessageType::Hello);
    putU8(out, static_cast<uint8_t>(_role));
    putU32(out, static_cast<uint32_t>(_guestNpc));
    putU64(out, manifestHash());
    auto module = _game.module();
    putU32(out, module && module->area() ? static_cast<uint32_t>(module->area()->objects().size()) : 0);
    putU64(out, _sessionStart);
    _socket.send(_peer, out);
    ++_counts.sent;
}

void NetSession::sendSnapshot() {
    auto module = _game.module();
    if (!module || !module->area()) {
        return;
    }
    auto hero = guestHero();
    std::vector<NetCreatureState> states;
    for (auto &object : module->area()->getObjectsByType(ObjectType::Creature)) {
        auto creature = std::static_pointer_cast<Creature>(object);
        states.push_back(stateOf(*creature));
    }
    // As many datagrams as it takes, each under kMaxDatagram.
    // The host's player's pause in a fight, which the guest follows
    // (kFlagPausedInFight); not a hold the guest put on it.
    uint8_t flags = pausedByPlayer(_game) && partyInCombat() ? kFlagPausedInFight : 0;
    // The party's experience, which the guest's hero shares, as KOTOR's party
    // members do; on the host, the hero, no party member, takes it here.
    int xp = _game.party().xp();
    if (hero && hero->xp() != xp) {
        hero->setXP(xp);
    }
    size_t perDatagram = (kMaxDatagram - kHeaderSize - 11) / 24;
    for (size_t first = 0; first < states.size() || first == 0; first += perDatagram) {
        auto out = header(MessageType::Snapshot);
        size_t count = std::min(perDatagram, states.size() - std::min(first, states.size()));
        putU8(out, flags);
        putU32(out, static_cast<uint32_t>(std::max(xp, 0)));
        putU32(out, _heroVersionApplied);
        putU16(out, static_cast<uint16_t>(count));
        for (size_t i = first; i < first + count; ++i) {
            putState(out, states[i]);
        }
        _socket.send(_peer, out);
        ++_counts.sent;
        if (states.empty()) {
            break;
        }
    }
}

void NetSession::sendGuestState() {
    auto hero = guestHero();
    if (!hero) {
        return;
    }
    auto out = header(MessageType::GuestState);
    // The guest's player's pause, which the host follows in a fight.
    putU8(out, pausedByPlayer(_game) ? kFlagPaused : 0);
    putState(out, stateOf(*hero));
    _socket.send(_peer, out);
    ++_counts.sent;
}

bool NetSession::forwardSaberHit(const Creature &target, JaBodyRegion region) {
    if (_role != NetRole::Guest) {
        return false;
    }
    auto out = header(MessageType::GuestHit);
    putU32(out, target.id());
    putU8(out, static_cast<uint8_t>(region));
    _socket.send(_peer, out);
    ++_counts.sent;
    ++_counts.hitsForwarded;
    info(str(boost::format("net: sent the host a hit on %s (%s)") % target.tag() % jaBodyRegionName(region)));
    return true;
}

void NetSession::update() {
    if (_role == NetRole::None) {
        return;
    }
    double time = now();
    float elapsed = _lastUpdate > 0.0 ? static_cast<float>(time - _lastUpdate) : 0.0f;
    _lastUpdate = time;

    while (auto datagram = _socket.receive()) {
        try {
            handle(*datagram);
        } catch (const std::exception &e) {
            // Whatever a message asked, the game goes on.
            ++_counts.rejected;
            warn(std::string("net: a message from ") + datagram->from.str() + " couldn't be taken in: " + e.what());
        }
    }

    if (_connected && time - _lastHeard > kTimeout) {
        info(str(boost::format("net: nothing from %s for %.0f s") % _peer.str() % kTimeout));
        _connected = false;
        // A side gone quiet holds no pause.
        followPause(false);
    }
    // A command file waiting for the other side goes on once it is here.
    checkWait();

    _sinceSend += elapsed;
    _sinceHello += elapsed;
    if (_role == NetRole::Guest && !_connected) {
        if (_sinceSend >= kHelloInterval) {
            _sinceSend = 0.0f;
            sendHello();
        }
        return;
    }
    if (_role == NetRole::Guest && _sinceHello >= kManifestInterval) {
        // Now and then, for both sides to compare their objects again.
        _sinceHello = 0.0f;
        sendHello();
    }
    if (_role == NetRole::Guest && _guestNpc == kNpcSessionHero && _heroId) {
        // A level-up of the hero's, which the host takes, and the file keeps.
        _sinceHeroUpdate += elapsed;
        if (_sinceHeroUpdate >= kHelloInterval) {
            _sinceHeroUpdate = 0.0f;
            checkHeroChanged();
            if (_heroVersionAcked < _heroVersion) {
                sendHeroUpdate();
            }
        }
    }
    if (_role == NetRole::Guest && _guestNpc == kNpcSessionHero && !_heroId) {
        // The hero of this side's own, until the host says it has made it.
        _sinceHeroSpec += elapsed;
        if (_sinceHeroSpec >= kHelloInterval) {
            _sinceHeroSpec = 0.0f;
            sendHeroSpec();
        }
    }
    if (!_connected || _sinceSend < kSendInterval) {
        return;
    }
    _sinceSend = 0.0f;
    if (_role == NetRole::Host) {
        sendSnapshot();
    } else {
        sendGuestState();
    }
}

void NetSession::handle(const NetSocket::Datagram &datagram) {
    const auto &bytes = datagram.bytes;
    if (bytes.size() < kHeaderSize || !std::equal(kMagic, kMagic + 4, bytes.begin())) {
        ++_counts.rejected;
        return;
    }
    Reader in(bytes, 4);
    uint16_t protocol = in.u16();
    auto type = static_cast<MessageType>(in.u8());
    uint32_t sequence = in.u32();
    uint32_t session = in.u32();
    if (protocol != kProtocol || !session) {
        ++_counts.rejected;
        return;
    }
    // Only a hello can begin a session of the other side's, and on the host
    // it may come from elsewhere: a guest, new or come back. It is taken only
    // once it checks out, so that a stray one cuts no one off.
    if (type != MessageType::Hello) {
        if (datagram.from != _peer) {
            ++_counts.rejected;
            return;
        }
        if (!_peerSession || session != *_peerSession) {
            // From a session of the other side's that is over: late.
            ++_counts.stale;
            return;
        }
        ++_counts.received;
        _lastHeard = now();
    } else if (_role == NetRole::Guest && datagram.from != _peer) {
        ++_counts.rejected;
        return;
    }

    switch (type) {
    case MessageType::Hello: {
        auto role = static_cast<NetRole>(in.u8());
        int npc = static_cast<int>(in.u32());
        uint64_t manifest = in.u64();
        uint32_t objects = in.u32();
        uint64_t start = in.u64();
        if (!in.ok()) {
            ++_counts.rejected;
            return;
        }
        bool newSession = !_peerSession || session != *_peerSession;
        if (newSession && _peerSession && start <= _peerSessionStart) {
            // A session that began before the one this side has, by the other
            // side's clock: late, or a guest that another has since replaced,
            // while that one is heard. Once it is quiet, the clock may have
            // gone back, as across an engine restart: the session is taken.
            if (_connected) {
                ++_counts.stale;
                if (_warnedEarlierSession != session) {
                    _warnedEarlierSession = session;
                    warn(str(boost::format("net: %s began a session before the one this side has; refused while that one is heard") %
                             datagram.from.str()));
                }
                return;
            }
            warn(str(boost::format("net: %s began a session before the last one, by its clock; taking it, as the last is quiet") %
                     datagram.from.str()));
        }
        if (npc != _guestNpc) {
            // Both sides would drive different heroes, and neither the other's.
            ++_counts.rejected;
            if (_refused != datagram.from) {
                _refused = datagram.from;
                warn(str(boost::format("net: %s %s plays party member %d, not %d as this side; start both with the same one") %
                         roleName(role) % datagram.from.str() % npc % _guestNpc));
            }
            return;
        }
        ++_counts.received;
        _lastHeard = now();
        if (newSession) {
            // A hold of the side's earlier session's goes with it; the new
            // one's next state says whether it holds.
            followPause(false);
        }
        // The other side started over, or a guest joined, again or anew: its
        // messages count from 1 again.
        bool startedOver = newSession && _peerSession;
        if (_role == NetRole::Host && (newSession || datagram.from != _peer)) {
            if (newSession) {
                _connected = false;
                _lastReceived = 0;
            }
            _peer = datagram.from;
        }
        if (startedOver && _role == NetRole::Guest) {
            _lastReceived = 0;
            if (_guestNpc == kNpcSessionHero && _heroId) {
                // Its game has no hero now: this side's goes, and is sent again.
                removeHero();
                _sinceHeroSpec = kHelloInterval;
                info(str(boost::format("net: the host %s started over; this side's hero comes again once it is made there") %
                         _peer.str()));
            } else {
                info(str(boost::format("net: the host %s started over") % _peer.str()));
            }
        }
        _peerSession = session;
        _peerSessionStart = start;
        uint64_t mine = manifestHash();
        bool match = manifest == mine;
        bool first = !_connected;
        bool changed = _manifestsMatch && *_manifestsMatch != match;
        _manifestsMatch = match;
        _connected = true;
        if (first) {
            info(str(boost::format("net: %s %s is here, playing party member %d; objects %s: %d there, %016x, and %016x here") %
                     roleName(role) % _peer.str() % npc % (match ? "match" : "DIFFER") % objects % manifest % mine));
        } else if (changed) {
            info(str(boost::format("net: the two sides' objects %s now: %d there, %016x, and %016x here") %
                     (match ? "match again" : "DIFFER") % objects % manifest % mine));
        }
        if (_role == NetRole::Host) {
            // Answer each hello, so a guest knows it is heard.
            sendHello();
        }
        break;
    }
    case MessageType::Snapshot: {
        if (_role != NetRole::Guest) {
            return;
        }
        if (sequence <= _lastReceived) {
            // Older than one applied: a newer one has said more.
            ++_counts.stale;
            return;
        }
        uint8_t flags = in.u8();
        int xp = static_cast<int>(in.u32());
        uint32_t heroVersionTaken = in.u32();
        uint16_t count = in.u16();
        if (!in.ok()) {
            ++_counts.rejected;
            return;
        }
        followPause((flags & kFlagPausedInFight) != 0);
        _heroVersionAcked = std::max(_heroVersionAcked, heroVersionTaken);
        // The host's party experience, which every member here shares, the
        // hero among them: KOTOR then offers it a level-up.
        auto &party = _game.party();
        auto ownHero = guestHero();
        if (party.xp() != xp || (ownHero && ownHero->xp() != xp)) {
            party.setXP(xp);
        }
        auto hero = guestHero();
        for (uint16_t i = 0; i < count; ++i) {
            auto state = readState(in);
            if (!in.ok()) {
                ++_counts.rejected;
                break;
            }
            // The guest drives its hero; the host decides its hit points.
            bool own = hero && state.id == hero->id();
            apply(state, !own);
        }
        _lastReceived = std::max(_lastReceived, sequence);
        break;
    }
    case MessageType::GuestState: {
        if (_role != NetRole::Host) {
            return;
        }
        if (sequence < _lastReceived) {
            ++_counts.stale;
            return;
        }
        _lastReceived = sequence;
        uint8_t flags = in.u8();
        auto state = readState(in);
        if (in.ok()) {
            // The guest's pause holds this side only in a fight, as the host decides.
            followPause((flags & kFlagPaused) != 0 && partyInCombat());
        }
        auto hero = guestHero();
        if (in.ok() && !hero && !_warnedNoHero) {
            _warnedNoHero = true;
            if (_guestNpc == kNpcSessionHero) {
                warn("net: the guest plays a hero this side hasn't made; the guest sends it again once it hears this side");
            } else {
                warn(str(boost::format("net: party member %d, the guest's hero, isn't in the host's party now") % _guestNpc));
            }
        }
        if (!in.ok() || !hero || state.id != hero->id()) {
            ++_counts.rejected;
            if (in.ok() && hero && _counts.rejected == 1) {
                warn(str(boost::format("net: the guest sent creature %d as its hero, but here that is %s, %d") %
                         state.id % hero->tag() % hero->id()));
            }
            return;
        }
        // In a conversation, KOTOR's scene may move the hero, and waits for
        // its actions: the guest's place waits until a second after it ends,
        // for what the conversation's last script gives the hero to do.
        double time = now();
        if (_game.currentScreen() == Game::Screen::Conversation) {
            _holdUntil = time + kHoldAfterConversation;
        }
        bool holding = time < _holdUntil;
        if (holding != _holding) {
            _holding = holding;
            info(holding ? "net: the host is in a conversation; the guest's hero stays as the scene has it"
                         : "net: the conversation is over; the guest's hero goes where the guest has it");
        }
        if (holding) {
            ++_counts.held;
            return;
        }
        // Where the guest has its hero, but the host keeps its hit points.
        NetCreatureState placed(stateOf(*hero));
        placed.position = state.position;
        placed.facing = state.facing;
        placed.movement = state.movement;
        apply(placed, true);
        break;
    }
    case MessageType::HeroSpec: {
        if (_role == NetRole::Host) {
            handleHeroSpec(bytes, session);
        }
        break;
    }
    case MessageType::HeroReady: {
        if (_role == NetRole::Guest) {
            handleHeroReady(bytes);
        }
        break;
    }
    case MessageType::HeroUpdate: {
        if (_role == NetRole::Host) {
            handleHeroUpdate(bytes);
        }
        break;
    }
    case MessageType::GuestHit: {
        if (_role != NetRole::Host) {
            return;
        }
        uint32_t target = in.u32();
        auto region = static_cast<JaBodyRegion>(in.u8());
        if (in.ok()) {
            applyHit(target, region);
        }
        break;
    }
    default:
        ++_counts.rejected;
        break;
    }
}

void NetSession::apply(const NetCreatureState &state, bool position) {
    auto creature = _game.getObjectById<Creature>(state.id);
    auto module = _game.module();
    if (!module || !module->area()) {
        return;
    }
    if (!creature) {
        // Such as one a script spawned on the host after the join.
        ++_counts.unknown;
        if (_unknownIds.insert(state.id).second) {
            warn(str(boost::format("net: the %s sent creature %d, which this side doesn't have") %
                     (_role == NetRole::Guest ? "host" : "guest") % state.id));
        }
        return;
    }
    if (position) {
        // It does only what the other side sends: its own AI and the party's
        // following would fight that.
        if (!creature->actions().empty()) {
            _counts.clearedActions += static_cast<int>(creature->actions().size());
            creature->clearAllActions(true);
        }
        creature->setPosition(state.position);
        creature->setFacing(state.facing);
        module->area()->determineObjectRoom(*creature);
        auto movement = static_cast<Creature::MovementType>(state.movement);
        if (creature->movementType() != movement) {
            creature->setMovementType(movement);
        }
    }
    if (_role == NetRole::Guest) {
        if (state.dead && !creature->isDead()) {
            // As the console's kill does: the guest's scripts don't run.
            auto effect = _game.newEffect<DamageEffect>(100000, DamageType::Universal, DamagePower::Normal);
            creature->applyEffect(std::move(effect), DurationType::Instant);
        } else if (!state.dead) {
            // Alive on the host, if at 0 or less, as an immortal creature
            // can be: alive here too, or brought back.
            int hitPoints = std::max<int>(state.hitPoints, 1);
            if (creature->isDead() || creature->currentHitPoints() != hitPoints) {
                creature->setCurrentHitPoints(hitPoints);
            }
        }
    }
    ++_counts.applied;
}

void NetSession::sendHeroSpec() {
    if (!_hero) {
        return;
    }
    auto out = header(MessageType::HeroSpec);
    std::string text(writeNetHero(*_hero));
    putU16(out, static_cast<uint16_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
    _socket.send(_peer, out);
    ++_counts.sent;
}

void NetSession::sendHeroReady() {
    auto hero = guestHero();
    if (!hero) {
        return;
    }
    auto out = header(MessageType::HeroReady);
    putU32(out, hero->id());
    putF32(out, hero->position().x);
    putF32(out, hero->position().y);
    putF32(out, hero->position().z);
    putF32(out, hero->getFacing());
    _socket.send(_peer, out);
    ++_counts.sent;
}

void NetSession::checkHeroChanged() {
    auto hero = guestHero();
    if (!hero || !_hero) {
        return;
    }
    Character now(*_hero);
    now.attributes = hero->attributes();
    std::string text(writeNetHero(now));
    if (text == _heroText) {
        return;
    }
    _heroText = text;
    _hero = now;
    ++_heroVersion;
    info(str(boost::format("net: this side's hero is now %s; sending it to the host") % heroSummary(now)));
    if (_heroFile) {
        if (writeNetHeroFile(*_heroFile, now)) {
            info("net: the hero " + now.name + " is written to " + *_heroFile);
        } else {
            warn("net: couldn't write the hero to " + *_heroFile);
        }
    }
}

void NetSession::sendHeroUpdate() {
    auto out = header(MessageType::HeroUpdate);
    putU32(out, _heroVersion);
    putU16(out, static_cast<uint16_t>(_heroText.size()));
    out.insert(out.end(), _heroText.begin(), _heroText.end());
    _socket.send(_peer, out);
    ++_counts.sent;
}

void NetSession::handleHeroUpdate(const std::vector<uint8_t> &bytes) {
    auto hero = guestHero();
    if (_guestNpc != kNpcSessionHero || !hero) {
        ++_counts.rejected;
        return;
    }
    Reader in(bytes, kHeaderSize);
    uint32_t version = in.u32();
    uint16_t size = in.u16();
    constexpr size_t kTextAt = kHeaderSize + 4 + 2;
    if (!in.ok() || kTextAt + size > bytes.size()) {
        ++_counts.rejected;
        return;
    }
    if (version <= _heroVersionApplied) {
        // Taken already: the guest hasn't heard yet.
        return;
    }
    Character character;
    try {
        character = readNetHero(std::string(bytes.begin() + kTextAt, bytes.begin() + kTextAt + size), _services.game.classes);
    } catch (const std::exception &e) {
        ++_counts.rejected;
        warn(std::string("net: the guest's hero update won't do: ") + e.what());
        _heroVersionApplied = version;
        return;
    }
    // As CharacterGeneration::finish levels up the party leader: the new
    // attributes, and each new level's hit die on the base hit points.
    int gained = 0;
    for (auto &[clazz, levels] : character.attributes.classLevels()) {
        int before = hero->attributes().getClassLevel(clazz->type());
        if (levels > before) {
            gained += (levels - before) * clazz->hitdie();
        }
    }
    hero->attributes() = character.attributes;
    if (gained > 0) {
        hero->setMaxHitPoints(hero->hitPoints() + gained);
    }
    _hero = character;
    _heroVersionApplied = version;
    info("net: the guest's hero is now " + heroSummary(character));
}

void NetSession::handleHeroSpec(const std::vector<uint8_t> &bytes, uint32_t session) {
    if (_guestNpc != kNpcSessionHero) {
        ++_counts.rejected;
        return;
    }
    // handle() took it only from the guest's session now.
    Reader in(bytes, kHeaderSize);
    uint16_t size = in.u16();
    constexpr size_t kTextAt = kHeaderSize + 2;
    if (!in.ok() || kTextAt + size > bytes.size()) {
        ++_counts.rejected;
        return;
    }
    if (_heroId && session == _heroSession) {
        // Made already, for this guest: it hasn't heard.
        sendHeroReady();
        return;
    }
    std::string text(bytes.begin() + kTextAt, bytes.begin() + kTextAt + size);
    Character character;
    try {
        character = readNetHero(text, _services.game.classes);
    } catch (const std::exception &e) {
        ++_counts.rejected;
        if (_refusedHeroSession != session) {
            // Once per guest: it sends the hero again every half second.
            _refusedHeroSession = session;
            warn(std::string("net: the guest's hero won't do: ") + e.what());
        }
        return;
    }
    if (_heroId) {
        // A guest that joined again, maybe with another hero: the earlier
        // one goes, and this one comes at an ID of its own.
        info("net: the guest joined again; its earlier hero, " + (_hero ? _hero->name : std::string("unnamed")) + ", goes");
        removeHero();
        _hero.reset();
    }
    auto pc = _game.party().player();
    auto module = _game.module();
    if (!pc || !module || !module->area()) {
        return;
    }
    // A fresh ID, far from the runtime's and from presentation objects', that
    // neither side has used this session.
    std::shared_ptr<Creature> hero;
    for (int attempt = 0; attempt < 16 && !hero; ++attempt) {
        hero = makeNetHero(_game, kFirstHeroId + (freshNumber() & 0xffffffu), character);
    }
    if (!hero) {
        warn("net: no ID free for the guest's hero");
        return;
    }
    // Beside the PC, a metre to its right, facing as it does.
    float facing = pc->getFacing();
    hero->setPosition(pc->position() + glm::vec3(std::cos(facing), std::sin(facing), 0.0f));
    hero->setFacing(facing);
    module->area()->add(hero);
    module->area()->determineObjectRoom(*hero);
    _hero = character;
    _heroId = hero->id();
    _heroSession = session;
    _heroVersionApplied = 0;
    info(str(boost::format("net: the guest's hero joins: %s, at %.2f %.2f") % heroSummary(character) %
             hero->position().x % hero->position().y));
    sendHeroReady();
}

void NetSession::handleHeroReady(const std::vector<uint8_t> &bytes) {
    if (_guestNpc != kNpcSessionHero || _heroId || !_hero) {
        return;
    }
    // handle() took it only from the host's session now.
    Reader in(bytes, kHeaderSize);
    uint32_t id = in.u32();
    glm::vec3 position;
    position.x = in.f32();
    position.y = in.f32();
    position.z = in.f32();
    float facing = in.f32();
    auto module = _game.module();
    if (!in.ok() || !module || !module->area()) {
        ++_counts.rejected;
        return;
    }
    auto hero = makeNetHero(_game, id, *_hero);
    if (!hero) {
        warn(str(boost::format("net: the host made the hero at ID %d, which this side has used; netoff, and join again") % id));
        stop("the hero's ID is taken here");
        return;
    }
    hero->setPosition(position);
    hero->setFacing(facing);
    module->area()->add(hero);
    module->area()->determineObjectRoom(*hero);
    _heroId = id;
    _heroText = writeNetHero(*_hero);
    _heroVersion = 0;
    _heroVersionAcked = 0;
    _sinceHeroUpdate = 0.0f;
    // This side plays it: the party's leader, for the controls, camera and saber mode.
    auto &party = _game.party();
    party.setLeaderLocked(false);
    party.addSessionHero(hero);
    party.setLeaderLocked(true);
    info("net: playing this side's own hero, " + heroSummary(*_hero));
}

void NetSession::removeHero() {
    if (!_heroId) {
        return;
    }
    auto hero = _game.getObjectById<Creature>(_heroId);
    if (_role == NetRole::Guest) {
        _game.party().removeSessionHero();
    }
    if (hero) {
        if (auto module = _game.module(); module && module->area()) {
            module->area()->destroyObject(*hero);
        }
    }
    // The ID stays session-only: the area destroys the hero later, and until
    // then it is still among its objects. No other object gets the ID again
    // before a load, which forgets it (Game::setSessionOnly).
    _heroId = 0;
}

void NetSession::applyHit(uint32_t targetId, JaBodyRegion region) {
    auto hero = guestHero();
    auto target = _game.getObjectById<Creature>(targetId);
    auto module = _game.module();
    if (!hero || hero->isDead() || !target || target->isDead() || !module || jaSparedBySaber(*target) || !module->isHostileToPartyLeader(*target)) {
        ++_counts.rejected;
        return;
    }
    if (_game.currentScreen() != Game::Screen::InGame || _game.isPaused() || now() < _holdUntil) {
        // Not in play on the host: in a conversation, its menu or paused.
        ++_counts.held;
        return;
    }
    // Saber mode's rule, from the host (JaSaber::strike).
    auto weapon = hero->getEquippedItem(InventorySlots::rightWeapon);
    AttackBuffer attack;
    attack.addContactHit(*hero, *target, weapon.get(), jaSaberHitExtraDamage(*target));
    attack.resolve(*hero, *target);
    attack.signal(_game, _services, *hero, *target);
    ++_counts.hitsApplied;
    info(str(boost::format("net: the guest's %s hit %s (%s): %d hit points left%s") %
             hero->tag() % target->tag() % jaBodyRegionName(region) % target->currentHitPoints() % (target->isDead() ? ", dead" : "")));
}

static int defaultGuestNpc(Game &game) {
    for (auto &member : game.party().members()) {
        if (member.npc != kNpcPlayer) {
            return member.npc;
        }
    }
    throw std::runtime_error("The party has no one but the PC for the guest to play");
}

// The guest's hero as the console names it: a party member's number, "hero"
// for one of the guest's own, or by default the first after the PC.
static int guestNpcArg(Game &game, const ConsoleArgs &args, size_t index) {
    if (args.size() <= index) {
        return defaultGuestNpc(game);
    }
    if (boost::iequals(std::string(*args[index]), "hero")) {
        return kNpcSessionHero;
    }
    return args.get<int>(index).value_or(-99);
}

static ClassType heroClassArg(const std::string &name) {
    static const std::map<std::string, ClassType> kClasses {
        {"soldier", ClassType::Soldier},
        {"scout", ClassType::Scout},
        {"scoundrel", ClassType::Scoundrel},
        {"guardian", ClassType::JediGuardian},
        {"consular", ClassType::JediConsular},
        {"sentinel", ClassType::JediSentinel}};
    auto it = kClasses.find(boost::to_lower_copy(name));
    if (it == kClasses.end()) {
        throw std::runtime_error("Usage: nethero quick <soldier|scout|scoundrel|guardian|consular|sentinel> <male|female> <name>");
    }
    return it->second;
}

void registerNetConsoleCommands(Game &game, IConsole &console) {
    auto print = [&console](const std::string &line) {
        console.printLine(line);
        info(line);
    };
    auto logged = [](const char *name, std::function<void(const ConsoleArgs &)> command) {
        return [name, command](const ConsoleArgs &args) {
            try {
                command(args);
            } catch (const std::exception &e) {
                error(std::string(name) + " failed: " + e.what());
                throw;
            }
        };
    };
    console.registerCommand("nethost", "host a two-player session: nethost [port, 7777] [guest's party member, the first after the PC, or hero for one of its own]",
                            logged("nethost", [&game, print](const ConsoleArgs &args) {
                                if (args.size() > 3) {
                                    throw std::runtime_error("Usage: nethost [port] [npc]");
                                }
                                int port = args.size() > 1 ? args.get<int>(1).value_or(-1) : 7777;
                                if (port < 0 || port > 65535) {
                                    throw std::runtime_error("Usage: nethost [port] [npc]");
                                }
                                int npc = guestNpcArg(game, args, 2);
                                game.netSession().host(static_cast<uint16_t>(port), npc);
                                print(game.netSession().status());
                            }));
    console.registerCommand("netjoin", "join a two-player session: netjoin <host> [port, 7777] [party member to play, the first after the PC, or hero for nethero's]",
                            logged("netjoin", [&game, print](const ConsoleArgs &args) {
                                if (args.size() < 2 || args.size() > 4) {
                                    throw std::runtime_error("Usage: netjoin <host> [port] [npc]");
                                }
                                int port = args.size() > 2 ? args.get<int>(2).value_or(-1) : 7777;
                                if (port <= 0 || port > 65535) {
                                    throw std::runtime_error("Usage: netjoin <host> [port] [npc]");
                                }
                                int npc = guestNpcArg(game, args, 3);
                                game.netSession().join(std::string(*args[1]), static_cast<uint16_t>(port), npc);
                                print(game.netSession().status());
                            }));
    console.registerCommand("nethero", "the hero this side brings to a session: nethero [<file> | quick <class> <male|female> <name> | save <file>]",
                            logged("nethero", [&game, print](const ConsoleArgs &args) {
                                auto &session = game.netSession();
                                if (args.size() == 1) {
                                    print(session.hero() ? "nethero: " + heroSummary(*session.hero()) : std::string("nethero: none"));
                                    return;
                                }
                                std::string first(*args[1]);
                                if (boost::iequals(first, "quick")) {
                                    if (args.size() < 5) {
                                        throw std::runtime_error("Usage: nethero quick <class> <male|female> <name>");
                                    }
                                    std::string gender(boost::to_lower_copy(std::string(*args[3])));
                                    if (gender != "male" && gender != "female") {
                                        throw std::runtime_error("Usage: nethero quick <class> <male|female> <name>");
                                    }
                                    std::string name(*args[4]);
                                    for (size_t i = 5; i < args.size(); ++i) {
                                        name += " " + std::string(*args[i]);
                                    }
                                    session.setHero(quickNetHero(session.services(), heroClassArg(std::string(*args[2])),
                                                                 gender == "female" ? Gender::Female : Gender::Male, name));
                                } else if (boost::iequals(first, "save")) {
                                    if (args.size() != 3 || !session.hero()) {
                                        throw std::runtime_error("Usage: nethero save <file>, with a hero set");
                                    }
                                    std::ofstream out(std::string(*args[2]), std::ios::binary);
                                    out << writeNetHero(*session.hero());
                                    if (!out) {
                                        throw std::runtime_error("Couldn't write " + std::string(*args[2]));
                                    }
                                } else {
                                    if (args.size() != 2) {
                                        throw std::runtime_error("Usage: nethero <file>");
                                    }
                                    std::ifstream in(first, std::ios::binary);
                                    if (!in) {
                                        throw std::runtime_error("No hero file " + first);
                                    }
                                    std::stringstream text;
                                    text << in.rdbuf();
                                    session.setHero(readNetHero(text.str(), session.services().game.classes), first);
                                }
                                print("nethero: " + heroSummary(*session.hero()));
                            }));
    console.registerCommand("netheromake", "make a two-player session's hero with KOTOR's character creation, from the main menu: netheromake <file>",
                            logged("netheromake", [&game](const ConsoleArgs &args) {
                                if (args.size() != 2) {
                                    throw std::runtime_error("Usage: netheromake <file>");
                                }
                                game.makeSessionHero(std::string(*args[1]));
                            }));
    console.registerCommand("netwait", "hold a command file until the other side, and with hero the guest's hero, is here: netwait [seconds, 60]",
                            logged("netwait", [&game](const ConsoleArgs &args) {
                                if (args.size() > 2) {
                                    throw std::runtime_error("Usage: netwait [seconds]");
                                }
                                int seconds = args.size() > 1 ? args.get<int>(1).value_or(-1) : 60;
                                if (seconds < 1) {
                                    throw std::runtime_error("Usage: netwait [seconds], where seconds is positive");
                                }
                                game.netSession().waitForPeer(static_cast<float>(seconds));
                            }));
    console.registerCommand("netstatus", "log the two-player session's state and counts: netstatus",
                            logged("netstatus", [&game, print](const ConsoleArgs &) {
                                print(game.netSession().status());
                            }));
    console.registerCommand("netoff", "end the two-player session: netoff",
                            logged("netoff", [&game, print](const ConsoleArgs &) {
                                game.netSession().stop("ended from the console");
                                print("net: off");
                            }));
}

std::string NetSession::status() const {
    std::string match = _manifestsMatch ? (*_manifestsMatch ? "match" : "DIFFER") : "unknown";
    // Where the two heroes stand here, for comparing the two sides' logs.
    auto where = [](const std::shared_ptr<Creature> &creature) {
        if (!creature) {
            return std::string("none");
        }
        return str(boost::format("%s at %.2f %.2f") % creature->tag() % creature->position().x % creature->position().y);
    };
    return str(boost::format("net: %s, %s%s, guest playing party member %d; objects %s; PC %s, guest's hero %s; "
                             "%d sent, %d received, %d applied, %d actions cleared, %d hits sent, %d hits applied, %d rejected, "
                             "%d unknown, %d stale, %d held, %d receive errors") %
               roleName(_role) % (_connected ? "connected to " : "waiting for ") %
               (_peer.port ? _peer.str() : std::string("a guest")) % _guestNpc % match %
               where(_game.party().player()) % where(guestHero()) %
               _counts.sent % _counts.received % _counts.applied % _counts.clearedActions % _counts.hitsForwarded %
               _counts.hitsApplied % _counts.rejected % _counts.unknown % _counts.stale % _counts.held % _socket.errors());
}

} // namespace game

} // namespace reone
