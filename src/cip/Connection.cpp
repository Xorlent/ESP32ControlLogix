#include "Connection.h"

#include <Arduino.h>
#include <esp_random.h>
#include <esp_mac.h>
#include <string.h>

#include "../transport/TcpConnection.h"

namespace clx {

// Originator identity for Forward Open/Close. The vendor ID must not be
// Rockwell's reserved 0x0001 (this is not a Rockwell device); it is a
// non-reserved placeholder for a hobbyist/private device.
constexpr uint16_t kOriginatorVendorId = 0xF33D;

// Fallback originator serial when the device MAC is unavailable (or all-zero).
constexpr uint32_t kOriginatorSerialFallback = 0x11223344;

// Originator serial number (32-bit): derived from the low 4 bytes of the base
// MAC so multiple ESP32s on one network get distinct CIP identities. Cached;
// falls back to a stable constant if the MAC is unavailable or all-zero.
static uint32_t originatorSerialNumber() {
    static uint32_t serial = 0;
    if (serial != 0) {
        return serial;
    }
    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        uint32_t v = uint32_t(mac[2]) | (uint32_t(mac[3]) << 8) |
                     (uint32_t(mac[4]) << 16) | (uint32_t(mac[5]) << 24);
        if (v != 0) {
            serial = v;
        }
    }
    if (serial == 0) {
        serial = kOriginatorSerialFallback;
    }
    return serial;
}

Connection::~Connection() {
    state_ = State::Idle;
}

Status Connection::open(TcpConnection &conn, uint32_t sessionHandle, const char *tagName,
                       uint32_t timeoutMs, uint8_t cpuSlot) {
    if (tagName == nullptr || strlen(tagName) >= sizeof(tagName_)) {
        return Status::InvalidArg;
    }
    if (state_ == State::Opening || state_ == State::Sending || state_ == State::Closing) {
        return Status::Busy;
    }
    return startOpen(conn, sessionHandle, tagName, timeoutMs, cpuSlot);
}

Status Connection::send(TcpConnection &conn, uint32_t sessionHandle, uint8_t service,
                        const uint8_t *path, size_t pathLen,
                        const uint8_t *data, size_t dataLen, uint32_t timeoutMs) {
    if (state_ != State::Open) {
        return Status::NotReady;
    }
    return startSend(conn, sessionHandle, service, path, pathLen, data, dataLen, timeoutMs);
}

Status Connection::close(TcpConnection &conn, uint32_t sessionHandle, uint32_t timeoutMs) {
    if (state_ != State::Open) {
        return Status::NotReady;
    }
    return startClose(conn, sessionHandle, timeoutMs);
}

// Encode the connection path. When routed (cpuSlot_ != kNoRoute), prepend the
// backplane route to the CPU followed by the Message Router (class 2, instance 1);
// when direct, the path is just the symbolic tag segment. Returns bytes written.
size_t Connection::buildConnectionPath(uint8_t *out, const char *tagName) const {
    size_t total = 0;
    if (cpuSlot_ != kNoRoute) {
        total += appendPortSegment(out + total, 1, cpuSlot_);  // backplane -> CPU slot
        total += appendClass(out + total, 2);                   // class 2 = Message Router
        total += appendInstance(out + total, 1);                // instance 1
    }
    total += appendSymbolic(out + total, tagName);              // symbolic tag
    return total;
}

Status Connection::startOpen(TcpConnection &conn, uint32_t sessionHandle, const char *tagName,
                             uint32_t timeoutMs, uint8_t cpuSlot) {
    // Choose a random target->originator connection ID (non-zero). Per CIP,
    // the originator assigns the T->O connection ID (we consume T->O data)
    // and the target assigns the O->T connection ID, returning it in the
    // Forward Open response. The O->T ID is therefore left at 0 in the request.
    toConnId_ = esp_random();
    if (toConnId_ == 0) {
        toConnId_ = 1;
    }
    otConnId_ = 0;
    strncpy(tagName_, tagName, sizeof(tagName_) - 1);
    tagName_[sizeof(tagName_) - 1] = 0;
    cpuSlot_ = cpuSlot;

    // Incrementing connection serial (process-wide) so the target can
    // distinguish sequential/reopened connections from the same originator.
    static uint16_t s_connSerial = 0;
    if (++s_connSerial == 0) ++s_connSerial;  // skip 0
    connSerial_ = s_connSerial;

    // Request path to the Connection Manager (class 6, instance 1).
    const uint8_t reqPath[4] = {0x20, 0x06, 0x24, 0x01};

    // Forward Open connection parameters + connection path.
    uint8_t data[128];
    size_t d = 0;
    data[d++] = 0x0A;                          // priority/tick time
    data[d++] = 0x0E;                          // timeout ticks
    putU32(data + d, 0); d += 4;               // O->T connection ID (0; target assigns)
    putU32(data + d, toConnId_); d += 4;       // T->O connection ID (ours)
    putU16(data + d, connSerial_); d += 2;               // connection serial number
    putU16(data + d, kOriginatorVendorId); d += 2;       // originator vendor ID
    putU32(data + d, originatorSerialNumber()); d += 4;  // originator serial number
    data[d++] = 0x03;                          // connection timeout multiplier
    data[d++] = 0; data[d++] = 0; data[d++] = 0;  // reserved (3)
    // Network connection params (O->T / T->O): 0x4200 = "variable size" Class 3
    // flags; OR in our connected payload capacity (bytes) so the target sees a
    // concrete max. Matches libplctag's AB_EIP_CONN_PARAM | max_payload_guess.
    const uint16_t connParams = uint16_t(0x4200u | kMaxDataSize);
    putU32(data + d, 10000); d += 4;           // O->T RPI (us)
    putU16(data + d, connParams); d += 2;      // O->T network params
    putU32(data + d, 10000); d += 4;           // T->O RPI (us)
    putU16(data + d, connParams); d += 2;      // T->O network params
    data[d++] = 0xA3;                          // transport type/trigger (class 3)

    // Connection path (backplane route -> Message Router -> symbolic tag, or
    // just the symbolic tag when direct). Use the truncated tagName_ (not the
    // caller's tagName) so a long name cannot overflow the data buffer.
    size_t pathLen = buildConnectionPath(data + d + 1, tagName_);
    data[d] = uint8_t(pathLen / 2);            // connection path size (words)
    d += 1 + pathLen;

    Status st = fwd_.send(conn, sessionHandle, 0x54, reqPath, sizeof(reqPath), data, d, timeoutMs);
    if (st != Status::Pending) {
        return st;
    }
    state_ = State::Opening;
    return Status::Pending;
}

Status Connection::startClose(TcpConnection &conn, uint32_t sessionHandle, uint32_t timeoutMs) {
    const uint8_t reqPath[4] = {0x20, 0x06, 0x24, 0x01};

    // Forward Close request data (matching the Forward Open identity):
    // priority/tick (1) + timeout ticks (1) + connection serial (2) +
    // originator vendor ID (2) + originator serial (4) + path size (1) +
    // reserved (1) + connection path.
    uint8_t data[128];
    size_t d = 0;
    data[d++] = 0x0A;                          // priority/tick time
    data[d++] = 0x0E;                          // timeout ticks
    putU16(data + d, connSerial_); d += 2;               // connection serial number
    putU16(data + d, kOriginatorVendorId); d += 2;       // originator vendor ID
    putU32(data + d, originatorSerialNumber()); d += 4;  // originator serial number

    // Connection path (matches Forward Open).
    size_t pathLen = buildConnectionPath(data + d + 2, tagName_);
    data[d] = uint8_t(pathLen / 2);            // connection path size (words)
    data[d + 1] = 0;                           // reserved
    d += 2 + pathLen;

    Status st = fwd_.send(conn, sessionHandle, 0x4E, reqPath, sizeof(reqPath), data, d, timeoutMs);
    if (st != Status::Pending) {
        return st;
    }
    state_ = State::Closing;
    return Status::Pending;
}

Status Connection::startSend(TcpConnection &conn, uint32_t sessionHandle, uint8_t service,
                             const uint8_t *path, size_t pathLen,
                             const uint8_t *data, size_t dataLen, uint32_t timeoutMs) {
    const size_t cipLen = 2 + pathLen + dataLen;
    if (cipLen > kMaxDataSize || (pathLen & 1) != 0 ||
        (pathLen > 0 && path == nullptr) || (dataLen > 0 && data == nullptr)) {
        return Status::InvalidArg;
    }
    conn_ = &conn;

    // Encapsulation header (SendUnitData).
    EncapsulationHeader h;
    h.command = static_cast<uint16_t>(Command::SendUnitData);
    h.length = uint16_t(22 + cipLen);
    h.session = sessionHandle;
    h.status = 0;
    h.context = context_;
    h.options = 0;
    encodeHeader(tx_, h);

    // Body: interface handle, timeout, item count, connected-address item,
    // connected-data item.
    uint8_t *body = tx_ + kEncapsulationHeaderSize;
    putU32(body, 0);                          // interface handle
    putU16(body + 4, 0);                      // timeout
    putU16(body + 6, 2);                      // CPF item count = 2
    putU16(body + 8, 0x00A1);                 // item 1: connected address type
    putU16(body + 10, 4);                     // item 1: length 4
    putU32(body + 12, otConnId_);             // item 1: O->T connection ID
    putU16(body + 16, 0x00B1);                // item 2: connected data type
    putU16(body + 18, uint16_t(2 + cipLen));  // item 2: length (sequence + CIP)
    putU16(body + 20, sequence_);             // item 2: sequence number

    // CIP request payload.
    uint8_t *cip = body + 22;
    cip[0] = service;
    cip[1] = uint8_t(pathLen / 2);
    if (pathLen) {
        memcpy(cip + 2, path, pathLen);
    }
    if (dataLen) {
        memcpy(cip + 2 + pathLen, data, dataLen);
    }

    txLen_ = kEncapsulationHeaderSize + 22 + cipLen;
    txSent_ = 0;
    rxLen_ = 0;
    rxExpected_ = kEncapsulationHeaderSize;
    sentContext_ = context_;
    sentSequence_ = sequence_;
    ++context_;
    ++sequence_;
    deadline_ = millis() + timeoutMs;
    state_ = State::Sending;
    return Status::Pending;
}

Status Connection::poll() {
    switch (state_) {
        case State::Opening: return pollOpening();
        case State::Sending: return pollSending();
        case State::Closing: return pollClosing();
        case State::Open:    return Status::Ok;
        case State::Closed:  return Status::Closed;
        case State::Failed:  return Status::Error;
        case State::Idle:    return Status::NotReady;
    }
    return Status::Error;
}

Status Connection::pollOpening() {
    Status st = fwd_.poll();
    if (st == Status::Pending) {
        return Status::Pending;
    }
    if (st != Status::Ok) {
        state_ = State::Failed;
        return st;
    }
    if (fwd_.resultCode() != 0 || fwd_.dataLength() < 12) {
        state_ = State::Failed;
        return Status::Error;
    }
    // Forward Open response: O->T conn ID (4), T->O conn ID (4), serial (2), vendor (2).
    uint32_t ot = getU32(fwd_.data());        // O->T ID assigned by the target
    uint32_t to = getU32(fwd_.data() + 4);    // T->O ID echoed back (ours)
    if (ot == 0 || to == 0) {
        state_ = State::Failed;
        return Status::Error;
    }
    // The target echoes our T->O ID and supplies the O->T ID.
    if (to != toConnId_) {
        state_ = State::Failed;
        return Status::Error;  // mismatched T->O connection ID (echo)
    }
    otConnId_ = ot;  // save the target-assigned O->T ID (used in SendUnitData)
    state_ = State::Open;
    return Status::Ok;
}

Status Connection::pollSending() {
    if ((int32_t)(millis() - deadline_) >= 0) {
        state_ = State::Failed;
        return Status::Timeout;
    }
    Status st = writePending();
    if (st != Status::Ok) {
        if (st != Status::Pending) {
            state_ = State::Failed;
        }
        return st;
    }
    st = readResponse();
    if (st == Status::Pending) {
        return Status::Pending;
    }
    if (st != Status::Ok) {
        state_ = State::Failed;
        return st;
    }
    st = parseConnectedResponse();
    state_ = (st == Status::Ok) ? State::Open : State::Failed;
    return st;
}

Status Connection::pollClosing() {
    Status st = fwd_.poll();
    if (st == Status::Pending) {
        return Status::Pending;
    }
    if (st != Status::Ok) {
        state_ = State::Failed;
        return st;
    }
    state_ = State::Closed;
    return Status::Ok;
}

Status Connection::writePending() {
    while (txSent_ < txLen_) {
        int n = conn_->write(tx_ + txSent_, txLen_ - txSent_);
        if (n > 0) {
            txSent_ += size_t(n);
            continue;
        }
        if (n == 0) {
            return Status::Pending;  // socket buffer full; retry on next poll
        }
        return static_cast<Status>(n);  // Closed/Error
    }
    return Status::Ok;
}

Status Connection::readResponse() {
    while (rxLen_ < rxExpected_) {
        int n = conn_->read(rx_ + rxLen_, rxExpected_ - rxLen_);
        if (n > 0) {
            rxLen_ += size_t(n);
            if (rxLen_ == kEncapsulationHeaderSize) {
                EncapsulationHeader h = decodeHeader(rx_);
                if (h.length > 22 + kMaxDataSize) {
                    return Status::Error;  // oversized / malformed packet
                }
                rxExpected_ = kEncapsulationHeaderSize + h.length;
            }
            continue;
        }
        if (n == 0) {
            return Status::Pending;  // no data yet; retry on next poll
        }
        return static_cast<Status>(n);  // Closed/Error
    }
    return Status::Ok;
}

Status Connection::parseConnectedResponse() {
    // Validate the encapsulation header.
    EncapsulationHeader h = decodeHeader(rx_);
    if (h.command != static_cast<uint16_t>(Command::SendUnitData) || h.status != 0) {
        return Status::Error;
    }
    if (h.context != sentContext_) {
        return Status::Error;  // stale / mismatched response
    }

    const uint8_t *body = rx_ + kEncapsulationHeaderSize;
    size_t bodyLen = rxLen_ - kEncapsulationHeaderSize;
    if (bodyLen < 8) {  // interface handle 4 + timeout 2 + item count 2
        return Status::Error;
    }

    uint16_t itemCount = getU16(body + 6);
    const uint8_t *p = body + 8;
    size_t remaining = bodyLen - 8;

    // Parse CPF items; locate the connected-address and connected-data items.
    uint32_t connId = 0;
    bool haveConnId = false;
    const uint8_t *connData = nullptr;
    size_t connDataLen = 0;
    for (uint16_t i = 0; i < itemCount; ++i) {
        if (remaining < 4) {
            return Status::Error;
        }
        uint16_t type = getU16(p);
        uint16_t len = getU16(p + 2);
        p += 4;
        remaining -= 4;
        if (len > remaining) {
            return Status::Error;
        }
        if (type == 0x00A1) {  // connected address item
            if (len < 4) {
                return Status::Error;
            }
            connId = getU32(p);
            haveConnId = true;
        } else if (type == 0x00B1) {  // connected data item
            connData = p;
            connDataLen = len;
        }
        p += len;
        remaining -= len;
    }

    if (!haveConnId || connData == nullptr || connDataLen < 4) {
        return Status::Error;
    }

    // Validate the connection ID (must be the negotiated T->O ID). Rejects
    // data addressed to a different connection (misrouted/replayed).
    if (connId != toConnId_) {
        return Status::Error;
    }

    // Validate the sequence number (must match the request). Rejects stale or
    // replayed responses.
    uint16_t seq = getU16(connData);
    if (seq != sentSequence_) {
        return Status::Error;
    }

    // Parse the CIP response: reply service, reserved, general status, ext status size.
    const uint8_t *cip = connData + 2;
    size_t cipLen = connDataLen - 2;
    if (cipLen < 4) {
        return Status::Error;
    }
    replyService_ = cip[0];
    resultCode_ = cip[2];
    data_ = cip + 4;
    dataLen_ = cipLen - 4;

    return Status::Ok;
}

}  // namespace clx


