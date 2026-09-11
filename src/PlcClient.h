#pragma once

#include <stddef.h>
#include <stdint.h>

#include <IPAddress.h>

#include "transport/Status.h"
#include "transport/Client.h"
#include "transport/TcpConnection.h"
#include "eip/Session.h"
#include "cip/ExplicitMessage.h"
#include "tag/Tag.h"

// Maximum number of simultaneous tags (bounded pool). Override by defining
// ESP32_CONTROLLOGIX_MAX_TAGS before including this header, e.g.:
//   #define ESP32_CONTROLLOGIX_MAX_TAGS 16
//   #include <ESP32ControlLogix.h>
#ifndef ESP32_CONTROLLOGIX_MAX_TAGS
#define ESP32_CONTROLLOGIX_MAX_TAGS 8
#endif

namespace clx {

// Tag-completion callback: invoked once when a tag's read/write completes.
using TagCallback = void (*)(int handle, Status status, void *userData);

// Connection-state callback: invoked on connect (Ok) and disconnect (Closed).
using StateCallback = void (*)(Status status, void *userData);

/*
 * PlcClient - the top-level public API.
 *
 * Owns the full stack (Ethernet, TCP, session) and a bounded pool of tags.
 * All calls are non-blocking: begin()/connect()/read()/write() start work and
 * return immediately; poll() advances everything.
 *
 * Bounded resources: the tag pool is a fixed array (kMaxTags). createTag()
 * returns a defined error when the pool is exhausted, and never leaves a
 * partially-registered tag. There is no heap allocation after construction.
 *
 * Single in-flight: only one tag read/write runs at a time. read()/write()
 * return Busy while another operation is in flight, so the shared connection
 * is never contended. Wait for the current operation to complete (tagStatus()
 * != Pending) before starting the next.
 *
 * Usage:
 *   PlcClient plc;
 *   plc.begin(cfg);                       // start Ethernet
 *   plc.connect(ip, 44818, 5000);         // queue TCP + RegisterSession
 *   while (!plc.ready()) plc.poll();      // advance until connected
 *   int t = plc.createTag("MyTag");        // allocate a tag (1 element)
 *   plc.read(t, 5000);                    // start a read
 *   while (plc.tagStatus(t) == Status::Pending) plc.poll();
 *   int32_t v = plc.tag(t)->getInt32(0);  // read the value
 *   plc.destroyTag(t);
 */
class PlcClient {
public:
    // Maximum number of simultaneous tags (bounded pool).
    static constexpr size_t kMaxTags = ESP32_CONTROLLOGIX_MAX_TAGS;
    // Maximum tag name length.
    static constexpr size_t kMaxTagName = 64;

    PlcClient() = default;
    ~PlcClient();

    PlcClient(const PlcClient &) = delete;
    PlcClient &operator=(const PlcClient &) = delete;

    // Start Ethernet setup and return immediately.
    Status begin(const Client::Config &cfg);

    // Queue a TCP connect + RegisterSession for the given target. The connect
    // begins once Ethernet is ready (advanced by poll()).
    Status connect(const IPAddress &ip, uint16_t port, uint32_t timeoutMs);

    // Advance Ethernet, connection, session, and the in-flight tag (at most one).
    Status poll();

    // Begin a graceful disconnect (UnregisterSession + close).
    Status disconnect();

    // Route tag read/write through the backplane to the CPU in `slot` (0..16).
    // Use this when connecting to a ControlLogix Ethernet module (e.g. 1756-EN2T)
    // rather than the controller's own embedded EtherNet/IP port. Pass kNoRoute
    // (the default) for direct access with no route.
    void setCpuSlot(uint8_t slot);

    // --- Tag registry (bounded) ---

    // Allocate a tag for the named symbolic tag. elementCount defaults to 1
    // (a single element). Returns a handle >= 0, or a negative Status
    // (NoMemory if the pool is exhausted).
    int createTag(const char *name, uint32_t elementCount = 1);

    // Free a tag. Returns Ok, or InvalidArg if the handle is invalid.
    Status destroyTag(int handle);

    // --- Tag operations ---

    // Start a read/write on a tag. Returns Pending while in flight, or Busy if
    // another tag operation is already in flight (single in-flight convention:
    // wait for the current operation to complete before starting another).
    Status read(int handle, uint32_t timeoutMs);
    Status write(int handle, uint32_t timeoutMs);

    // Current tag status (non-advancing).
    Status tagStatus(int handle) const;

    // Access the underlying tag (for typed accessors). Returns nullptr when
    // the handle is invalid.
    Tag *tag(int handle);

    // Abort an in-flight tag operation, returning it to Idle.
    Status abortTag(int handle);

    // Register optional callbacks.
    void setTagCallback(TagCallback cb, void *userData);
    void setStateCallback(StateCallback cb, void *userData);

    // --- Status ---

    // True once Ethernet + session are ready.
    bool ready() const;

    // The registered session handle (valid once ready()).
    uint32_t sessionHandle() const;

    // Number of allocated tags.
    int tagCount() const;

private:
    enum class State : uint8_t {
        Idle,
        Ethernet,
        Connecting,
        Registering,
        Ready,
        Disconnecting,
        Failed,
    };

    Client eth_;
    TcpConnection tcp_;
    Session session_;
    ExplicitMessage msg_;  // single shared message (one tag operation at a time)
    State state_ = State::Idle;

    IPAddress remoteIp_;
    uint16_t remotePort_ = 0;
    bool wantConnect_ = false;
    uint32_t connectTimeoutMs_ = 0;
    uint32_t deadline_ = 0;

    // Backplane route target for tag I/O (kNoRoute = direct, 0..16 = CPU slot).
    uint8_t cpuSlot_ = kNoRoute;

    // Bounded tag pool.
    Tag tags_[kMaxTags];
    bool inUse_[kMaxTags] = {};
    char tagNames_[kMaxTags][kMaxTagName];
    uint32_t tagElemCount_[kMaxTags] = {};
    Status tagPrevStatus_[kMaxTags] = {};

    // Callbacks.
    TagCallback tagCallback_ = nullptr;
    void *tagUserData_ = nullptr;
    StateCallback stateCallback_ = nullptr;
    void *stateUserData_ = nullptr;

    int findFreeTag() const;

    // True if any allocated tag currently has a read/write in flight.
    bool anyTagBusy() const;

    // True if handle is a currently-allocated tag index.
    bool validHandle(int handle) const;
};

}  // namespace clx
