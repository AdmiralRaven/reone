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
#include "reone/game/effect/damage.h"
#include "reone/game/game.h"
#include "reone/game/object/area.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/item.h"
#include "reone/game/object/module.h"
#include "reone/game/party.h"
#include "reone/system/logutil.h"

#include <chrono>

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
constexpr size_t kHeaderSize = 4 + 2 + 1 + 4;
constexpr float kHelloInterval = 0.5f;
constexpr float kManifestInterval = 2.0f;
constexpr double kHoldAfterConversation = 1.0;
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

} // namespace

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
    for (auto &member : _game.party().members()) {
        if (member.npc == _guestNpc) {
            return member.creature;
        }
    }
    return nullptr;
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
    if (!guestHero()) {
        throw std::runtime_error("No party member " + std::to_string(guestNpc) + " for the guest to play");
    }
    _socket.open(port);
    _role = NetRole::Host;
    _lastHeard = now();
    // The host plays the PC, whichever member the save had leading.
    _game.party().setPartyLeader(kNpcPlayer);
    _game.party().setLeaderLocked(true);
    info(str(boost::format("net: hosting on UDP port %d for a guest playing party member %d (%s); objects %016x") %
             _socket.localPort() % guestNpc % guestHero()->tag() % manifestHash()));
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
    auto hero = guestHero();
    if (!hero) {
        throw std::runtime_error("No party member " + std::to_string(guestNpc) + " to play");
    }
    _socket.open(0);
    _peer = *address;
    _role = NetRole::Guest;
    _lastHeard = now();
    // The guest's controls, camera and saber mode drive its hero.
    _game.party().setPartyLeader(guestNpc);
    _game.party().setLeaderLocked(true);
    _game.setFollowingHost(true);
    info(str(boost::format("net: joining %s as party member %d (%s); objects %016x") %
             _peer.str() % guestNpc % hero->tag() % manifestHash()));
    sendHello();
}

void NetSession::stop(const std::string &reason) {
    if (_role == NetRole::None) {
        return;
    }
    _game.party().setLeaderLocked(false);
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
    _refused = NetSocket::Address();
    _sequence = 0;
    _lastReceived = 0;
    _sinceSend = 0.0f;
    _sinceHello = 0.0f;
    _lastUpdate = 0.0;
    _holding = false;
    _holdUntil = 0.0;
    _warnedNoHero = false;
    _unknownIds.clear();
    _counts = Counts();
}

uint64_t NetSession::manifestHash() const {
    uint64_t hash = 1469598103934665603ull;
    auto module = _game.module();
    if (!module || !module->area()) {
        return hash;
    }
    std::vector<std::tuple<uint32_t, int, std::string>> objects;
    for (auto &object : module->area()->objects()) {
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
    return out;
}

void NetSession::sendHello() {
    auto out = header(MessageType::Hello);
    putU8(out, static_cast<uint8_t>(_role));
    putU32(out, static_cast<uint32_t>(_guestNpc));
    putU64(out, manifestHash());
    auto module = _game.module();
    putU32(out, module && module->area() ? static_cast<uint32_t>(module->area()->objects().size()) : 0);
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
    size_t perDatagram = (kMaxDatagram - kHeaderSize - 2) / 24;
    for (size_t first = 0; first < states.size() || first == 0; first += perDatagram) {
        auto out = header(MessageType::Snapshot);
        size_t count = std::min(perDatagram, states.size() - std::min(first, states.size()));
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
        handle(*datagram);
    }

    if (_connected && time - _lastHeard > kTimeout) {
        info(str(boost::format("net: nothing from %s for %.0f s") % _peer.str() % kTimeout));
        _connected = false;
    }

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
    if (protocol != kProtocol) {
        ++_counts.rejected;
        return;
    }
    // A hello to the host from elsewhere: a guest, new or come back. It is
    // taken only once it checks out, so that a stray one cuts no one off.
    bool newGuest = _role == NetRole::Host && type == MessageType::Hello && datagram.from != _peer;
    if (datagram.from != _peer && !newGuest) {
        ++_counts.rejected;
        return;
    }
    if (!newGuest) {
        ++_counts.received;
        _lastHeard = now();
    }

    switch (type) {
    case MessageType::Hello: {
        auto role = static_cast<NetRole>(in.u8());
        int npc = static_cast<int>(in.u32());
        uint64_t manifest = in.u64();
        uint32_t objects = in.u32();
        if (!in.ok()) {
            ++_counts.rejected;
            return;
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
        if (newGuest) {
            _peer = datagram.from;
            _lastReceived = 0;
            _connected = false;
            ++_counts.received;
            _lastHeard = now();
        }
        uint64_t mine = manifestHash();
        bool match = manifest == mine;
        bool first = !_connected;
        bool changed = _manifestsMatch && *_manifestsMatch != match;
        _manifestsMatch = match;
        _connected = true;
        if (_role == NetRole::Guest && sequence < _lastReceived) {
            // A host that started over counts from 1 again.
            _lastReceived = 0;
        }
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
        uint16_t count = in.u16();
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
        if (_role != NetRole::Host || sequence < _lastReceived) {
            return;
        }
        _lastReceived = sequence;
        auto state = readState(in);
        auto hero = guestHero();
        if (in.ok() && !hero && !_warnedNoHero) {
            _warnedNoHero = true;
            warn(str(boost::format("net: party member %d, the guest's hero, isn't in the host's party now") % _guestNpc));
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

void NetSession::applyHit(uint32_t targetId, JaBodyRegion region) {
    auto hero = guestHero();
    auto target = _game.getObjectById<Creature>(targetId);
    auto module = _game.module();
    if (!hero || hero->isDead() || !target || target->isDead() || !module || jaSparedBySaber(*target) || !module->isHostileToPartyLeader(*target)) {
        ++_counts.rejected;
        return;
    }
    if (_game.currentScreen() != Game::Screen::InGame || now() < _holdUntil) {
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
    console.registerCommand("nethost", "host a two-player session: nethost [port, 7777] [guest's party member, the first after the PC]",
                            logged("nethost", [&game, print](const ConsoleArgs &args) {
                                if (args.size() > 3) {
                                    throw std::runtime_error("Usage: nethost [port] [npc]");
                                }
                                int port = args.size() > 1 ? args.get<int>(1).value_or(-1) : 7777;
                                if (port < 0 || port > 65535) {
                                    throw std::runtime_error("Usage: nethost [port] [npc]");
                                }
                                int npc = args.size() > 2 ? args.get<int>(2).value_or(-99) : defaultGuestNpc(game);
                                game.netSession().host(static_cast<uint16_t>(port), npc);
                                print(game.netSession().status());
                            }));
    console.registerCommand("netjoin", "join a two-player session: netjoin <host> [port, 7777] [party member to play, the first after the PC]",
                            logged("netjoin", [&game, print](const ConsoleArgs &args) {
                                if (args.size() < 2 || args.size() > 4) {
                                    throw std::runtime_error("Usage: netjoin <host> [port] [npc]");
                                }
                                int port = args.size() > 2 ? args.get<int>(2).value_or(-1) : 7777;
                                if (port <= 0 || port > 65535) {
                                    throw std::runtime_error("Usage: netjoin <host> [port] [npc]");
                                }
                                int npc = args.size() > 3 ? args.get<int>(3).value_or(-99) : defaultGuestNpc(game);
                                game.netSession().join(std::string(*args[1]), static_cast<uint16_t>(port), npc);
                                print(game.netSession().status());
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
