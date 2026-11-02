"""Looks up .local names by asking on the network directly, so they work on systems without avahi or nss-mdns."""

import socket
import struct

MDNS_GROUP = ("224.0.0.251", 5353)
TYPE_A = 1
CLASS_IN = 1
CACHE_FLUSH_BIT = 0x8000  # mDNS sets this on the class of some answers


def encode_name(name):
    labels = name.rstrip(".").split(".")
    return b"".join(bytes([len(label)]) + label.encode() for label in labels) + b"\0"


def read_name(data, pos):
    """The name at `pos` and where the data after it starts, following compression pointers."""
    labels = []
    end = None
    for _ in range(128):  # a bad packet could have pointers that loop
        length = data[pos]
        if length >= 0xC0:
            end = end or pos + 2
            pos = (length & 0x3F) << 8 | data[pos + 1]
        elif length:
            labels.append(data[pos + 1:pos + 1 + length].decode(errors="replace"))
            pos += 1 + length
        else:
            return ".".join(labels), end or pos + 1
    raise ValueError("name is too long")


def find_address(data, host):
    """The IPv4 address for `host` in a DNS response, or None."""
    try:
        _, flags, questions, answers, authority, additional = struct.unpack_from(">6H", data)
        if not flags & 0x8000:  # a query from someone else, not a response
            return None
        pos = 12
        for _ in range(questions):
            pos = read_name(data, pos)[1] + 4
        for _ in range(answers + authority + additional):
            name, pos = read_name(data, pos)
            kind, record_class, _, length = struct.unpack_from(">HHIH", data, pos)
            pos += 10
            if (kind == TYPE_A and record_class & ~CACHE_FLUSH_BIT == CLASS_IN and length == 4
                    and name.lower() == host.rstrip(".").lower()):
                return socket.inet_ntoa(data[pos:pos + 4])
            pos += length
    except (IndexError, ValueError, struct.error):  # a malformed packet
        pass
    return None


def resolve(host, attempts=3, wait=1):
    """The IPv4 address of a .local name, or None if nothing answers."""
    query = struct.pack(">6H", 0, 0, 1, 0, 0, 0) + encode_name(host) + struct.pack(">HH", TYPE_A, CLASS_IN)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
        sock.settimeout(wait)
        for _ in range(attempts):
            try:
                sock.sendto(query, MDNS_GROUP)
                address = find_address(sock.recv(1500), host)
            except OSError:  # no reply yet, or no network
                continue
            if address:
                return address
    return None
