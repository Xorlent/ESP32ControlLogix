#!/usr/bin/env python3
"""
Minimal EtherNet/IP synthetic server for testing the ESP32ControlLogix stack.

Implements just enough EtherNet/IP + CIP to validate the client without a real
PLC:
  - RegisterSession   (0x0065) -> returns a fixed session handle
  - UnregisterSession (0x0066) -> returns success
  - SendRRData        (0x006F) -> CIP Identity Object queries and
                                   symbolic Read/Write Tag

Usage:
    python synthetic_eip_server.py [port]
"""

import socket
import struct
import sys

REGISTER_SESSION = 0x0065
UNREGISTER_SESSION = 0x0066
SEND_RR_DATA = 0x006F
HEADER_SIZE = 24

GET_ATTRIBUTE_SINGLE = 0x0E
GET_ATTRIBUTE_SINGLE_REPLY = GET_ATTRIBUTE_SINGLE | 0x80

READ_TAG = 0x4C
READ_TAG_REPLY = READ_TAG | 0x80
WRITE_TAG = 0x4D
WRITE_TAG_REPLY = WRITE_TAG | 0x80

CPF_NULL_ADDRESS = 0x0000
CPF_UNCONNECTED_DATA = 0x00B2
CPF_CONNECTED_ADDRESS = 0x00A1
CPF_CONNECTED_DATA = 0x00B1

SEND_UNIT_DATA = 0x0070
FORWARD_OPEN = 0x54
FORWARD_OPEN_REPLY = FORWARD_OPEN | 0x80
FORWARD_CLOSE = 0x4E
FORWARD_CLOSE_REPLY = FORWARD_CLOSE | 0x80

UNCONNECTED_SEND = 0x52

# Rockwell custom: enumerate instances of the Symbol Object (class 0x6B).
GET_INSTANCE_ATTRIBUTE_LIST = 0x55
GET_INSTANCE_ATTRIBUTE_LIST_REPLY = GET_INSTANCE_ATTRIBUTE_LIST | 0x80

# Symbol Object class code.
SYMBOL_CLASS = 0x6B

# Fixed session handle handed out on RegisterSession (any non-zero value).
SESSION_HANDLE = 0x13572468

# Close a connection if it is idle (no data received) for this many seconds,
# so a session that is not closed cleanly does not hang the server.
SESSION_TIMEOUT = 2.0

# Encapsulation header: command, length, session, status, context, options.
HEADER_FMT = "<HHIIQI"

# CIP Identity Object (class 1, instance 1) attribute values.
IDENTITY_ATTRIBUTES = {
    1: struct.pack("<H", 1),           # vendor ID = Rockwell
    2: struct.pack("<H", 0x000E),      # device type = programmable logic controller
    3: struct.pack("<H", 0x0001),      # product code
    4: struct.pack("<BB", 1, 20),      # revision major=1, minor=20
    5: struct.pack("<H", 0),           # status
    6: struct.pack("<I", 0x12345678),  # serial number
    7: b"Synthetic PLC",               # product name
    8: struct.pack("<B", 3),           # state = operational
}

# In-memory Logix tag table: name -> (data type code, raw bytes).
TAGS = {
    "TestDint": (0xC4, struct.pack("<i", 1001)),
    "TestReal": (0xCA, struct.pack("<f", 3.14)),
    "TestBool": (0xC1, struct.pack("<B", 1)),
    "TestInt": (0xC3, struct.pack("<h", -123)),
    "TestString": (0xD0, struct.pack("<I", 11) + b"Hello World"),
    # A DINT tag (mode = 1 = RUN); enumerated by the ListTags demo.
    "ControllerInfo.Mode": (0xC4, struct.pack("<i", 1)),
}


def parse_symbolic_path(path):
    """Extract the tag name from a symbolic (0x91) path segment, or None."""
    if len(path) < 2 or path[0] != 0x91:
        return None
    name_len = path[1]
    return path[2:2 + name_len].decode("ascii", "replace")


# Connection state: session handle -> {"ot": O->T conn id, "to": T->O conn id}.
connections = {}


def recv_exact(conn, n):
    """Read exactly n bytes, or return None on EOF."""
    data = b""
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def send_packet(conn, command, session, status, context, body=b""):
    header = struct.pack(HEADER_FMT, command, len(body), session, status, context, 0)
    conn.sendall(header + body)


def handle_register_session(conn, context):
    resp_body = struct.pack("<HH", 1, 0)  # protocol version 1, options 0
    send_packet(conn, REGISTER_SESSION, SESSION_HANDLE, 0, context, resp_body)
    print(f"  -> RegisterSession response, handle=0x{SESSION_HANDLE:08X}")


def handle_unregister_session(conn, context):
    send_packet(conn, UNREGISTER_SESSION, 0, 0, context)
    print("  -> UnregisterSession response")


def _parse_instance_from_path(path):
    """Parse a Symbol Object path (class 0x6B + instance segment) and return the
    starting instance number. Handles 8/16/32-bit logical instance segments."""
    if len(path) < 2 or path[0] != 0x20 or path[1] != SYMBOL_CLASS:
        return None
    if len(path) < 4:
        return None
    seg = path[2]
    if seg == 0x24 and len(path) >= 4:
        return path[3]
    if seg == 0x25 and len(path) >= 5:
        return struct.unpack("<H", path[3:5])[0]
    if seg == 0x26 and len(path) >= 7:
        return struct.unpack("<I", path[3:7])[0]
    return None


def _handle_get_instance_attribute_list(start, rest):
    """Synthesize a Symbol Object (0x6B) instance list from the TAGS table."""
    if rest is None or len(rest) < 2:
        attr_ids = [1, 2]
    else:
        count = struct.unpack("<H", rest[0:2])[0]
        attr_ids = list(struct.unpack("<" + "H" * count, rest[2:2 + count * 2]))
    records = b""
    returned = 0
    for idx, (name, (type_code, _value)) in enumerate(TAGS.items(), start=1):
        if idx < (start or 0):
            continue
        record = struct.pack("<I", idx)
        for attr in attr_ids:
            if attr == 1:  # Symbol Name (Logix STRING: 4-byte length + bytes)
                record += struct.pack("<I", len(name)) + name.encode("ascii")
            elif attr == 2:  # Symbol Type (UINT): atomic scalar, type code in low byte
                record += struct.pack("<H", type_code & 0xFF)
            else:
                record += struct.pack("<I", 0)
        records += record
        returned += 1
    print(f"  -> Symbol instance list: start={start} attrs={attr_ids} -> {returned} symbol(s)")
    # status 0 = success (no paging for the tiny synthetic table).
    return bytes([GET_INSTANCE_ATTRIBUTE_LIST_REPLY, 0, 0, 0]) + records


def handle_cip_request(session, service, path, rest):
    """Dispatch a CIP request and return the response bytes (reply service ...)."""
    if service == GET_INSTANCE_ATTRIBUTE_LIST:
        start = _parse_instance_from_path(path)
        if start is not None:
            return _handle_get_instance_attribute_list(start, rest)
    if service == GET_ATTRIBUTE_SINGLE and len(path) >= 6 and path[:4] == bytes([0x20, 0x01, 0x24, 0x01]):
        # Identity object Get_Attribute_Single: the attribute segment
        # (0x30 <attr>) is the final word of the 3-word request path, matching
        # the wire format real ControlLogix hardware expects.
        if path[4] == 0x30:
            attr = path[5]
            value = IDENTITY_ATTRIBUTES.get(attr)
            if value is None:
                print(f"  -> Identity attr {attr}: unknown")
                return bytes([GET_ATTRIBUTE_SINGLE_REPLY, 0, 0x09, 0])  # invalid attribute
            print(f"  -> Identity attr {attr}: {value.hex()}")
            return bytes([GET_ATTRIBUTE_SINGLE_REPLY, 0, 0, 0]) + value
        print("  -> Identity: bad attribute segment")
        return bytes([GET_ATTRIBUTE_SINGLE_REPLY, 0, 0x05, 0])  # path unknown
    if service == READ_TAG:
        name = parse_symbolic_path(path)
        entry = TAGS.get(name)
        if entry is not None:
            type_code, value = entry
            print(f"  -> Read Tag {name}: type=0x{type_code:02X} data={value.hex()}")
            return bytes([READ_TAG_REPLY, 0, 0, 0]) + struct.pack("<H", type_code) + value
        print(f"  -> Read Tag {name}: not found")
        return bytes([READ_TAG_REPLY, 0, 0x04, 0])  # path segment error
    if service == WRITE_TAG:
        name = parse_symbolic_path(path)
        if len(rest) >= 4:
            type_code = struct.unpack("<H", rest[2:4])[0]
            value = rest[4:]
            TAGS[name] = (type_code & 0xFF, value)
            print(f"  -> Write Tag {name}: type=0x{type_code:04X} data={value.hex()}")
            return bytes([WRITE_TAG_REPLY, 0, 0, 0])
        print(f"  -> Write Tag {name}: bad request")
        return bytes([WRITE_TAG_REPLY, 0, 0x04, 0])
    if service == FORWARD_OPEN:
        # rest = [priority/tick (1)][timeout ticks (1)][O->T conn ID (4)][T->O conn ID (4)][...]
        if len(rest) >= 6:
            ot = struct.unpack("<I", rest[2:6])[0]
            to = 0x10203040
            connections[session] = {"ot": ot, "to": to}
            print(f"  -> Forward Open: O->T=0x{ot:08X} T->O=0x{to:08X}")
            return bytes([FORWARD_OPEN_REPLY, 0, 0, 0]) + struct.pack("<IIHH", ot, to, 1, 0x0001)
        print("  -> Forward Open: bad request")
        return bytes([FORWARD_OPEN_REPLY, 0, 0x04, 0])
    if service == FORWARD_CLOSE:
        print("  -> Forward Close")
        return bytes([FORWARD_CLOSE_REPLY, 0, 0, 0])
    print(f"  -> unsupported service 0x{service:02X}")
    return bytes([service | 0x80, 0, 0x08, 0])  # service not supported


def handle_send_rr_data(conn, session, context, body):
    # Body: interface handle (4), timeout (2), item count (2), CPF items.
    if len(body) < 8:
        print("  -> SendRRData: body too short")
        return
    item_count = struct.unpack("<H", body[6:8])[0]
    p = 8
    cip = None
    for _ in range(item_count):
        if p + 4 > len(body):
            return
        item_type, item_len = struct.unpack("<HH", body[p:p + 4])
        p += 4
        if p + item_len > len(body):
            return
        if item_type == CPF_UNCONNECTED_DATA:
            cip = body[p:p + item_len]
        p += item_len

    if cip is None or len(cip) < 4:
        print("  -> SendRRData: no unconnected-data item")
        return

    service = cip[0]
    path_words = cip[1]
    path = cip[2:2 + path_words * 2]
    rest = cip[2 + path_words * 2:]

    print(f"  -> SendRRData: service=0x{service:02X} path={path.hex()} data={rest.hex()}")

    if service == UNCONNECTED_SEND:
        # Unconnected Send (0x52): rest = priority(1) + timeout(1) + length(2)
        # + embedded request + pad + route path(4). Logix returns the embedded
        # response directly (no 0xD2 wrapper).
        if len(rest) < 4:
            print("  -> Unconnected Send: body too short")
            return
        msg_len = struct.unpack("<H", rest[2:4])[0]
        embedded = rest[4:4 + msg_len]
        if len(embedded) < 2:
            print("  -> Unconnected Send: bad embedded request")
            return
        emb_service = embedded[0]
        emb_path_words = embedded[1]
        emb_path = embedded[2:2 + emb_path_words * 2]
        emb_rest = embedded[2 + emb_path_words * 2:]
        slot = rest[-1] if len(rest) >= 4 + msg_len + 4 else 0
        print(f"  -> Unconnected Send: slot={slot} service=0x{emb_service:02X} path={emb_path.hex()}")
        cip_resp = handle_cip_request(session, emb_service, emb_path, emb_rest)
    else:
        cip_resp = handle_cip_request(session, service, path, rest)

    # Build the SendRRData response body.
    resp_body = struct.pack("<IHH", 0, 0, 2)                          # iface handle, timeout, item count
    resp_body += struct.pack("<HH", CPF_NULL_ADDRESS, 0)              # null address item
    resp_body += struct.pack("<HH", CPF_UNCONNECTED_DATA, len(cip_resp))  # unconnected data item
    resp_body += cip_resp
    send_packet(conn, SEND_RR_DATA, SESSION_HANDLE, 0, context, resp_body)


def handle_send_unit_data(conn, session, context, body):
    # Body: interface handle (4), timeout (2), item count (2), CPF items.
    if len(body) < 8:
        print("  -> SendUnitData: body too short")
        return
    item_count = struct.unpack("<H", body[6:8])[0]
    p = 8
    conn_id = None
    conn_data = None
    for _ in range(item_count):
        if p + 4 > len(body):
            return
        item_type, item_len = struct.unpack("<HH", body[p:p + 4])
        p += 4
        if p + item_len > len(body):
            return
        if item_type == CPF_CONNECTED_ADDRESS:
            conn_id = struct.unpack("<I", body[p:p + 4])[0]
        elif item_type == CPF_CONNECTED_DATA:
            conn_data = body[p:p + item_len]
        p += item_len

    if conn_id is None or conn_data is None or len(conn_data) < 4:
        print("  -> SendUnitData: missing items")
        return

    sequence = struct.unpack("<H", conn_data[0:2])[0]
    cip = conn_data[2:]
    service = cip[0]
    path_words = cip[1]
    path = cip[2:2 + path_words * 2]
    rest = cip[2 + path_words * 2:]

    print(f"  -> SendUnitData: conn_id=0x{conn_id:08X} seq={sequence} service=0x{service:02X} path={path.hex()}")

    conn_state = connections.get(session)
    if conn_state is None:
        print("  -> SendUnitData: unknown session")
        return
    # Validate the connection ID (originator sends with O->T).
    if conn_id != conn_state["ot"]:
        print(f"  -> SendUnitData: wrong connection ID (got 0x{conn_id:08X}, expected 0x{conn_state['ot']:08X})")
        return  # drop; do not respond

    if service == READ_TAG:
        name = parse_symbolic_path(path)
        entry = TAGS.get(name)
        if entry is not None:
            type_code, value = entry
            cip_resp = bytes([READ_TAG_REPLY, 0, 0, 0]) + struct.pack("<H", type_code) + value
            print(f"  -> Connected Read Tag {name}: type=0x{type_code:02X} data={value.hex()}")
        else:
            cip_resp = bytes([READ_TAG_REPLY, 0, 0x04, 0])
            print(f"  -> Connected Read Tag {name}: not found")
    elif service == WRITE_TAG:
        name = parse_symbolic_path(path)
        if len(rest) >= 4:
            type_code = struct.unpack("<H", rest[2:4])[0]
            value = rest[4:]
            TAGS[name] = (type_code & 0xFF, value)
            cip_resp = bytes([WRITE_TAG_REPLY, 0, 0, 0])
            print(f"  -> Connected Write Tag {name}: data={value.hex()}")
        else:
            cip_resp = bytes([WRITE_TAG_REPLY, 0, 0x04, 0])
    else:
        cip_resp = bytes([service | 0x80, 0, 0x08, 0])
        print(f"  -> SendUnitData: unsupported service 0x{service:02X}")

    # Build the SendUnitData response (echo the sequence).
    resp_body = struct.pack("<IHH", 0, 0, 2)
    resp_body += struct.pack("<HH", CPF_CONNECTED_ADDRESS, 4) + struct.pack("<I", conn_state["to"])
    resp_body += struct.pack("<HH", CPF_CONNECTED_DATA, 2 + len(cip_resp)) + struct.pack("<H", sequence) + cip_resp
    send_packet(conn, SEND_UNIT_DATA, session, 0, context, resp_body)


def handle_connection(conn, addr):
    print(f"connection from {addr[0]}:{addr[1]}")
    conn.settimeout(SESSION_TIMEOUT)
    try:
        while True:
            try:
                header = recv_exact(conn, HEADER_SIZE)
            except socket.timeout:
                print(f"  -> session timed out (no activity for {SESSION_TIMEOUT:g}s)")
                break
            if header is None:
                break
            command, length, session, status, context, options = struct.unpack(HEADER_FMT, header)
            try:
                body = recv_exact(conn, length) if length else b""
            except socket.timeout:
                print("  -> session timed out (incomplete packet)")
                break
            if body is None:
                break
            print(f"  cmd=0x{command:04X} len={length} session=0x{session:08X} ctx={context}")

            if command == REGISTER_SESSION:
                handle_register_session(conn, context)
            elif command == UNREGISTER_SESSION:
                handle_unregister_session(conn, context)
            elif command == SEND_RR_DATA:
                handle_send_rr_data(conn, session, context, body)
            elif command == SEND_UNIT_DATA:
                handle_send_unit_data(conn, session, context, body)
            else:
                print(f"  -> unhandled command 0x{command:04X} (no response)")
    except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError, OSError):
        pass
    finally:
        conn.close()
        print("connection closed")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 44818
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(1)
    print(f"synthetic EtherNet/IP server listening on 0.0.0.0:{port}")

    while True:
        conn, addr = srv.accept()
        handle_connection(conn, addr)


if __name__ == "__main__":
    main()

