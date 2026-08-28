/*
 * ListTags - enumerate every tag in a Logix controller.
 *
 * Walks the CIP Symbol Object (class 0x6B) with the Rockwell "Get Instance
 * Attribute List" service (0x55) to discover all controller-scope tags, then
 * reads the value of each elementary (atomic, scalar) tag via Read Tag (0x4C).
 *
 * The Symbol Object and tags live in the CPU, so requests are routed through
 * the backplane to slot 0 (Unconnected Send) -- the same technique the
 * Identity query demo uses.
 *
 * Point TARGET_IP/TARGET_PORT at the PLC (EtherNet/IP port 44818). For a
 * ControlLogix (1756 chassis) the CPU is expected in slot 0; change
 * CPU_SLOT below if yours is elsewhere.
 */

#include <ESP32ControlLogix.h>

const IPAddress LOCAL_IP(192, 168, 1, 50);
const IPAddress LOCAL_GATEWAY(192, 168, 1, 1);
const IPAddress LOCAL_SUBNET(255, 255, 255, 0);
const IPAddress LOCAL_DNS(192, 168, 1, 1);

const IPAddress TARGET_IP(192, 168, 1, 20);
constexpr uint16_t TARGET_PORT = 44818;
constexpr uint8_t  CPU_SLOT = 0;              // backplane slot of the CPU

constexpr uint32_t CONNECT_TIMEOUT_MS = 5000;
constexpr uint32_t SESSION_TIMEOUT_MS = 5000;
constexpr uint32_t MESSAGE_TIMEOUT_MS = 5000;

clx::Client eth;
clx::Client::Config cfg;
clx::TcpConnection conn;
clx::Session session;
clx::ExplicitMessage msg;

constexpr size_t kMaxTags = 200;
struct TagEntry {
    char name[46];
    uint16_t symbolType;  // bit 15 struct, bits 14-13 dims, low byte type code
};
TagEntry tags[kMaxTags];
size_t tagCount = 0;

uint32_t nextInstance = 0;
bool enumerating = false;
size_t valueCursor = 0;

enum class Phase : uint8_t { WaitEthernet, Connect, OpenSession, Enumerate, ReadValues, Done };
Phase phase = Phase::WaitEthernet;
uint32_t phaseStarted = 0;

// -- helpers ---------------------------------------------------------------

// Build a Symbol Object path (class 0x6B + instance segment), padded to a
// whole 16-bit word. Returns bytes written.
size_t buildSymbolPath(uint8_t *path, uint32_t instance) {
    size_t pl = clx::appendClass(path, clx::kSymbolClass);          // class 0x6B
    if (instance <= 0xFF) {
        return pl + clx::appendInstance(path + pl, uint8_t(instance));
    } else if (instance <= 0xFFFF) {
        return pl + clx::appendInstance16(path + pl, uint16_t(instance));
    } else {
        return pl + clx::appendInstance32(path + pl, instance);
    }
}

// Start a "Get Instance Attribute List" request for the given starting
// instance, asking for attributes 1 (name) and 2 (symbol type).
clx::Status startEnumerate(uint32_t startInstance) {
    uint8_t path[8];
    size_t pl = buildSymbolPath(path, startInstance);

    uint8_t data[6];
    clx::putU16(data, 2);                        // number of attributes
    clx::putU16(data + 2, clx::kSymbolName);     // attr 1 = name
    clx::putU16(data + 4, clx::kSymbolType);     // attr 2 = symbol type

    return msg.sendRouted(conn, session.handle(), CPU_SLOT,
                          static_cast<uint8_t>(clx::Service::GetInstanceAttributeList),
                          path, pl, data, sizeof(data), MESSAGE_TIMEOUT_MS);
}

// Start a Read Tag request for the named tag (1 element), routed to the CPU.
clx::Status startReadTag(const char *name) {
    uint8_t path[64];
    size_t pl = clx::appendSymbolic(path, name);
    uint8_t count[2];
    clx::putU16(count, 1);  // one element
    return msg.sendRouted(conn, session.handle(), CPU_SLOT,
                          static_cast<uint8_t>(clx::TagService::Read),
                          path, pl, count, sizeof(count), MESSAGE_TIMEOUT_MS);
}

// Decode a Logix STRING (4-byte length + bytes); returns length.
uint32_t readLogixString(const uint8_t *p, char *out, size_t outCap) {
    uint32_t len = clx::getU32(p);
    if (len > outCap - 1) len = outCap - 1;
    memcpy(out, p + 4, len);
    out[len] = 0;
    return len;
}

const char *typeName(uint16_t code) {
    return clx::dataTypeName(static_cast<clx::DataType>(code & 0xFF));
}

void printValue(const uint8_t *data, size_t len) {
    if (len < 2) return;
    uint16_t code = clx::getU16(data);
    const uint8_t *v = data + 2;
    size_t n = len - 2;
    switch (code & 0xFF) {
        case 0xC1: Serial.print(n >= 1 ? (v[0] ? "TRUE" : "FALSE") : "?"); break;
        case 0xC2: Serial.print(n >= 1 ? int(int8_t(v[0])) : 0); break;
        case 0xC3: Serial.print(n >= 2 ? int16_t(clx::getU16(v)) : 0); break;
        case 0xC4: Serial.print(n >= 4 ? int32_t(clx::getU32(v)) : 0); break;
        case 0xC5: Serial.print(n >= 8 ? int64_t(clx::getU64(v)) : 0); break;
        case 0xC6: Serial.print(n >= 1 ? v[0] : 0); break;
        case 0xC7: Serial.print(n >= 2 ? clx::getU16(v) : 0); break;
        case 0xC8: Serial.print(n >= 4 ? clx::getU32(v) : 0); break;
        case 0xC9: Serial.print(n >= 8 ? clx::getU64(v) : 0); break;
        case 0xCA: { float f; if (n >= 4) memcpy(&f, v, 4); else f = 0; Serial.print(f, 6); break; }
        case 0xCB: { double d; if (n >= 8) memcpy(&d, v, 8); else d = 0; Serial.print(d, 6); break; }
        case 0xD0: { char s[46]; if (n >= 4) readLogixString(v, s, sizeof(s)); else s[0] = 0; Serial.print('"'); Serial.print(s); Serial.print('"'); break; }
        default: Serial.print("(unhandled)"); break;
    }
}

void printTag(const TagEntry &t) {
    bool isStruct = (t.symbolType & 0x8000) != 0;
    uint8_t dims = (t.symbolType >> 13) & 0x03;
    Serial.printf("  %-45s", t.name);
    if (isStruct) {
        Serial.print("[struct]");
    } else {
        Serial.print(typeName(t.symbolType));
        if (dims) Serial.printf("[%uD]", dims);
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.printf("ESP32ControlLogix %s - Logix tag inventory demo\n", clx::version());

    cfg.ip = LOCAL_IP;
    cfg.gateway = LOCAL_GATEWAY;
    cfg.subnet = LOCAL_SUBNET;
    cfg.dns = LOCAL_DNS;
    // Adjust PHY pins here if not using the W5500 defaults.

    clx::Status st = eth.begin(cfg);
    Serial.printf("Client::begin() -> %s\n", clx::statusString(st));
    phaseStarted = millis();
}

void loop() {
    switch (phase) {
        case Phase::WaitEthernet: {
            clx::Status st = eth.poll();
            if (st == clx::Status::Ok) {
                Serial.printf("Ethernet ready: ip=%s\n", eth.localIP().toString().c_str());
                Serial.printf("Connecting to %s:%u ...\n", TARGET_IP.toString().c_str(), TARGET_PORT);
                conn.connect(TARGET_IP, TARGET_PORT, CONNECT_TIMEOUT_MS);
                phase = Phase::Connect;
            } else if (st == clx::Status::Error) {
                eth.begin(cfg);
            }
            break;
        }
        case Phase::Connect: {
            clx::Status st = conn.poll();
            if (st == clx::Status::Ok) {
                session.open(conn, SESSION_TIMEOUT_MS);
                phase = Phase::OpenSession;
            } else if (st == clx::Status::Timeout || st == clx::Status::Error) {
                Serial.printf("TCP connect failed: %s\n", clx::statusString(st));
                phase = Phase::Done;
            }
            break;
        }
        case Phase::OpenSession: {
            clx::Status st = session.poll();
            if (st == clx::Status::Ok) {
                Serial.println("Session registered; enumerating tags...");
                nextInstance = 0;
                tagCount = 0;
                enumerating = false;
                phase = Phase::Enumerate;
            } else if (st == clx::Status::Timeout || st == clx::Status::Error) {
                Serial.printf("RegisterSession failed: %s\n", clx::statusString(st));
                phase = Phase::Done;
            }
            break;
        }
        case Phase::Enumerate: {
            if (!enumerating) {
                clx::Status st = startEnumerate(nextInstance);
                if (st == clx::Status::Pending) {
                    enumerating = true;
                } else {
                    Serial.printf("enumerate failed to start: %s\n", clx::statusString(st));
                    phase = Phase::Done;
                }
                break;
            }
            clx::Status st = msg.poll();
            if (st == clx::Status::Pending) break;
            if (st != clx::Status::Ok) {
                Serial.printf("enumerate failed: %s (CIP 0x%02X)\n",
                              clx::statusString(st), msg.resultCode());
                phase = Phase::Done;
                break;
            }
            // Parse the batch: UDINT instance + STRING name + UINT symbol type.
            const uint8_t *d = msg.data();
            size_t n = msg.dataLength();
            size_t p = 0;
            uint32_t last = 0;
            while (p + 4 <= n) {
                uint32_t instance = clx::getU32(d + p); p += 4;
                if (p + 4 > n) break;
                uint32_t nameLen = clx::getU32(d + p); p += 4;
                if (p + nameLen > n) break;
                char name[46];
                size_t c = nameLen < sizeof(name) - 1 ? nameLen : sizeof(name) - 1;
                memcpy(name, d + p, c); name[c] = 0; p += nameLen;
                if (p + 2 > n) break;
                uint16_t symbolType = clx::getU16(d + p); p += 2;

                if (tagCount < kMaxTags) {
                    strncpy(tags[tagCount].name, name, sizeof(tags[0].name) - 1);
                    tags[tagCount].name[sizeof(tags[0].name) - 1] = 0;
                    tags[tagCount].symbolType = symbolType;
                    tagCount++;
                }
                last = instance;

                TagEntry tmp;
                strncpy(tmp.name, name, sizeof(tmp.name) - 1);
                tmp.name[sizeof(tmp.name) - 1] = 0;
                tmp.symbolType = symbolType;
                Serial.printf("  [%u] ", (unsigned)tagCount);
                printTag(tmp);
                Serial.println();
            }

            uint8_t status = msg.resultCode();
            if (status == 0x00) {
                Serial.printf("Enumeration complete: %u tag(s)\n", (unsigned)tagCount);
                valueCursor = 0;
                enumerating = false;
                phase = Phase::ReadValues;
            } else if (status == 0x06) {  // partial transfer: more tags remain
                nextInstance = last + 1;
                enumerating = false;
            } else {
                Serial.printf("enumerate status 0x%02X\n", status);
                valueCursor = 0;
                enumerating = false;
                phase = Phase::ReadValues;
            }
            break;
        }
        case Phase::ReadValues: {
            while (valueCursor < tagCount) {
                TagEntry &t = tags[valueCursor];
                bool isStruct = (t.symbolType & 0x8000) != 0;
                uint8_t dims = (t.symbolType >> 13) & 0x03;
                if (!isStruct && dims == 0) break;  // readable scalar
                valueCursor++;
            }
            if (valueCursor >= tagCount) {
                Serial.println("Done.");
                session.close();
                phase = Phase::Done;
                break;
            }
            if (!enumerating) {
                clx::Status st = startReadTag(tags[valueCursor].name);
                if (st == clx::Status::Pending) {
                    enumerating = true;
                } else {
                    valueCursor++;
                }
                break;
            }
            clx::Status st = msg.poll();
            if (st == clx::Status::Pending) break;
            if (st == clx::Status::Ok && msg.resultCode() == 0) {
                Serial.printf("  %-45s = ", tags[valueCursor].name);
                printValue(msg.data(), msg.dataLength());
                Serial.println();
            } else {
                Serial.printf("  %-45s = <read failed %s CIP 0x%02X>\n",
                              tags[valueCursor].name, clx::statusString(st), msg.resultCode());
            }
            enumerating = false;
            valueCursor++;
            break;
        }
        case Phase::Done:
        default:
            // Scan is complete: keep the connection closed and stay idle. Do not
            // restart the chip (a soft reset looks like a crash in the log).
            session.abort();
            conn.close();
            delay(1000);
            break;
    }
    delay(1);
}
