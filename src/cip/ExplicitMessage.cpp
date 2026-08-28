#include "ExplicitMessage.h"
#include "Cip.h"

#include <Arduino.h>
#include <string.h>

#include "../transport/TcpConnection.h"

namespace clx {

ExplicitMessage::~ExplicitMessage() {
    conn_ = nullptr;
    state_ = State::Idle;
}

void ExplicitMessage::abort() {
    conn_ = nullptr;
    state_ = State::Idle;
}

Status ExplicitMessage::send(TcpConnection &conn, uint32_t sessionHandle, uint8_t service,
                             const uint8_t *path, size_t pathLen,
                             const uint8_t *data, size_t dataLen, uint32_t timeoutMs) {
    if (state_ == State::Sending || state_ == State::Receiving) {
        return Status::Busy;
    }
    if (!conn.connected()) {
        return Status::NotReady;
    }

    // CIP request = service (1) + path size in words (1) + path + data.
    const size_t cipLen = 2 + pathLen + dataLen;
    if (cipLen > kMaxCipData || (pathLen & 1) != 0 ||
        (pathLen > 0 && path == nullptr) || (dataLen > 0 && data == nullptr)) {
        return Status::InvalidArg;
    }

    return startSend(conn, sessionHandle, service, path, pathLen, data, dataLen, timeoutMs);
}

Status ExplicitMessage::sendRouted(TcpConnection &conn, uint32_t sessionHandle, uint8_t slot,
                                   uint8_t service, const uint8_t *path, size_t pathLen,
                                   const uint8_t *data, size_t dataLen, uint32_t timeoutMs) {
    if (state_ == State::Sending || state_ == State::Receiving) {
        return Status::Busy;
    }
    if (!conn.connected()) {
        return Status::NotReady;
    }
    if (slot > 16) {  // 17-slot 1756 chassis (slots 0..16)
        return Status::InvalidArg;
    }

    // Embedded request = service (1) + path size (1) + path + data.
    const size_t embeddedLen = 2 + pathLen + dataLen;
    if (embeddedLen > kMaxCipData || (pathLen & 1) != 0 ||
        (pathLen > 0 && path == nullptr) || (dataLen > 0 && data == nullptr)) {
        return Status::InvalidArg;
    }

    // Build the Unconnected Send (0x52) request data:
    //   priority(1) + timeout(1) + length(2) + embedded request + pad + route(4).
    uint8_t wrapper[kRoutedOverhead + kMaxCipData];
    size_t w = 0;
    wrapper[w++] = 0x0A;                        // priority/tick time
    wrapper[w++] = 0x05;                        // timeout ticks
    putU16(wrapper + w, uint16_t(embeddedLen)); // embedded message length
    w += 2;
    wrapper[w++] = service;                     // embedded service
    wrapper[w++] = uint8_t(pathLen / 2);        // embedded path size (words)
    if (pathLen) {
        memcpy(wrapper + w, path, pathLen);
        w += pathLen;
    }
    if (dataLen) {
        memcpy(wrapper + w, data, dataLen);
        w += dataLen;
    }
    if (embeddedLen & 1) {
        wrapper[w++] = 0x00;                    // pad to a whole 16-bit word
    }
    w += appendBackplaneRoute(wrapper + w, slot);  // route: port 1 = backplane, link = slot

    // Connection Manager path (class 6, instance 1).
    const uint8_t cmPath[4] = {0x20, kConnectionManagerClass, 0x24, 0x01};

    return startSend(conn, sessionHandle, kUnconnectedSend, cmPath, sizeof(cmPath),
                     wrapper, w, timeoutMs);
}

Status ExplicitMessage::startSend(TcpConnection &conn, uint32_t sessionHandle, uint8_t service,
                                  const uint8_t *path, size_t pathLen,
                                  const uint8_t *data, size_t dataLen, uint32_t timeoutMs) {
    const size_t cipLen = 2 + pathLen + dataLen;
    conn_ = &conn;

    // Encapsulation header.
    EncapsulationHeader h;
    h.command = static_cast<uint16_t>(Command::SendRRData);
    h.length = uint16_t(16 + cipLen);
    h.session = sessionHandle;
    h.status = 0;
    h.context = context_;
    h.options = 0;
    encodeHeader(tx_, h);

    // Body: interface handle, timeout, CPF item count, two CPF items.
    uint8_t *body = tx_ + kEncapsulationHeaderSize;
    putU32(body, 0);                          // interface handle
    putU16(body + 4, 10);                     // timeout (ticks)
    putU16(body + 6, 2);                      // CPF item count = 2
    putU16(body + 8, 0x0000);                 // item 1: null address type
    putU16(body + 10, 0);                     // item 1: length 0
    putU16(body + 12, 0x00B2);                // item 2: unconnected data type
    putU16(body + 14, uint16_t(cipLen));      // item 2: length

    // CIP request payload.
    uint8_t *cip = body + 16;
    cip[0] = service;
    cip[1] = uint8_t(pathLen / 2);            // path size in 16-bit words
    if (pathLen) {
        memcpy(cip + 2, path, pathLen);
    }
    if (dataLen) {
        memcpy(cip + 2 + pathLen, data, dataLen);
    }

    txLen_ = kEncapsulationHeaderSize + 16 + cipLen;
    txSent_ = 0;
    rxLen_ = 0;
    rxExpected_ = kEncapsulationHeaderSize;
    sentContext_ = context_;
    ++context_;
    deadline_ = millis() + timeoutMs;
    state_ = State::Sending;
    return Status::Pending;
}

Status ExplicitMessage::poll() {
    switch (state_) {
        case State::Sending:
        case State::Receiving: {
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
            state_ = State::Receiving;
            st = readResponse();
            if (st == Status::Pending) {
                return Status::Pending;
            }
            if (st != Status::Ok) {
                state_ = State::Failed;
                return st;
            }
            st = parseResponse();
            state_ = (st == Status::Ok) ? State::Done : State::Failed;
            return st;
        }
        case State::Done:   return Status::Ok;
        case State::Failed: return Status::Error;
        case State::Idle:   return Status::NotReady;
    }
    return Status::Error;
}

Status ExplicitMessage::writePending() {
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

Status ExplicitMessage::readResponse() {
    while (rxLen_ < rxExpected_) {
        int n = conn_->read(rx_ + rxLen_, rxExpected_ - rxLen_);
        if (n > 0) {
            rxLen_ += size_t(n);
            if (rxLen_ == kEncapsulationHeaderSize) {
                EncapsulationHeader h = decodeHeader(rx_);
                if (h.length > 16 + kMaxCipData + kRoutedOverhead) {
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

Status ExplicitMessage::parseResponse() {
    EncapsulationHeader h = decodeHeader(rx_);
    if (h.command != static_cast<uint16_t>(Command::SendRRData) || h.status != 0) {
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

    // Locate the unconnected-data item (type 0x00B2).
    const uint8_t *cipData = nullptr;
    size_t cipLen = 0;
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
        if (type == 0x00B2) {
            cipData = p;
            cipLen = len;
        }
        p += len;
        remaining -= len;
    }

    if (cipData == nullptr || cipLen < 4) {
        return Status::Error;
    }

    // CIP response: reply service, reserved, general status, ext status size.
    replyService_ = cipData[0];
    resultCode_ = cipData[2];
    data_ = cipData + 4;
    dataLen_ = cipLen - 4;

    return Status::Ok;
}

}  // namespace clx

