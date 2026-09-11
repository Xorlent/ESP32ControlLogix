#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../transport/Status.h"
#include "../eip/Encapsulation.h"
#include "Cip.h"
#include "ExplicitMessage.h"

namespace clx {

class TcpConnection;

/*
 * A connected CIP connection (Forward Open / SendUnitData / Forward Close).
 *
 * open() performs Forward Open and negotiates the O->T / T->O connection IDs.
 * send() performs a connected SendUnitData exchange (e.g. Read/Write Tag over
 * the connection). close() performs Forward Close. poll() advances the state
 * machine. No call blocks.
 *
 * Security/robustness: every received packet is bounds-checked, and connected
 * responses are rejected unless the connection ID matches the negotiated T->O
 * ID and the sequence number matches the request. This rejects stale, replayed,
 * misrouted, and malformed data.
 */
class Connection {
public:
    // Maximum connected CIP payload size (bytes). 508 is the legacy Forward Open
    // ceiling (the standard Logix connected-explicit message size). The tx_/rx_
    // buffers below are sized encap(24) + connected overhead(22) + this value.
    static constexpr size_t kMaxDataSize = 508;

    Connection() = default;
    ~Connection();

    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    // Forward Open for the named symbolic tag. cpuSlot routes the connection
    // path through the backplane to that CPU slot (kNoRoute = direct).
    Status open(TcpConnection &conn, uint32_t sessionHandle, const char *tagName,
                uint32_t timeoutMs, uint8_t cpuSlot = kNoRoute);

    // Send a connected CIP request (service + path + data).
    Status send(TcpConnection &conn, uint32_t sessionHandle, uint8_t service,
                const uint8_t *path, size_t pathLen,
                const uint8_t *data, size_t dataLen, uint32_t timeoutMs);

    // Forward Close.
    Status close(TcpConnection &conn, uint32_t sessionHandle, uint32_t timeoutMs);

    // Advance the state machine.
    Status poll();

    // Connection IDs (valid once open). otConnId_ holds the O->T ID (assigned
    // by the target) and toConnId_ holds the T->O ID (assigned by this device).
    uint32_t originatorConnectionId() const { return otConnId_; }
    uint32_t targetConnectionId() const { return toConnId_; }

    // True once the connection is open.
    bool isOpen() const { return state_ == State::Open; }

    // Response accessors (valid after a successful send()).
    uint8_t replyService() const { return replyService_; }
    uint8_t resultCode() const { return resultCode_; }
    const uint8_t *data() const { return data_; }
    size_t dataLength() const { return dataLen_; }

private:
    enum class State : uint8_t {
        Idle,
        Opening,
        Open,
        Sending,
        Closing,
        Closed,
        Failed,
    };

    ExplicitMessage fwd_;  // Forward Open/Close (unconnected SendRRData)
    TcpConnection *conn_ = nullptr;
    State state_ = State::Idle;

    uint32_t otConnId_ = 0;   // O->T connection ID (assigned by the target)
    uint32_t toConnId_ = 0;   // T->O connection ID (assigned by this device)
    uint16_t sequence_ = 1;   // connected sequence number
    uint16_t connSerial_ = 0; // connection serial number (Forward Open/Close)
    char tagName_[64] = {};   // tag name (for Forward Close)
    uint8_t cpuSlot_ = kNoRoute;  // route target for the connection path

    // Connected (SendUnitData) transmit/receive buffers. The connected body
    // overhead is 22 bytes: interface handle (4) + timeout (2) + item count (2)
    // + connected-address item (8) + connected-data item header (4) + sequence (2).
    uint8_t tx_[kEncapsulationHeaderSize + 22 + kMaxDataSize];
    size_t txLen_ = 0;
    size_t txSent_ = 0;
    uint8_t rx_[kEncapsulationHeaderSize + 22 + kMaxDataSize];
    size_t rxLen_ = 0;
    size_t rxExpected_ = 0;

    uint8_t replyService_ = 0;
    uint8_t resultCode_ = 0;
    const uint8_t *data_ = nullptr;
    size_t dataLen_ = 0;

    uint32_t deadline_ = 0;
    uint64_t context_ = 1;
    uint64_t sentContext_ = 0;
    uint16_t sentSequence_ = 0;

    Status startOpen(TcpConnection &conn, uint32_t sessionHandle, const char *tagName,
                     uint32_t timeoutMs, uint8_t cpuSlot);
    Status startSend(TcpConnection &conn, uint32_t sessionHandle, uint8_t service,
                     const uint8_t *path, size_t pathLen,
                     const uint8_t *data, size_t dataLen, uint32_t timeoutMs);
    Status startClose(TcpConnection &conn, uint32_t sessionHandle, uint32_t timeoutMs);
    // Encode the connection path (route + Message Router + symbolic tag, or just
    // the symbolic tag when direct). Returns bytes written (a whole number of words).
    size_t buildConnectionPath(uint8_t *out, const char *tagName) const;
    Status pollOpening();
    Status pollSending();
    Status pollClosing();
    Status writePending();
    Status readResponse();
    Status parseConnectedResponse();
};

}  // namespace clx
