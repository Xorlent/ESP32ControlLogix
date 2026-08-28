#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace clx {

/*
 * CIP (Common Industrial Protocol) constants and path encoding helpers.
 * CIP Vol 1 (services, path segments, status) and Vol 2 (CPF item types).
 */

// CIP service codes (CIP Vol 1, Appendix C).
enum class Service : uint8_t {
    GetAttributeAll = 0x01,
    SetAttributeAll = 0x02,
    GetAttributeList = 0x03,
    SetAttributeList = 0x04,
    Reset = 0x05,
    Start = 0x06,
    Stop = 0x07,
    Create = 0x08,
    Delete = 0x09,
    MultipleServicePacket = 0x0A,
    ApplyAttributes = 0x0D,
    GetAttributeSingle = 0x0E,
    SetAttributeSingle = 0x10,
    FindNextObjectInstance = 0x11,
    Restore = 0x15,
    Save = 0x16,
    NoOperation = 0x17,
    GetMember = 0x18,
    SetMember = 0x19,
    InsertMember = 0x1A,
    RemoveMember = 0x1B,
    // Rockwell custom: enumerate the instances (and selected attribute values)
    // of an object such as the Logix Symbol Object (class 0x6B).
    GetInstanceAttributeList = 0x55,
};

// CIP path segment type codes (encoded in the high 3 bits of the first byte).
enum : uint8_t {
    kSegmentClassId8 = 0x20,      // 8-bit class ID
    kSegmentClassId16 = 0x21,     // 16-bit class ID
    kSegmentInstanceId8 = 0x24,   // 8-bit instance ID
    kSegmentInstanceId16 = 0x25,  // 16-bit instance ID
    kSegmentElementId8 = 0x28,    // 8-bit element ID
    kSegmentElementId16 = 0x29,   // 16-bit element ID
    kSegmentAttributeId8 = 0x30,  // 8-bit attribute ID
    kSegmentAttributeId16 = 0x31, // 16-bit attribute ID
    kSegmentInstanceId32 = 0x26,  // 32-bit instance ID
    kSegmentSymbolic = 0x91,      // extended symbol segment (data segment, 1-byte length)
};

// CIP Connection Manager object (class 0x06).
constexpr uint8_t kConnectionManagerClass = 0x06;

// Connection Manager "Unconnected Send" service. (0x52 is also "Read Tag
// Fragmented" in the Logix tag-service context; here it routes an embedded
// request through the Connection Manager to another module.)
constexpr uint8_t kUnconnectedSend = 0x52;

// CIP Symbol Object (class 0x6B) - the Logix tag database.
constexpr uint8_t kSymbolClass = 0x6B;

// Symbol Object (class 0x6B) instance attribute IDs.
enum : uint8_t {
    kSymbolName = 1,    // Logix STRING (4-byte length + characters)
    kSymbolType = 2,    // UINT (struct flag / dimensions / elementary type code)
    kSymbolAddress = 3, // UDINT
};

// CIP general status codes (CIP Vol 1, Appendix B).
enum class GeneralStatus : uint8_t {
    Success = 0x00,
    ConnectionFailure = 0x01,
    ResourceUnavailable = 0x02,
    InvalidParameterValue = 0x03,
    PathSegmentError = 0x04,
    PathDestinationUnknown = 0x05,
    PartialTransfer = 0x06,
    ConnectionLost = 0x07,
    ServiceNotSupported = 0x08,
    InvalidAttributeValue = 0x09,
    AttributeListError = 0x0A,
    AlreadyInRequestedMode = 0x0B,
    ObjectStateConflict = 0x0C,
    ObjectAlreadyExists = 0x0D,
    AttributeNotSettable = 0x0E,
    PrivilegeViolation = 0x0F,
    DeviceStateConflict = 0x10,
    ReplyDataTooLarge = 0x11,
    NotEnoughData = 0x13,
    AttributeNotSupported = 0x14,
    TooMuchData = 0x15,
    ObjectDoesNotExist = 0x16,
    EmbeddedServiceError = 0x1E,
    InvalidParameter = 0x20,
    InvalidReplyReceived = 0x22,
    BufferOverflow = 0x23,
    PathSizeInvalid = 0x26,
};

// CPF item type codes (CIP Vol 2).
enum class CpfItemType : uint16_t {
    NullAddress = 0x0000,
    ListIdentity = 0x000C,
    ConnectedAddress = 0x00A1,
    ConnectedData = 0x00B1,
    UnconnectedData = 0x00B2,
    ListServices = 0x0100,
};

// CIP Identity Object (class 0x01) attribute IDs.
enum : uint8_t {
    kIdentityVendorId = 1,
    kIdentityDeviceType = 2,
    kIdentityProductCode = 3,
    kIdentityRevision = 4,
    kIdentityStatus = 5,
    kIdentitySerialNumber = 6,
    kIdentityProductName = 7,
    kIdentityState = 8,
};

// Append an 8-bit class segment; returns bytes written (2).
inline size_t appendClass(uint8_t *out, uint8_t classId) {
    out[0] = kSegmentClassId8;
    out[1] = classId;
    return 2;
}

// Append an 8-bit instance segment; returns bytes written (2).
inline size_t appendInstance(uint8_t *out, uint8_t instanceId) {
    out[0] = kSegmentInstanceId8;
    out[1] = instanceId;
    return 2;
}

// Append a 16-bit instance segment (padded to a whole word); returns 4.
inline size_t appendInstance16(uint8_t *out, uint16_t instanceId) {
    out[0] = kSegmentInstanceId16;
    out[1] = uint8_t(instanceId);
    out[2] = uint8_t(instanceId >> 8);
    out[3] = 0x00;  // pad the odd segment to a whole 16-bit word
    return 4;
}

// Append a 32-bit instance segment (padded to a whole word); returns 6.
inline size_t appendInstance32(uint8_t *out, uint32_t instanceId) {
    out[0] = kSegmentInstanceId32;
    out[1] = uint8_t(instanceId);
    out[2] = uint8_t(instanceId >> 8);
    out[3] = uint8_t(instanceId >> 16);
    out[4] = uint8_t(instanceId >> 24);
    out[5] = 0x00;  // pad the odd segment to a whole 16-bit word
    return 6;
}

// Append an 8-bit attribute segment; returns bytes written (2).
inline size_t appendAttribute(uint8_t *out, uint8_t attributeId) {
    out[0] = kSegmentAttributeId8;
    out[1] = attributeId;
    return 2;
}

// Maximum symbolic tag-name length. Bounded so the encoded path (2-byte header
// + name + optional pad) always fits in the library's 128-byte path buffers.
constexpr size_t kMaxSymbolicName = 120;

// Encode a symbolic (0x91, "extended symbol" data segment) segment for a tag
// name into out. Returns bytes written (including any pad byte to a whole word).
// The name is capped to kMaxSymbolicName to keep the output bounded.
inline size_t appendSymbolic(uint8_t *out, const char *name) {
    size_t len = strlen(name);
    if (len > kMaxSymbolicName) {
        len = kMaxSymbolicName;
    }
    out[0] = kSegmentSymbolic;
    out[1] = uint8_t(len);
    memcpy(out + 2, name, len);
    size_t total = 2 + len;
    if (len & 1) {
        out[total] = 0;  // pad to a whole 16-bit word
        ++total;
    }
    return total;
}

// Append a backplane route path (port 1 = backplane, link = slot) as a
// PADDED_EPATH: length(1 word) + pad + port + link. Returns bytes written (4).
inline size_t appendBackplaneRoute(uint8_t *out, uint8_t slot) {
    out[0] = 0x01;  // path length = 1 word
    out[1] = 0x00;  // pad byte
    out[2] = 0x01;  // port 1 = backplane
    out[3] = slot;  // link address = slot number
    return 4;
}

// CIP elementary data type codes (Logix Read/Write Tag services).
enum class DataType : uint8_t {
    Bool = 0xC1,    // boolean, 1 byte
    Sint = 0xC2,    // signed 8-bit
    Int = 0xC3,     // signed 16-bit
    Dint = 0xC4,    // signed 32-bit
    Lint = 0xC5,    // signed 64-bit
    Usint = 0xC6,   // unsigned 8-bit
    Uint = 0xC7,    // unsigned 16-bit
    Udint = 0xC8,   // unsigned 32-bit
    Ulint = 0xC9,   // unsigned 64-bit
    Real = 0xCA,    // 32-bit IEEE float
    Lreal = 0xCB,   // 64-bit IEEE float
    String = 0xD0,  // Logix STRING (4-byte length + data)
};

// Element size in bytes for a data type (0 if unknown/variable).
inline size_t dataTypeElementSize(DataType t) {
    switch (t) {
        case DataType::Bool:   return 1;
        case DataType::Sint:   return 1;
        case DataType::Int:    return 2;
        case DataType::Dint:   return 4;
        case DataType::Lint:   return 8;
        case DataType::Usint:  return 1;
        case DataType::Uint:   return 2;
        case DataType::Udint:  return 4;
        case DataType::Ulint:  return 8;
        case DataType::Real:   return 4;
        case DataType::Lreal:  return 8;
        case DataType::String: return 0;  // variable: 4-byte length + data
    }
    return 0;
}

// Human-readable name for a data type.
inline const char *dataTypeName(DataType t) {
    switch (t) {
        case DataType::Bool:   return "BOOL";
        case DataType::Sint:   return "SINT";
        case DataType::Int:    return "INT";
        case DataType::Dint:   return "DINT";
        case DataType::Lint:   return "LINT";
        case DataType::Usint:  return "USINT";
        case DataType::Uint:   return "UINT";
        case DataType::Udint:  return "UDINT";
        case DataType::Ulint:  return "ULINT";
        case DataType::Real:   return "REAL";
        case DataType::Lreal:  return "LREAL";
        case DataType::String: return "STRING";
    }
    return "UNKNOWN";
}

// CIP Logix tag read/write service codes.
enum class TagService : uint8_t {
    Read = 0x4C,        // Read Tag
    Write = 0x4D,       // Write Tag
    ReadFragmented = 0x52,
    WriteFragmented = 0x53,
};

}  // namespace clx

