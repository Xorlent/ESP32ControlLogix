#include "PlcClient.h"

#include <Arduino.h>
#include <string.h>

namespace clx {

PlcClient::~PlcClient() {
    session_.abort();
    tcp_.close();
}

Status PlcClient::begin(const Client::Config &cfg) {
    state_ = State::Ethernet;
    wantConnect_ = false;
    return eth_.begin(cfg);
}

Status PlcClient::connect(const IPAddress &ip, uint16_t port, uint32_t timeoutMs) {
    remoteIp_ = ip;
    remotePort_ = port;
    connectTimeoutMs_ = timeoutMs;
    wantConnect_ = false;
    if (eth_.ready()) {
        // Ethernet already ready; start TCP connect now (initial or reconnect).
        // Drop any previous session so a hot reconnect cannot reuse a stale
        // session handle (Session::open() would otherwise return Busy).
        session_.abort();
        tcp_.close();
        state_ = State::Connecting;
        deadline_ = millis() + timeoutMs;
        Status st = tcp_.connect(remoteIp_, remotePort_, timeoutMs);
        if (st != Status::Pending) {
            state_ = State::Failed;
            return st;
        }
    } else {
        // Ethernet not ready yet; queue the connect.
        wantConnect_ = true;
        state_ = State::Ethernet;
    }
    return Status::Pending;
}

Status PlcClient::disconnect() {
    if (state_ == State::Ready) {
        state_ = State::Disconnecting;
        session_.close();
        return Status::Pending;
    }
    session_.abort();
    tcp_.close();
    state_ = State::Idle;
    return Status::Ok;
}

void PlcClient::setCpuSlot(uint8_t slot) {
    cpuSlot_ = slot;
}

Status PlcClient::poll() {
    switch (state_) {
        case State::Ethernet: {
            Status st = eth_.poll();
            if (st == Status::Ok) {
                if (wantConnect_) {
                    wantConnect_ = false;  // consume the queued connect
                    state_ = State::Connecting;
                    // Reset the deadline now that the TCP connect actually
                    // starts, so the timeout covers only TCP + session (not
                    // the Ethernet setup time).
                    deadline_ = millis() + connectTimeoutMs_;
                    Status cst = tcp_.connect(remoteIp_, remotePort_, connectTimeoutMs_);
                    if (cst != Status::Pending) {
                        state_ = State::Failed;
                        return cst;
                    }
                    return Status::Pending;
                }
                return Status::Ok;  // Ethernet ready, no connect requested
            }
            if (st == Status::Error) {
                state_ = State::Failed;
                return st;
            }
            return Status::Pending;
        }
        case State::Connecting: {
            if ((int32_t)(millis() - deadline_) >= 0) {
                state_ = State::Failed;
                return Status::Timeout;
            }
            Status st = tcp_.poll();
            if (st == Status::Ok) {
                Status openSt = session_.open(tcp_, deadline_ - millis());
                if (openSt != Status::Pending) {
                    state_ = State::Failed;
                    return openSt;
                }
                state_ = State::Registering;
                return Status::Pending;
            }
            if (st == Status::Timeout || st == Status::Error) {
                state_ = State::Failed;
                return st;
            }
            return Status::Pending;
        }
        case State::Registering: {
            if ((int32_t)(millis() - deadline_) >= 0) {
                state_ = State::Failed;
                return Status::Timeout;
            }
            Status st = session_.poll();
            if (st == Status::Ok) {
                state_ = State::Ready;
                if (stateCallback_) {
                    stateCallback_(Status::Ok, stateUserData_);
                }
                return Status::Ok;
            }
            if (st == Status::Timeout || st == Status::Error) {
                state_ = State::Failed;
                return st;
            }
            return Status::Pending;
        }
        case State::Ready: {
            bool disconnected = false;
            for (size_t i = 0; i < kMaxTags; ++i) {
                if (!inUse_[i]) {
                    continue;
                }
                Status prev = tagPrevStatus_[i];
                Status st = tags_[i].poll(msg_);
                tagPrevStatus_[i] = st;
                if (st == Status::Closed) {
                    disconnected = true;  // connection lost (PLC restart / drop)
                }
                if (prev == Status::Pending && st != Status::Pending && tagCallback_) {
                    tagCallback_(int(i), st, tagUserData_);
                }
            }
            if (disconnected) {
                session_.abort();
                tcp_.close();
                state_ = State::Idle;
                if (stateCallback_) {
                    stateCallback_(Status::Closed, stateUserData_);
                }
                return Status::Closed;
            }
            return Status::Ok;
        }
        case State::Disconnecting: {
            Status st = session_.poll();
            if (st == Status::Ok || st == Status::Closed) {
                tcp_.close();
                state_ = State::Idle;
                if (stateCallback_) {
                    stateCallback_(Status::Closed, stateUserData_);
                }
                return Status::Ok;
            }
            return Status::Pending;
        }
        case State::Failed:
            return Status::Error;
        case State::Idle:
            return Status::NotReady;
    }
    return Status::Error;
}

int PlcClient::createTag(const char *name, uint32_t elementCount) {
    if (name == nullptr || name[0] == 0 || strlen(name) >= kMaxTagName) {
        return static_cast<int>(Status::InvalidArg);
    }
    int h = findFreeTag();
    if (h < 0) {
        return static_cast<int>(Status::NoMemory);
    }
    strncpy(tagNames_[h], name, kMaxTagName - 1);
    tagNames_[h][kMaxTagName - 1] = 0;
    tagElemCount_[h] = elementCount;
    tagPrevStatus_[h] = Status::NotReady;
    inUse_[h] = true;
    return h;
}

Status PlcClient::destroyTag(int handle) {
    if (!validHandle(handle)) {
        return Status::InvalidArg;
    }
    inUse_[handle] = false;
    return Status::Ok;
}

Status PlcClient::read(int handle, uint32_t timeoutMs) {
    if (!validHandle(handle)) {
        return Status::InvalidArg;
    }
    if (state_ != State::Ready) {
        return Status::NotReady;
    }
    if (anyTagBusy()) {
        return Status::Busy;  // single in-flight: one tag operation at a time
    }
    return tags_[handle].read(msg_, tcp_, session_.handle(), tagNames_[handle],
                              tagElemCount_[handle], timeoutMs, cpuSlot_);
}

Status PlcClient::write(int handle, uint32_t timeoutMs) {
    if (!validHandle(handle)) {
        return Status::InvalidArg;
    }
    if (state_ != State::Ready) {
        return Status::NotReady;
    }
    if (anyTagBusy()) {
        return Status::Busy;  // single in-flight: one tag operation at a time
    }
    return tags_[handle].write(msg_, tcp_, session_.handle(), tagNames_[handle],
                               tagElemCount_[handle], timeoutMs, cpuSlot_);
}

Status PlcClient::tagStatus(int handle) const {
    if (!validHandle(handle)) {
        return Status::InvalidArg;
    }
    return tags_[handle].status();
}

Tag *PlcClient::tag(int handle) {
    if (!validHandle(handle)) {
        return nullptr;
    }
    return &tags_[handle];
}

Status PlcClient::abortTag(int handle) {
    if (!validHandle(handle)) {
        return Status::InvalidArg;
    }
    tags_[handle].abort(msg_);
    tagPrevStatus_[handle] = Status::NotReady;
    return Status::Ok;
}

void PlcClient::setTagCallback(TagCallback cb, void *userData) {
    tagCallback_ = cb;
    tagUserData_ = userData;
}

void PlcClient::setStateCallback(StateCallback cb, void *userData) {
    stateCallback_ = cb;
    stateUserData_ = userData;
}

bool PlcClient::ready() const {
    return state_ == State::Ready;
}

uint32_t PlcClient::sessionHandle() const {
    return session_.handle();
}

int PlcClient::tagCount() const {
    int n = 0;
    for (size_t i = 0; i < kMaxTags; ++i) {
        if (inUse_[i]) {
            ++n;
        }
    }
    return n;
}

int PlcClient::findFreeTag() const {
    for (size_t i = 0; i < kMaxTags; ++i) {
        if (!inUse_[i]) {
            return int(i);
        }
    }
    return -1;
}

bool PlcClient::anyTagBusy() const {
    for (size_t i = 0; i < kMaxTags; ++i) {
        if (inUse_[i] && tags_[i].status() == Status::Pending) {
            return true;
        }
    }
    return false;
}

bool PlcClient::validHandle(int handle) const {
    return handle >= 0 && handle < int(kMaxTags) && inUse_[handle];
}

}  // namespace clx
