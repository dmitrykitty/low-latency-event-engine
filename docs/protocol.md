# Wire protocol

Status: design work scheduled for week 1.

The protocol must define magic, version, flags, byte order, header sizes, packet sequence, event count, stream ID, event sequence, source timestamp, payload length, maximum datagram size, and compatibility behavior. Encoding and decoding use explicit functions rather than sending native C++ structures.

Parser order: validate the minimum header, magic/version, declared sizes, event count, and every frame boundary before exposing payload bytes.

