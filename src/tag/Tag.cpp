#include "Tag.h"

#include <Arduino.h>
#include <string.h>

#include "../transport/TcpConnection.h"

namespace clx {

Tag::~Tag() {
    state_ = State::Idle;
}

Status Tag::read(ExplicitMessage &msg, TcpConnection &conn, uint32_t sessionHandle,
                 const char *name, uint32_t elementCount, uint32_t timeoutMs, uint8_t cpuSlot) {
    if (name == nullptr || strlen(name) > kMaxSymbolicName) {
        return Status::InvalidArg;
    }
    if (state_ == State::Reading || state_ == State::Writing) {
        return Status::Busy;
    }
    return startRead(msg, conn, sessionHandle, name, elementCount, timeoutMs, cpuSlot);
}

Status Tag::write(ExplicitMessage &msg, TcpConnection &conn, uint32_t sessionHandle,
                  const char *name, uint32_t elementCount, uint32_t timeoutMs, uint8_t cpuSlot) {
    if (name == nullptr || strlen(name) > kMaxSymbolicName) {
        return Status::InvalidArg;
    }
    if (dataType_ == 0) {
        return Status::NotReady;  // data type unknown; read or setDataType() first
    }
    if (state_ == State::Reading || state_ == State::Writing) {
        return Status::Busy;
    }
    return startWrite(msg, conn, sessionHandle, name, elementCount, timeoutMs, cpuSlot);
}

Status Tag::startRead(ExplicitMessage &msg, TcpConnection &conn, uint32_t sessionHandle,
                      const char *name, uint32_t elementCount, uint32_t timeoutMs, uint8_t cpuSlot) {
    uint8_t path[128];
    size_t pathLen = appendSymbolic(path, name);

    uint8_t count[2];
    putU16(count, uint16_t(elementCount));

    Status st;
    if (cpuSlot == kNoRoute) {
        st = msg.send(conn, sessionHandle, static_cast<uint8_t>(TagService::Read),
                      path, pathLen, count, sizeof(count), timeoutMs);
    } else {
        st = msg.sendRouted(conn, sessionHandle, cpuSlot, static_cast<uint8_t>(TagService::Read),
                            path, pathLen, count, sizeof(count), timeoutMs);
    }
    if (st != Status::Pending) {
        return st;
    }
    // Invalidate the previous result so a failed read cannot leave stale
    // data/result from an earlier exchange.
    dataLen_ = 0;
    resultCode_ = 0;
    state_ = State::Reading;
    return Status::Pending;
}

Status Tag::startWrite(ExplicitMessage &msg, TcpConnection &conn, uint32_t sessionHandle,
                       const char *name, uint32_t elementCount, uint32_t timeoutMs, uint8_t cpuSlot) {
    // Determine and validate the data length from the data type + element count.
    size_t dataLen;
    if (dataType_ == static_cast<uint8_t>(DataType::String)) {
        // STRING: 4-byte length prefix + characters.
        uint32_t strLen = getU32(data_);
        if (strLen > kMaxDataSize - 4) {
            return Status::InvalidArg;  // string length too large
        }
        dataLen = 4 + strLen;
    } else {
        size_t elemSize = dataTypeElementSize(static_cast<DataType>(dataType_));
        if (elemSize == 0 || elementCount > kMaxDataSize / elemSize) {
            return Status::InvalidArg;  // unknown data type or data too large
        }
        dataLen = elementCount * elemSize;
    }

    uint8_t path[128];
    size_t pathLen = appendSymbolic(path, name);

    // Write request data: element count (2) + data type (2) + tag data.
    uint8_t wdata[2 + 2 + kMaxDataSize];
    putU16(wdata, uint16_t(elementCount));
    putU16(wdata + 2, dataType_);
    memcpy(wdata + 4, data_, dataLen);
    size_t wdataLen = 4 + dataLen;

    Status st;
    if (cpuSlot == kNoRoute) {
        st = msg.send(conn, sessionHandle, static_cast<uint8_t>(TagService::Write),
                      path, pathLen, wdata, wdataLen, timeoutMs);
    } else {
        st = msg.sendRouted(conn, sessionHandle, cpuSlot, static_cast<uint8_t>(TagService::Write),
                            path, pathLen, wdata, wdataLen, timeoutMs);
    }
    if (st != Status::Pending) {
        return st;
    }
    state_ = State::Writing;
    return Status::Pending;
}

Status Tag::poll(ExplicitMessage &msg) {
    if (state_ != State::Reading && state_ != State::Writing) {
        switch (state_) {
            case State::Done:   return Status::Ok;
            case State::Failed: return Status::Error;
            default:            return Status::NotReady;
        }
    }

    Status st = msg.poll();
    if (st == Status::Pending) {
        return Status::Pending;
    }
    if (st != Status::Ok) {
        state_ = State::Failed;
        return st;
    }

    resultCode_ = msg.resultCode();

    if (resultCode_ != 0) {
        state_ = State::Failed;
        return Status::Error;
    }

    if (state_ == State::Reading && msg.dataLength() >= 2) {
        dataType_ = uint8_t(getU16(msg.data()));
        size_t n = msg.dataLength() - 2;
        if (n > kMaxDataSize) {
            n = kMaxDataSize;
        }
        memcpy(data_, msg.data() + 2, n);
        dataLen_ = n;
    }

    state_ = State::Done;
    return Status::Ok;
}

Status Tag::status() const {
    switch (state_) {
        case State::Reading:
        case State::Writing: return Status::Pending;
        case State::Done:    return Status::Ok;
        case State::Failed:  return Status::Error;
        case State::Idle:    return Status::NotReady;
    }
    return Status::Error;
}

void Tag::abort(ExplicitMessage &msg) {
    // Only abort the shared message if this tag is the one using it. With a
    // shared message, aborting an idle tag must not disturb another tag's
    // in-flight operation.
    if (state_ == State::Reading || state_ == State::Writing) {
        msg.abort();
    }
    state_ = State::Idle;
    dataLen_ = 0;
}

bool Tag::getBool(size_t off) const {
    return inBounds(off, 1) && data_[off] != 0;
}

int8_t Tag::getInt8(size_t off) const {
    return inBounds(off, 1) ? int8_t(data_[off]) : 0;
}

int16_t Tag::getInt16(size_t off) const {
    return inBounds(off, 2) ? int16_t(getU16(data_ + off)) : 0;
}

int32_t Tag::getInt32(size_t off) const {
    return inBounds(off, 4) ? int32_t(getU32(data_ + off)) : 0;
}

int64_t Tag::getInt64(size_t off) const {
    return inBounds(off, 8) ? int64_t(getU64(data_ + off)) : 0;
}

uint8_t Tag::getUint8(size_t off) const {
    return inBounds(off, 1) ? data_[off] : 0;
}

uint16_t Tag::getUint16(size_t off) const {
    return inBounds(off, 2) ? getU16(data_ + off) : 0;
}

uint32_t Tag::getUint32(size_t off) const {
    return inBounds(off, 4) ? getU32(data_ + off) : 0;
}

uint64_t Tag::getUint64(size_t off) const {
    return inBounds(off, 8) ? getU64(data_ + off) : 0;
}

float Tag::getFloat32(size_t off) const {
    if (!inBounds(off, 4)) {
        return 0.0f;
    }
    float v;
    memcpy(&v, data_ + off, sizeof(v));
    return v;
}

double Tag::getFloat64(size_t off) const {
    if (!inBounds(off, 8)) {
        return 0.0;
    }
    double v;
    memcpy(&v, data_ + off, sizeof(v));
    return v;
}

void Tag::setBool(size_t off, bool v) {
    if (inBounds(off, 1)) {
        data_[off] = v ? 1 : 0;
    }
}

void Tag::setInt8(size_t off, int8_t v) {
    if (inBounds(off, 1)) {
        data_[off] = uint8_t(v);
    }
}

void Tag::setInt16(size_t off, int16_t v) {
    if (inBounds(off, 2)) {
        putU16(data_ + off, uint16_t(v));
    }
}

void Tag::setInt32(size_t off, int32_t v) {
    if (inBounds(off, 4)) {
        putU32(data_ + off, uint32_t(v));
    }
}

void Tag::setInt64(size_t off, int64_t v) {
    if (inBounds(off, 8)) {
        putU64(data_ + off, uint64_t(v));
    }
}

void Tag::setUint8(size_t off, uint8_t v) {
    if (inBounds(off, 1)) {
        data_[off] = v;
    }
}

void Tag::setUint16(size_t off, uint16_t v) {
    if (inBounds(off, 2)) {
        putU16(data_ + off, v);
    }
}

void Tag::setUint32(size_t off, uint32_t v) {
    if (inBounds(off, 4)) {
        putU32(data_ + off, v);
    }
}

void Tag::setUint64(size_t off, uint64_t v) {
    if (inBounds(off, 8)) {
        putU64(data_ + off, v);
    }
}

void Tag::setFloat32(size_t off, float v) {
    if (inBounds(off, 4)) {
        memcpy(data_ + off, &v, sizeof(v));
    }
}

void Tag::setFloat64(size_t off, double v) {
    if (inBounds(off, 8)) {
        memcpy(data_ + off, &v, sizeof(v));
    }
}

size_t Tag::getString(char *buf, size_t bufLen) const {
    if (dataLen_ < 4 || buf == nullptr || bufLen == 0) {
        return 0;
    }
    uint32_t len = getU32(data_);  // Logix STRING length (DINT)
    size_t n = (len < dataLen_ - 4) ? len : dataLen_ - 4;
    if (n > bufLen - 1) {
        n = bufLen - 1;
    }
    memcpy(buf, data_ + 4, n);
    buf[n] = 0;
    return n;
}

void Tag::setString(const char *s) {
    if (s == nullptr) {
        return;
    }
    size_t len = strlen(s);
    if (len > kMaxDataSize - 4) {
        len = kMaxDataSize - 4;
    }
    putU32(data_, uint32_t(len));
    memcpy(data_ + 4, s, len);
    dataLen_ = 4 + len;
}

bool Tag::inBounds(size_t off, size_t size) const {
    return size <= kMaxDataSize && off <= kMaxDataSize - size;
}

}  // namespace clx

