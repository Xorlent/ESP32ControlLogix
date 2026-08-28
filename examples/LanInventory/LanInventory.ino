/*
 * LanInventory - scan the local subnet for EtherNet/IP devices (port 44818) and
 * report each device's ControlLogix hardware, firmware, and run-switch status.
 *
 * The network address (e.g. 192.168.1.0), broadcast address
 * (e.g. 192.168.1.255), the configured default gateway, and this device's
 * own address are skipped; only valid host addresses are probed.
 *
 * Limitation: the scan probes every host address sequentially, based on the configured
 * subnet mask (e.g.: /24 = 254 addresses, /22 = 1022 addresses)
 *
 * For every device that accepts a TCP connection on 44818, the sketch:
 *   1. registers an EtherNet/IP session;
 *   2. reads the CIP Identity Object (vendor, device type, product code,
 *      firmware revision, status, serial, product name, state);
 *   3. derives the run-switch position from the Identity status word
 *      (low byte = keyswitch, high byte = remote) rather than a tag;
 *   4. reads the device state from the ListIdentity (UDP) response, since the
 *      Identity State attribute (attr 8) is not exposed by Get Attribute Single;
 *   5. prints the results and closes the session.
 *
 */

#include <ESP32ControlLogix.h>

#include <lwip/inet.h>
#include <lwip/sockets.h>

const IPAddress LOCAL_IP(192, 168, 1, 50);
const IPAddress LOCAL_GATEWAY(192, 168, 1, 1);
const IPAddress LOCAL_SUBNET(255, 255, 255, 0);
const IPAddress LOCAL_DNS(192, 168, 1, 1);

constexpr uint16_t EIP_PORT = 44818;
// Per-address TCP connect deadline. TcpConnection uses a non-blocking connect
// and aborts at this deadline, so this bounds how long each address is probed.
// (LwIP's own SYN-retransmit timeout is much longer and would dominate a
// blocking connect.) Lower = faster scan, but may miss slow/busy devices.
constexpr uint32_t SCAN_TIMEOUT_MS = 100;
constexpr uint32_t SESSION_TIMEOUT_MS = 1000;
constexpr uint32_t MESSAGE_TIMEOUT_MS = 1000;

clx::Client eth;
clx::Client::Config cfg;
clx::TcpConnection tcp;
clx::Session session;
clx::ExplicitMessage msg;

struct Identity {
    uint16_t vendor = 0;
    uint16_t deviceType = 0;
    uint16_t productCode = 0;
    uint8_t revMajor = 0;
    uint8_t revMinor = 0;
    uint16_t status = 0;
    uint32_t serial = 0;
    char productName[64] = {};
    uint8_t state = 0xFF;  // 0xFF = unknown (Identity attr 8 is best-effort)
};

const uint8_t kIdentityAttrs[] = {
    clx::kIdentityVendorId, clx::kIdentityDeviceType, clx::kIdentityProductCode,
    clx::kIdentityRevision, clx::kIdentityStatus, clx::kIdentitySerialNumber,
    clx::kIdentityProductName, clx::kIdentityState,
};
constexpr size_t kIdentityAttrCount = sizeof(kIdentityAttrs) / sizeof(kIdentityAttrs[0]);

enum class Phase : uint8_t {
    WaitEthernet, Scan, Connect, Register, Query, Close, Done,
};

Phase phase = Phase::WaitEthernet;
uint32_t phaseStarted = 0;
IPAddress network;      // network address (e.g. 192.168.1.0)
IPAddress broadcast;    // broadcast address (e.g. 192.168.1.255)
IPAddress currentIp;    // current probe target
int found = 0;
int queryIndex = 0;     // index into kIdentityAttrs[]
bool stepStarted = false;
Identity id;

const char *identityState(uint8_t state) {
    switch (state) {
        case 0: return "nonexistent/unknown";
        case 1: return "self-testing";
        case 2: return "standby";
        case 3: return "operational";
        case 4: return "major recoverable fault";
        case 5: return "major unrecoverable fault";
        case 6: return "communication fault";
        case 7: return "unconfigured";
        default: return "unknown";
    }
}

// Identity status word keyswitch encodings (low byte = mode, high byte = remote).
const uint8_t KEYSWITCH_RUN      = 0x60;
const uint8_t KEYSWITCH_PROG     = 0x70;
const uint8_t KEYSWITCH_REMOTE_0 = 0x30;
const uint8_t KEYSWITCH_REMOTE_1 = 0x31;

// Derive the run-switch (keyswitch) text from the Identity status word.
const char *keyswitchName(uint16_t statusWord) {
    uint8_t s0 = statusWord & 0xFF;          // keyswitch mode byte
    uint8_t s1 = (statusWord >> 8) & 0xFF;   // remote/other byte
    bool remote = (s1 == KEYSWITCH_REMOTE_0 || s1 == KEYSWITCH_REMOTE_1);
    if (s0 == KEYSWITCH_RUN)  return remote ? "REMOTE RUN"  : "RUN";
    if (s0 == KEYSWITCH_PROG) return remote ? "REMOTE PROG" : "PROG";
    return "UNKNOWN";
}

// Advance an IPv4 address to the next host address (increments the last octet,
// carrying into higher octets). Returns false on overflow past 255.255.255.255.
bool nextHost(IPAddress &ip) {
    for (int i = 3; i >= 0; --i) {
        if (ip[i] < 255) {
            ++ip[i];
            return true;
        }
        ip[i] = 0;
    }
    return false;
}

// Query the device "state" via the ListIdentity (0x0063) message over UDP.
// Many Logix CPUs do not expose the Identity object's State attribute (attr 8)
// to a Get Attribute Single read, but the ListIdentity response always carries
// a one-byte state field after the product name. Returns the state, or 0xFF if
// no valid response arrives within the bounded wait.
uint8_t queryListIdentityState(const IPAddress &ip) {
    int fd = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return 0xFF;

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000;  // ~500 ms bounded wait
    lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Bind the EtherNet/IP UDP port so the reply is routed back to us.
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(44818);
    local.sin_addr.s_addr = 0;  // INADDR_ANY
    if (lwip_bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0) {
        lwip_close(fd);
        return 0xFF;
    }

    // ListIdentity request: 24-byte header, command 0x0063, no body.
    clx::EncapsulationHeader h;
    h.command = static_cast<uint16_t>(clx::Command::ListIdentity);
    uint8_t req[clx::kEncapsulationHeaderSize];
    clx::encodeHeader(req, h);

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(44818);
    dst.sin_addr.s_addr = ip;  // IPAddress converts to network byte order

    lwip_sendto(fd, req, sizeof(req), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));

    uint8_t state = 0xFF;
    uint8_t buf[512];
    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);
    int n = lwip_recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &fromLen);

    // Layout: 24-byte header + item count (2) then items. Each item is
    // type(2) + length(2) + data(...). The Identity item's data lays out:
    //  ...zero(8) IP@6 vendor@18 devType@20 prodCode@22 rev@24 status@26
    //  serial@28 nameLen@32 name@33 state@(33+nameLen)
    if (n >= 26 &&
        clx::decodeHeader(buf).command == static_cast<uint16_t>(clx::Command::ListIdentity)) {
        uint16_t itemCount = clx::getU16(buf + 24);
        int off = 26;
        for (uint16_t i = 0; i < itemCount && off + 4 <= n; ++i) {
            uint16_t type = clx::getU16(buf + off);
            uint16_t ilen = clx::getU16(buf + off + 2);
            off += 4;
            if (off + ilen > n) break;
            if (type == 0x000C && ilen >= 33) {  // Identity item (0x0C)
                uint8_t nameLen = buf[off + 32];
                if (33 + nameLen < ilen) {
                    state = buf[off + 33 + nameLen];
                }
            }
            off += ilen;
        }
    }

    lwip_close(fd);
    return state;
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.printf("ESP32ControlLogix %s - LAN inventory\n", clx::version());

    cfg.ip = LOCAL_IP;
    cfg.gateway = LOCAL_GATEWAY;
    cfg.subnet = LOCAL_SUBNET;
    cfg.dns = LOCAL_DNS;

    // Ethernet PHY configuration.
    // The defaults below match the M5Stack AtomS3 + AtomPoE (W5500) SPI PHY.
    // To use different SPI pins, uncomment and adjust:
    //   cfg.sck = 5;     // SPI clock
    //   cfg.miso = 7;    // SPI MISO
    //   cfg.mosi = 8;    // SPI MOSI
    //   cfg.cs = 6;      // chip select (SPI PHYs)
    //
    // To use an RMII PHY (e.g. LAN8720) on a board with the built-in EMAC,
    // uncomment and adjust these instead of the SPI defaults:
    //   cfg.phyType = ETH_PHY_LAN8720;
    //   cfg.rmii = true;
    //   cfg.phyAddr = 0;
    //   cfg.mdc = 23;
    //   cfg.mdio = 18;
    //   cfg.power = -1;
    //   cfg.clkMode = ETH_CLOCK_GPIO0_IN;  // 0

    clx::Status st = eth.begin(cfg);
    Serial.printf("Client::begin() -> %s\n", clx::statusString(st));
    if (st == clx::Status::Error) {
        phase = Phase::Done;
    }
    phaseStarted = millis();
}

void startQuery() {
    uint8_t attr = kIdentityAttrs[queryIndex];
    // Get Attribute Single path = class + instance + attribute (3 words). The
    // attribute is a path segment, not service data, so appending it to the
    // path keeps the path-size word correct.
    uint8_t path[6];
    size_t pl = 0;
    pl += clx::appendClass(path + pl, 1);          // class = Identity (1)
    pl += clx::appendInstance(path + pl, 1);       // instance = 1
    pl += clx::appendAttribute(path + pl, attr);   // attribute segment
    // Route through the backplane to the CPU (slot 0). When connected to a
    // ControlLogix Ethernet module, the Identity object at class 1/instance 1
    // belongs to the module itself; the CPU's identity lives in slot 0.
    clx::Status st = msg.sendRouted(tcp, session.handle(), 0,
                                    static_cast<uint8_t>(clx::Service::GetAttributeSingle),
                                    path, pl, nullptr, 0, MESSAGE_TIMEOUT_MS);
    stepStarted = (st == clx::Status::Pending);
}

void finishQuery() {
    // A failed attribute read leaves the field at its default (state stays 0xFF).
    if (queryIndex < int(kIdentityAttrCount) && msg.resultCode() == 0) {
        uint8_t attr = kIdentityAttrs[queryIndex];
        const uint8_t *d = msg.data();
        size_t n = msg.dataLength();
        switch (attr) {
            case clx::kIdentityVendorId:     if (n >= 2) id.vendor = clx::getU16(d); break;
            case clx::kIdentityDeviceType:   if (n >= 2) id.deviceType = clx::getU16(d); break;
            case clx::kIdentityProductCode:  if (n >= 2) id.productCode = clx::getU16(d); break;
            case clx::kIdentityRevision:     if (n >= 2) { id.revMajor = d[0]; id.revMinor = d[1]; } break;
            case clx::kIdentityStatus:       if (n >= 2) id.status = clx::getU16(d); break;
            case clx::kIdentitySerialNumber: if (n >= 4) id.serial = clx::getU32(d); break;
            case clx::kIdentityProductName:
                if (n > 0) { size_t c = n < sizeof(id.productName) - 1 ? n : sizeof(id.productName) - 1; memcpy(id.productName, d, c); id.productName[c] = 0; }
                break;
            case clx::kIdentityState:        if (n >= 1) id.state = d[0]; break;
        }
    }
    ++queryIndex;
    stepStarted = false;
    if (queryIndex >= int(kIdentityAttrCount)) {
        // The Identity State attribute (attr 8) is often not exposed by Get
        // Attribute Single on Logix CPUs; fall back to the ListIdentity (UDP)
        // response, which always carries the one-byte state field.
        uint8_t st = queryListIdentityState(currentIp);
        if (st != 0xFF) id.state = st;
        printDevice();
        session.close();
        phase = Phase::Close;
        phaseStarted = millis();
    }
}

void printDevice() {
    Serial.printf("DEVICE %s\n", currentIp.toString().c_str());
    Serial.printf("  Hardware: vendor=%u device_type=%u product_code=%u serial=0x%08lX name=%s\n",
                  id.vendor, id.deviceType, id.productCode, (unsigned long)id.serial,
                  id.productName[0] ? id.productName : "<not reported>");
    Serial.printf("  Firmware: %u.%u\n", id.revMajor, id.revMinor);
    Serial.printf("  Identity status: 0x%04X\n", id.status);
    Serial.printf("  Device state: %u (%s)\n", id.state, identityState(id.state));
    Serial.printf("  RUN switch: %s\n", keyswitchName(id.status));
}

void loop() {
    switch (phase) {
        case Phase::WaitEthernet: {
            clx::Status st = eth.poll();
            if (st == clx::Status::Ok) {
                IPAddress ip = eth.localIP();
                IPAddress mask = eth.subnetMask();
                for (int i = 0; i < 4; ++i) {
                    network[i] = ip[i] & mask[i];
                    broadcast[i] = ip[i] | uint8_t(~mask[i]);
                }
                Serial.printf("Scanning %s for TCP port %u (excluding network %s, broadcast %s, gateway %s, and self %s)...\n",
                              mask.toString().c_str(), EIP_PORT,
                              network.toString().c_str(), broadcast.toString().c_str(),
                              cfg.gateway.toString().c_str(), ip.toString().c_str());
                currentIp = network;  // nextHost() advances to network+1 in Scan
                phase = Phase::Scan;
                phaseStarted = millis();
            } else if (st == clx::Status::Error) {
                Serial.println("Ethernet failed.");
                phase = Phase::Done;
            }
            break;
        }
        case Phase::Scan: {
            // Advance to the next host address. Stop once we reach the
            // broadcast address (or overflow), which is not a valid host.
            if (!nextHost(currentIp) || currentIp == broadcast) {
                Serial.printf("LAN INVENTORY COMPLETE: %d device(s) responded.\n", found);
                phase = Phase::Done;
                break;
            }
            if (currentIp == cfg.gateway || currentIp == eth.localIP()) {
                break;  // skip the gateway and our own address; advance next loop
            }
            tcp.connect(currentIp, EIP_PORT, SCAN_TIMEOUT_MS);
            phase = Phase::Connect;
            phaseStarted = millis();
            break;
        }
        case Phase::Connect: {
            clx::Status st = tcp.poll();
            if (st == clx::Status::Ok) {
                // A device is listening; register a session.
                session.open(tcp, SESSION_TIMEOUT_MS);
                phase = Phase::Register;
                phaseStarted = millis();
            } else if (st == clx::Status::Timeout || st == clx::Status::Error || st == clx::Status::Closed) {
                // No device (or not EtherNet/IP); move on.
                tcp.close();
                phase = Phase::Scan;
            }
            break;
        }
        case Phase::Register: {
            clx::Status st = session.poll();
            if (st == clx::Status::Ok) {
                id = Identity{};
                queryIndex = 0;
                stepStarted = false;
                phase = Phase::Query;
                phaseStarted = millis();
            } else if (st == clx::Status::Timeout || st == clx::Status::Error) {
                session.abort();
                tcp.close();
                phase = Phase::Scan;
            }
            break;
        }
        case Phase::Query: {
            if (!stepStarted) {
                startQuery();
            } else {
                clx::Status st = msg.poll();
                if (st == clx::Status::Ok) {
                    finishQuery();
                } else if (st == clx::Status::Timeout || st == clx::Status::Error || st == clx::Status::Closed) {
                    // Identity query failed; skip to the next device.
                    session.abort();
                    tcp.close();
                    phase = Phase::Scan;
                }
            }
            break;
        }
        case Phase::Close: {
            clx::Status st = session.poll();
            if (st == clx::Status::Ok || st == clx::Status::Closed) {
                tcp.close();
                ++found;
                phase = Phase::Scan;
            } else if (st == clx::Status::Timeout || st == clx::Status::Error) {
                session.abort();
                tcp.close();
                ++found;
                phase = Phase::Scan;
            }
            break;
        }
        case Phase::Done:
        default:
            delay(1000);
            break;
    }
    delay(1);
}

