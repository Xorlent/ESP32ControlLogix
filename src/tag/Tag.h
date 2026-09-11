#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../transport/Status.h"
#include "../cip/Cip.h"
#include "../cip/ExplicitMessage.h"

namespace clx {

class TcpConnection;

/*
 * A Logix tag: symbolic name, typed data buffer, and non-blocking Read Tag /
 * Write Tag (unconnected messaging) over a TcpConnection.
 *
 * read() starts a Read Tag (0x4C) exchange; write() starts a Write Tag (0x4D)
 * exchange using the current data buffer and data type. poll() advances the
 * exchange. Typed accessors read/write the data buffer at a byte offset.
 *
 * The data type is learned from a read (or set explicitly via setDataType());
 * a write requires a known data type.
 *
 */
class Tag {
public:
    // Maximum tag data buffer size (bytes).
    static constexpr size_t kMaxDataSize = 256;

    Tag() = default;
    ~Tag();

    Tag(const Tag &) = delete;
    Tag &operator=(const Tag &) = delete;

    // Start a Read Tag for the named tag (elementCount elements). msg is the
    // shared message that carries the exchange. cpuSlot routes the request
    // through the backplane to that CPU slot (kNoRoute = direct).
    Status read(ExplicitMessage &msg, TcpConnection &conn, uint32_t sessionHandle,
                const char *name, uint32_t elementCount, uint32_t timeoutMs,
                uint8_t cpuSlot = kNoRoute);

    // Start a Write Tag for the named tag using the current data buffer.
    Status write(ExplicitMessage &msg, TcpConnection &conn, uint32_t sessionHandle,
                 const char *name, uint32_t elementCount, uint32_t timeoutMs,
                 uint8_t cpuSlot = kNoRoute);

    // Advance the current read/write using the shared message.
    Status poll(ExplicitMessage &msg);

    // Abort an in-flight read/write, returning to Idle.
    void abort(ExplicitMessage &msg);

    // Current status (non-advancing): Ok once the last read/write completed,
    // Pending while in flight, Error on failure, NotReady if idle.
    Status status() const;

    // Data type code (valid after a read, or set explicitly).
    uint8_t dataType() const { return dataType_; }

    // Raw data buffer and length (valid after a read).
    const uint8_t *data() const { return data_; }
    size_t dataLength() const { return dataLen_; }

    // CIP result code of the last completed exchange (0 = success).
    uint8_t resultCode() const { return resultCode_; }

    // Set the data type explicitly (for writes without a prior read).
    void setDataType(uint8_t type) { dataType_ = type; }

    // Typed accessors (offset is a byte offset into the data buffer).
    bool getBool(size_t off) const;
    int8_t getInt8(size_t off) const;
    int16_t getInt16(size_t off) const;
    int32_t getInt32(size_t off) const;
    int64_t getInt64(size_t off) const;
    uint8_t getUint8(size_t off) const;
    uint16_t getUint16(size_t off) const;
    uint32_t getUint32(size_t off) const;
    uint64_t getUint64(size_t off) const;
    float getFloat32(size_t off) const;
    double getFloat64(size_t off) const;
    void setBool(size_t off, bool v);
    void setInt8(size_t off, int8_t v);
    void setInt16(size_t off, int16_t v);
    void setInt32(size_t off, int32_t v);
    void setInt64(size_t off, int64_t v);
    void setUint8(size_t off, uint8_t v);
    void setUint16(size_t off, uint16_t v);
    void setUint32(size_t off, uint32_t v);
    void setUint64(size_t off, uint64_t v);
    void setFloat32(size_t off, float v);
    void setFloat64(size_t off, double v);

    // Logix STRING accessors (4-byte length prefix + characters).
    size_t getString(char *buf, size_t bufLen) const;
    void setString(const char *s);

private:
    enum class State : uint8_t {
        Idle,
        Reading,
        Writing,
        Done,
        Failed,
    };

    State state_ = State::Idle;
    uint8_t data_[kMaxDataSize];
    size_t dataLen_ = 0;
    uint8_t dataType_ = 0;
    uint8_t resultCode_ = 0;

    Status startRead(ExplicitMessage &msg, TcpConnection &conn, uint32_t sessionHandle,
                     const char *name, uint32_t elementCount, uint32_t timeoutMs,
                     uint8_t cpuSlot);
    Status startWrite(ExplicitMessage &msg, TcpConnection &conn, uint32_t sessionHandle,
                      const char *name, uint32_t elementCount, uint32_t timeoutMs,
                      uint8_t cpuSlot);

    // True if [off, off+size) lies within the data buffer.
    bool inBounds(size_t off, size_t size) const;
};

}  // namespace clx
