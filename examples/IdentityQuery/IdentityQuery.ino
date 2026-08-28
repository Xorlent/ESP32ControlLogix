/*
 * IdentityQuery - CIP explicit-messaging demo.
 *
 * The sketch queries the Identity object (class 1, instance 1) attributes
 * vendor ID, product name, serial number, and state, printing each result.
 *
 * Point TARGET_IP/TARGET_PORT at a PLC (EtherNet/IP port 44818) or at the
 * host-side synthetic server (tools/synthetic_eip_server.py), which answers
 * Identity queries with fixed values.
 */

#include <ESP32ControlLogix.h>

// Static IPv4 for the AtomPoE on the target LAN.
const IPAddress LOCAL_IP(192, 168, 1, 50);
const IPAddress LOCAL_GATEWAY(192, 168, 1, 1);
const IPAddress LOCAL_SUBNET(255, 255, 255, 0);
const IPAddress LOCAL_DNS(192, 168, 1, 1);

// EtherNet/IP target (change to your PLC or the synthetic server host).
const IPAddress TARGET_IP(192, 168, 1, 2);
constexpr uint16_t TARGET_PORT = 44818;
constexpr uint32_t CONNECT_TIMEOUT_MS = 5000;
constexpr uint32_t SESSION_TIMEOUT_MS = 5000;
constexpr uint32_t MESSAGE_TIMEOUT_MS = 5000;
constexpr uint32_t RETRY_DELAY_MS = 5000;

clx::Client eth;
clx::Client::Config cfg;
clx::TcpConnection conn;
clx::Session session;
clx::ExplicitMessage msg;

// Identity attributes to query, in order.
struct Query {
    uint8_t attr;
    const char *label;
};
const Query kQueries[] = {
    {clx::kIdentityVendorId, "vendor"},
    {clx::kIdentityProductName, "product name"},
    {clx::kIdentitySerialNumber, "serial"},
    {clx::kIdentityState, "state"},
};
constexpr size_t kQueryCount = sizeof(kQueries) / sizeof(kQueries[0]);

enum class Phase : uint8_t {
    WaitEthernet,
    Connect,
    OpenSession,
    Query,
    CloseSession,
    RetryWait,
};

Phase phase = Phase::WaitEthernet;
uint32_t phaseStarted = 0;
size_t queryIndex = 0;

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.printf("ESP32ControlLogix %s - CIP Identity query demo\n", clx::version());

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
        phase = Phase::RetryWait;
    }
    phaseStarted = millis();
}

// Build the Identity path (class 1, instance 1) and start a query.
clx::Status startQuery(uint8_t attr) {
    // Get Attribute Single path = class + instance + attribute (3 words). The
    // attribute segment is a path segment, not service data, so it must be
    // appended to the path for the path-size word to be correct.
    uint8_t path[6];
    size_t pl = 0;
    pl += clx::appendClass(path + pl, 1);          // class = Identity (1)
    pl += clx::appendInstance(path + pl, 1);       // instance = 1
    pl += clx::appendAttribute(path + pl, attr);   // attribute segment

    // Route through the backplane to the CPU (slot 0). When connected to a
    // ControlLogix Ethernet module, the Identity object at class 1/instance 1
    // belongs to the module itself; the CPU's identity lives in slot 0.
    return msg.sendRouted(conn, session.handle(), 0,
                          static_cast<uint8_t>(clx::Service::GetAttributeSingle),
                          path, pl, nullptr, 0, MESSAGE_TIMEOUT_MS);
}

void printResult(const Query &q) {
    if (msg.resultCode() != 0) {
        Serial.printf("  %s: CIP status 0x%02X\n", q.label, msg.resultCode());
        return;
    }
    const uint8_t *d = msg.data();
    size_t n = msg.dataLength();
    if (q.attr == clx::kIdentityVendorId && n >= 2) {
        Serial.printf("  %s: %u\n", q.label, (unsigned)(d[0] | (d[1] << 8)));
    } else if (q.attr == clx::kIdentitySerialNumber && n >= 4) {
        Serial.printf("  %s: 0x%08lX\n", q.label,
                      (unsigned long)(d[0] | (d[1] << 8) | (d[2] << 16) | (d[3] << 24)));
    } else if (q.attr == clx::kIdentityState && n >= 1) {
        Serial.printf("  %s: %u\n", q.label, (unsigned)d[0]);
    } else if (q.attr == clx::kIdentityProductName) {
        Serial.printf("  %s: ", q.label);
        for (size_t i = 0; i < n; ++i) {
            Serial.write(d[i]);
        }
        Serial.println();
    } else {
        Serial.printf("  %s: %u bytes\n", q.label, (unsigned)n);
    }
}

void loop() {
    switch (phase) {
        case Phase::WaitEthernet: {
            clx::Status st = eth.poll();
            if (st == clx::Status::Ok) {
                Serial.printf("Ethernet ready: ip=%s\n", eth.localIP().toString().c_str());
                Serial.printf("Connecting to %s:%u ...\n", TARGET_IP.toString().c_str(), TARGET_PORT);
                st = conn.connect(TARGET_IP, TARGET_PORT, CONNECT_TIMEOUT_MS);
                Serial.printf("TcpConnection::connect() -> %s\n", clx::statusString(st));
                phase = Phase::Connect;
                phaseStarted = millis();
            } else if (st == clx::Status::Error) {
                eth.begin(cfg);
                phaseStarted = millis();
            }
            break;
        }
        case Phase::Connect: {
            clx::Status st = conn.poll();
            if (st == clx::Status::Ok) {
                Serial.println("TCP connected; opening session.");
                st = session.open(conn, SESSION_TIMEOUT_MS);
                Serial.printf("Session::open() -> %s\n", clx::statusString(st));
                phase = Phase::OpenSession;
                phaseStarted = millis();
            } else if (st == clx::Status::Timeout || st == clx::Status::Error) {
                Serial.printf("TCP connect failed: %s\n", clx::statusString(st));
                phase = Phase::RetryWait;
                phaseStarted = millis();
            }
            break;
        }
        case Phase::OpenSession: {
            clx::Status st = session.poll();
            if (st == clx::Status::Ok) {
                Serial.printf("Session registered: handle=0x%08lX\n",
                              (unsigned long)session.handle());
                queryIndex = 0;
                st = startQuery(kQueries[queryIndex].attr);
                Serial.printf("Query %s -> %s\n", kQueries[queryIndex].label, clx::statusString(st));
                phase = Phase::Query;
                phaseStarted = millis();
            } else if (st == clx::Status::Timeout || st == clx::Status::Error) {
                Serial.printf("RegisterSession failed: %s\n", clx::statusString(st));
                phase = Phase::RetryWait;
                phaseStarted = millis();
            }
            break;
        }
        case Phase::Query: {
            clx::Status st = msg.poll();
            if (st == clx::Status::Ok) {
                printResult(kQueries[queryIndex]);
                ++queryIndex;
                if (queryIndex < kQueryCount) {
                    st = startQuery(kQueries[queryIndex].attr);
                    Serial.printf("Query %s -> %s\n", kQueries[queryIndex].label, clx::statusString(st));
                } else {
                    Serial.println("Identity queries complete; closing session.");
                    session.close();
                    phase = Phase::CloseSession;
                }
                phaseStarted = millis();
            } else if (st == clx::Status::Timeout || st == clx::Status::Error) {
                Serial.printf("Query %s failed: %s\n", kQueries[queryIndex].label, clx::statusString(st));
                phase = Phase::RetryWait;
                phaseStarted = millis();
            }
            break;
        }
        case Phase::CloseSession: {
            clx::Status st = session.poll();
            if (st == clx::Status::Ok) {
                Serial.println("Session closed.");
                conn.close();
                phase = Phase::RetryWait;
                phaseStarted = millis();
            } else if (st == clx::Status::Timeout || st == clx::Status::Error) {
                session.abort();
                conn.close();
                phase = Phase::RetryWait;
                phaseStarted = millis();
            }
            break;
        }
        case Phase::RetryWait:
        default:
            session.abort();
            conn.close();
            if (millis() - phaseStarted >= RETRY_DELAY_MS) {
                phase = Phase::WaitEthernet;
                phaseStarted = millis();
            }
            break;
    }
    delay(10);
}

