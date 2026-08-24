# ESP32ControlLogix - Native EtherNet/IP + CIP Client for ControlLogix

> [!CAUTION]
> _DO NOT USE THIS LIBRARY IN PRODUCTION._  This library is currently not working and is under active development. It has only been validated against the
> synthetic EtherNet/IP server found in /tools.

A lightweight, non-blocking EtherNet/IP and CIP (Common Industrial Protocol) client for the ESP32, purpose-built to talk to Allen-Bradley/Rockwell ControlLogix and compatible Logix controllers. No external dependencies, bounded memory, and a simple API.

## Table of Contents

- [Features](#features)
- [Supported Data Types](#supported-data-types)
- [Requirements](#requirements)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [API Reference](#api-reference)
  - [PlcClient (Top-Level API)](#plcclient-top-level-api)
  - [Tag](#tag)
  - [Connection](#connection)
  - [ExplicitMessage](#explicitmessage)
  - [Session](#session)
  - [Client (Ethernet)](#client-ethernet)
  - [TcpConnection](#tcpconnection)
  - [Status Codes](#status-codes)
  - [CIP Helpers](#cip-helpers)
- [Configuration](#configuration)
- [Examples](#examples)
- [Architecture](#architecture)
- [Limitations](#limitations)
- [Best Practices](#best-practices)
- [Troubleshooting](#troubleshooting)
- [Version History](#version-history)

## Features

- **Simple & Beginner-Friendly** : Simple API with a single top-level `PlcClient`.
- **Fully Non-Blocking** : No public call waits on network, socket, or PLC progress. `begin()`/`connect()`/`read()`/`write()` start work and return immediately; `poll()` advances everything.
- **Bounded Memory** : Fixed-size tag pool and reusable packet buffers.
- **Single In-Flight Operation** : One tag read/write at a time. `read()`/`write()` return `Busy` while another operation is in flight, so the shared connection is never contended.
- **Native Protocol Stack** : EtherNet/IP encapsulation, CIP explicit messaging, connected (Forward Open / SendUnitData) and unconnected (SendRRData) messaging.
- **Symbolic Logix Tags** : Read and write symbolic tags with typed accessors for scalars, arrays, and strings.
- **Callbacks** : Optional tag-completion and connection-state callbacks for event-driven integration.
- **Reconnect & Recovery** : Handles timeouts, disconnects, stale responses, and PLC restarts.
- **Transport-Independent** : Session and CIP logic are decoupled from the underlying transport (Wi-Fi or Ethernet).
- **Testable** : Includes a host-side Python-based synthetic EtherNet/IP server for validation without hardware.

## Supported Data Types

Typed accessors are provided for the following Logix elementary data types:

| Data Type | CIP Code | Size | Accessors |
|-----------|----------|------|-----------|
| BOOL | `0xC1` | 1 byte | `getBool` / `setBool` |
| SINT | `0xC2` | 1 byte | `getInt8` / `setInt8` |
| INT | `0xC3` | 2 bytes | `getInt16` / `setInt16` |
| DINT | `0xC4` | 4 bytes | `getInt32` / `setInt32` |
| LINT | `0xC5` | 8 bytes | `getInt64` / `setInt64` |
| USINT | `0xC6` | 1 byte | `getUint8` / `setUint8` |
| UINT | `0xC7` | 2 bytes | `getUint16` / `setUint16` |
| UDINT | `0xC8` | 4 bytes | `getUint32` / `setUint32` |
| ULINT | `0xC9` | 8 bytes | `getUint64` / `setUint64` |
| REAL | `0xCA` | 4 bytes | `getFloat32` / `setFloat32` |
| LREAL | `0xCB` | 8 bytes | `getFloat64` / `setFloat64` |
| STRING | `0xD0` | variable (4-byte length + data) | `getString` / `setString` |

## Requirements

- **Hardware** : Any ESP32-family board. The default configuration targets the M5Stack AtomS3 with the AtomPoE (W5500) shield, but other SPI or RMII Ethernet PHYs (e.g. LAN8720) are supported.
- **Board Library** : Arduino-ESP32 3.3.11
- **Network** : Static IPv4 configuration is provided by the sketch. A ControlLogix PLC (or the synthetic server) listening on port 44818.

## Installation

1. Copy the `ESP32ControlLogix` folder into your Arduino `libraries` directory.
2. Restart the Arduino IDE.
3. Include the umbrella header in your sketch:

```cpp
#include <ESP32ControlLogix.h>
```

## Quick Start

The `PlcClient` class is the easiest way to get started. It owns the full stack - Ethernet, TCP, EtherNet/IP session, and a tag pool for memory efficiency.

```cpp
#include <ESP32ControlLogix.h>

clx::PlcClient plc;
int tag = -1;
bool started = false;

void setup() {
    Serial.begin(115200);

    // Static IPv4 for the AtomPoE on the target LAN.
    clx::Client::Config cfg;
    cfg.ip      = IPAddress(192, 168, 1, 50);
    cfg.gateway = IPAddress(192, 168, 1, 1);
    cfg.subnet  = IPAddress(255, 255, 255, 0);
    cfg.dns     = IPAddress(192, 168, 1, 1);

    plc.begin(cfg);                                        // start Ethernet
    plc.connect(IPAddress(192, 168, 1, 2), 44818, 5000);   // queue TCP + session
}

void loop() {
    plc.poll();                                            // advance everything

    if (plc.ready() && !started) {
        tag = plc.createTag("MyTag");                      // allocate a tag (1 element)
        if (tag >= 0) {                                    // check for a valid handle
            started = true;
            plc.read(tag, 5000);                           // start a read
        }
        // else: pool exhausted (NoMemory) or bad name (InvalidArg) - retry later
    }

    if (started && plc.tagStatus(tag) == clx::Status::Ok) {
        int32_t v = plc.tag(tag)->getInt32(0);             // read the value
        Serial.printf("MyTag = %ld\n", (long)v);
        plc.destroyTag(tag);
        started = false;
    }

    delay(10); // Do other work...
}
```

## API Reference

All symbols live in the `clx` namespace. Every call is non-blocking: methods that begin an operation return immediately, and `poll()` advances state.

### PlcClient (Top-Level API)

The high-level facade that owns the Ethernet client, TCP connection, EtherNet/IP session, and a bounded tag pool.

```cpp
class PlcClient {
    static constexpr size_t kMaxTags    = 8;    // max simultaneous tags
    static constexpr size_t kMaxTagName = 64;   // max tag name length

    Status begin(const Client::Config &cfg);           // start Ethernet
    Status connect(const IPAddress &ip, uint16_t port, uint32_t timeoutMs);
    Status poll();                                     // advance everything
    Status disconnect();                               // graceful disconnect

    int    createTag(const char *name, uint32_t elementCount = 1); // >=0 handle, or negative Status
    Status destroyTag(int handle);
    Status read(int handle, uint32_t timeoutMs);
    Status write(int handle, uint32_t timeoutMs);
    Status tagStatus(int handle) const;                // non-advancing
    Tag   *tag(int handle);                            // typed access (nullptr if invalid)
    Status abortTag(int handle);

    void setTagCallback(TagCallback cb, void *userData);
    void setStateCallback(StateCallback cb, void *userData);

    bool     ready() const;
    uint32_t sessionHandle() const;
    int      tagCount() const;
};
```

> **Single in-flight:** `read()`/`write()` return `Busy` while another tag operation is in flight. Drive one operation at a time — wait for `tagStatus()` to leave `Pending` (or chain from the tag callback) before starting the next.

Callbacks:
- `TagCallback` : `void (*)(int handle, Status status, void *userData)` - invoked once when a tag's read/write completes.
- `StateCallback` : `void (*)(Status status, void *userData)` - invoked on connect (`Ok`) and disconnect (`Closed`).

### Tag

A single Logix tag: symbolic name, typed data buffer, and non-blocking Read Tag (`0x4C`) / Write Tag (`0x4D`) over unconnected messaging. The `ExplicitMessage` is supplied by the caller (not owned by `Tag`), so many tags can share one message — only one operation runs at a time.

```cpp
class Tag {
    static constexpr size_t kMaxDataSize = 256;

    Status read(ExplicitMessage &msg, TcpConnection &conn, uint32_t sessionHandle,
                const char *name, uint32_t elementCount, uint32_t timeoutMs);
    Status write(ExplicitMessage &msg, TcpConnection &conn, uint32_t sessionHandle,
                 const char *name, uint32_t elementCount, uint32_t timeoutMs);
    Status poll(ExplicitMessage &msg);
    void   abort(ExplicitMessage &msg);
    Status status() const;

    uint8_t        dataType() const;
    const uint8_t *data() const;
    size_t         dataLength() const;
    uint8_t        resultCode() const;
    void           setDataType(uint8_t type);

    // Typed accessors (offset is a byte offset into the data buffer).
    bool     getBool(size_t off) const;
    int8_t   getInt8(size_t off) const;
    int16_t  getInt16(size_t off) const;
    int32_t  getInt32(size_t off) const;
    int64_t  getInt64(size_t off) const;
    uint8_t  getUint8(size_t off) const;
    uint16_t getUint16(size_t off) const;
    uint32_t getUint32(size_t off) const;
    uint64_t getUint64(size_t off) const;
    float    getFloat32(size_t off) const;
    double   getFloat64(size_t off) const;
    // ...and matching setBool/setInt8/.../setFloat64 setters.

    size_t getString(char *buf, size_t bufLen) const;
    void   setString(const char *s);
};
```

The data type is learned from a read (or set explicitly via `setDataType()`); a write requires a known data type.

### Connection

A connected CIP connection (Forward Open / SendUnitData / Forward Close). Every received packet is bounds-checked, and responses are rejected unless the connection ID and sequence number match the request - rejecting stale, replayed, and malformed data.

```cpp
class Connection {
    static constexpr size_t kMaxDataSize = 256;

    Status open(TcpConnection &conn, uint32_t sessionHandle, const char *tagName,
                uint32_t timeoutMs);
    Status send(TcpConnection &conn, uint32_t sessionHandle, uint8_t service,
                const uint8_t *path, size_t pathLen,
                const uint8_t *data, size_t dataLen, uint32_t timeoutMs);
    Status close(TcpConnection &conn, uint32_t sessionHandle, uint32_t timeoutMs);
    Status poll();

    uint32_t originatorConnectionId() const;  // O->T
    uint32_t targetConnectionId() const;      // T->O
    bool     isOpen() const;

    uint8_t        replyService() const;
    uint8_t        resultCode() const;
    const uint8_t *data() const;
    size_t         dataLength() const;
};
```

### ExplicitMessage

One CIP explicit-message exchange (SendRRData / unconnected messaging). The `data()` pointer is valid until the next `send()`.

```cpp
class ExplicitMessage {
    static constexpr size_t kMaxCipData = 256;

    Status send(TcpConnection &conn, uint32_t sessionHandle, uint8_t service,
                const uint8_t *path, size_t pathLen,
                const uint8_t *data, size_t dataLen, uint32_t timeoutMs);
    Status poll();
    void   abort();

    uint8_t        replyService() const;
    uint8_t        resultCode() const;
    const uint8_t *data() const;
    size_t         dataLength() const;
};
```

### Session

EtherNet/IP encapsulation session (RegisterSession / UnregisterSession) with bounded receive framing.

```cpp
class Session {
    static constexpr size_t kMaxBodySize = 512;

    Status open(TcpConnection &conn, uint32_t timeoutMs);
    Status poll();
    Status close();
    void   abort();

    uint32_t handle() const;
    bool     registered() const;
};
```

### Client (Ethernet)

Non-blocking Ethernet client over the ESP32 `ETH` driver with a static IPv4 configuration.

```cpp
class Client {
    struct Config {
        eth_phy_type_t phyType = ETH_PHY_W5500;
        IPAddress ip{192, 168, 1, 50};
        IPAddress gateway{192, 168, 1, 1};
        IPAddress subnet{255, 255, 255, 0};
        IPAddress dns{192, 168, 1, 1};
        int sck = 5, miso = 7, mosi = 8, cs = 6;   // SPI pins
        int irq = -1, rst = -1, phyAddr = 1;
        bool rmii = false;                          // true = RMII PHY
        int mdc = 23, mdio = 18, power = -1, clkMode = 0;
    };

    Status begin(const Config &cfg);
    Status poll();
    bool     ready() const;
    IPAddress localIP() const;
    IPAddress subnetMask() const;
};
```

### TcpConnection

Non-blocking TCP connection over lwIP sockets.

```cpp
class TcpConnection {
    Status connect(const IPAddress &ip, uint16_t port, uint32_t timeoutMs);
    Status poll();
    int    read(uint8_t *buffer, size_t length);   // >0 bytes, 0 would-block, <0 error
    int    write(const uint8_t *buffer, size_t length);
    void   close();
    bool   connected() const;
    int    fd() const;
};
```

### Status Codes

Every operation returns a `clx::Status`. `Pending` (0) and `Ok` (1) are the progress/success path; every failure value is negative.

| Value | Name | Meaning |
|-------|------|---------|
| `0` | `Pending` | Operation in progress; call `poll()` again |
| `1` | `Ok` | Operation completed successfully |
| `2` | `Busy` | Resource already active |
| `3` | `WouldBlock` | Non-blocking I/O would block; retry later |
| `-1` | `Closed` | Connection closed / not connected |
| `-2` | `Timeout` | Deadline exceeded |
| `-3` | `NotReady` | Transport not ready (no link / no IP) |
| `-4` | `NoMemory` | Allocation failure |
| `-5` | `InvalidArg` | Invalid argument |
| `-6` | `Error` | Generic / unspecified error |

Use `clx::statusString(status)` to get a human-readable name.

### CIP Helpers

Path-encoding and data-type helpers in `clx`:

```cpp
size_t appendClass(uint8_t *out, uint8_t classId);        // 8-bit class segment
size_t appendInstance(uint8_t *out, uint8_t instanceId);  // 8-bit instance segment
size_t appendAttribute(uint8_t *out, uint8_t attributeId); // 8-bit attribute segment
size_t appendSymbolic(uint8_t *out, const char *name);   // symbolic tag segment

size_t      dataTypeElementSize(DataType t);
const char *dataTypeName(DataType t);
```

Little-endian encode/decode helpers (`putU16`, `putU32`, `putU64`, `getU16`, `getU32`, `getU64`) and the `EncapsulationHeader` struct with `encodeHeader()`/`decodeHeader()` are also available for advanced use.

## Configuration

The sketch supplies the Ethernet configuration via `clx::Client::Config`. The defaults match the M5Stack AtomS3 + AtomPoE (W5500) SPI PHY:

```cpp
clx::Client::Config cfg;
cfg.ip      = IPAddress(192, 168, 1, 50);
cfg.gateway = IPAddress(192, 168, 1, 1);
cfg.subnet  = IPAddress(255, 255, 255, 0);
cfg.dns     = IPAddress(192, 168, 1, 1);
```

To use an RMII PHY (e.g. LAN8720) on a board with the built-in EMAC:

```cpp
cfg.phyType = ETH_PHY_LAN8720;
cfg.rmii    = true;
cfg.phyAddr = 0;
cfg.mdc     = 23;
cfg.mdio    = 18;
cfg.power   = -1;
cfg.clkMode = ETH_CLOCK_GPIO0_IN;  // 0
```

The tag pool size (`PlcClient::kMaxTags`) defaults to 8. To change it, define `ESP32_CONTROLLOGIX_MAX_TAGS` before including the header:

```cpp
#define ESP32_CONTROLLOGIX_MAX_TAGS 16
#include <ESP32ControlLogix.h>
```


## Examples

The `examples/` directory contains progressively lower-level demos:

| Example | Description |
|---------|-------------|
| `PlcClientDemo` | Top-level `PlcClient` API: connect, read-modify-write of a DINT tag. |
| `TagReadWrite` | Unconnected `Tag` read/write for DINT, REAL, and STRING tags. |
| `ConnectedTagReadWrite` | Connected messaging (`Connection`): Forward Open -> Read -> Write -> Verify -> Forward Close. |
| `IdentityQuery` | CIP Identity Object `Get_Attribute_Single` queries via `ExplicitMessage`. |
| `EipSession` | EtherNet/IP `Session` register/unregister. |
| `LanInventory` | LAN inventory scan. |
| `ReliabilityDemo` | Reconnect, abort, and resource-stress behavior. |

Each example can point at a real ControlLogix PLC or at the host-side synthetic server (`tools/synthetic_eip_server.py`).

## Architecture

```text
Application / public tag API (PlcClient, Tag)
        |
Tag registry and lifecycle manager
        |
Single in-flight transaction (one tag operation at a time)
        |
EtherNet/IP session manager (Session)
        |
CIP encoder/decoder and Logix symbolic path builder (ExplicitMessage, Connection, Cip)
        |
Transport interface (Client, TcpConnection)
   |                 |
ESP32 Wi-Fi      ESP32 W5500 Ethernet
```

Key design goals:

- **Non-blocking** : No public call waits for network, socket, or PLC progress. Timeouts are deadlines checked from `poll()`, never sleeps or blocking `select()`.
- **Bounded RAM** : Fixed tag pool (`kMaxTags = 8`), fixed buffers, no heap allocation after construction.
- **Minimal task count** : One shared worker task / event-driven scheduler, not one task per session or tag.
- **Reusable buffers** : Packet buffers are reused.
- **Predictable failure** : Allocation failure returns a defined error.

## Limitations

- **Bounded tag pool** : `kMaxTags` (default 8) simultaneous tags; `createTag()` returns `NoMemory` when exhausted. Override via `ESP32_CONTROLLOGIX_MAX_TAGS`.
- **Bounded buffer** : Tag data is capped at 256 bytes
- **Single in-flight** : Only one tag read/write runs at a time (no pipelining). `read()`/`write()` return `Busy` while another operation is in flight.
- **Static IP** : The current transport targets static IPv4 configuration; DHCP is not yet wired into the public API.

## Best Practices

- **Poll in a loop** : Drive everything with `poll()`; never assume a call completes synchronously.
- **Check return values** : Always verify `Status` codes - `Pending` means "call again", negative values are failures.
- **One operation at a time** : `read()`/`write()` return `Busy` while another operation is in flight. Serialize tag operations — wait for completion (or chain from the tag callback) before starting the next.
- **Respect the tag pool** : `destroyTag()` handles you no longer need to avoid exhausting `kMaxTags`.
- **Verify writes** : Read back after a write to confirm the PLC accepted the value.
- **Use callbacks** : Prefer `setTagCallback`/`setStateCallback` for event-driven designs instead of tight polling.
- **Set timeouts** : Every operation takes a `timeoutMs` deadline; choose values appropriate to your network.

Reading several tags one at a time (single in-flight) — callback style:

```cpp
int handles[3];
size_t cursor = 0;

void onTag(int handle, clx::Status st, void *ud) {
    (void)handle; (void)st; (void)ud;
    if (++cursor < 3) plc.read(handles[cursor], 5000);  // chain the next read
}

// after connect + createTag() for each name:
plc.setTagCallback(onTag, nullptr);
plc.read(handles[0], 5000);   // start the first; the callback chains the rest
```

## Troubleshooting

**`begin()` returns `Error`**
- Check the Ethernet PHY configuration matches your hardware (SPI vs. RMII pins).

**`connect()` never becomes `ready()`**
- Verify the target IP/port and that the PLC (or synthetic server) is reachable on port 44818.
- Confirm the static IP/subnet/gateway are correct for your LAN.

**`createTag()` returns a negative value**
- The tag pool is exhausted (`NoMemory`) - free unused tags with `destroyTag()`.

**`tagStatus()` returns `Timeout`**
- The read/write deadline elapsed. Increase `timeoutMs` or check network/PLC health.

**Read value looks wrong**
- Confirm the data type matches the tag's Logix type; use the correct typed accessor (e.g. `getInt32` for DINT).

## Version History

- **0.1.1** : Enforce a single in-flight tag operation (`read()`/`write()` return `Busy` while another is active); `Tag` now takes a shared, caller-supplied `ExplicitMessage`
- **0.1.0** : Initial release.
