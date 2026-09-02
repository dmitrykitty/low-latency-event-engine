# Low-Latency Event Protocol (LLEP)

Status: protocol version 1.

LLEP is a compact binary protocol for transporting ordered streams of opaque events. This document defines only the LLEP representation. Shared-memory representation and synchronization are specified separately in `docs/memory-model.md`.

## 1. Protocol properties

LLEP version 1 has these properties:

- maximum LLEP packet size: 1,416 bytes;
- little-endian encoding for every multi-byte integer;
- one-byte magic followed by a four-bit version and four-bit message type;
- no flag fields;
- allocation-free encoding and decoding;
- sequential event framing without an event-count field;
- one stream per DATA packet;
- event sequence and timestamp reconstruction from DATA-level base values;
- bounded multi-range NACK messages.

The 1,416-byte limit applies only to the LLEP packet. Outer transport and network headers are not included in `packet_length`.

## 2. Terminology and byte order

The words **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, and **MAY** describe normative requirements. All offsets are measured in bytes from the beginning of the enclosing packet or frame. 
Types named `u16`, `u32`, and `u64` are unsigned integers encoded in little-endian byte order.
Encoders and decoders MUST use explicit integer helpers. They MUST NOT serialize native C++ structures, C++ bit fields, compiler padding, or packed structures directly.

## 3. Message hierarchy and decoded views

LLEP has two decoding layers:

```text
LLEP packet bytes
       |
       v
+---------------------------+
| DecodedPacketView         |
|                           |
| magic                     |
| version                   |
| message type              |
| packet length             |
| session ID                |
+-------------+-------------+
              |
       +------+------+
       |             |
       v             v
+-------------+  +----------------+
| DATA view   |  | NACK view      |
|             |  |                |
| packet seq  |  | range iterator |
| stream ID   |  +----------------+
| first event |
| base time   |
+------+------+
       |
       v
+---------------------------+
| DecodedEventView          |
|                           |
| stream ID      (inherited)|
| event sequence (computed) |
| timestamp      (computed) |
| payload         (view)    |
+---------------------------+
```

A decoded event view is the logical representation of one event. Its values do not all need to exist physically in each event frame:

- `stream_id` is inherited from the containing DATA packet;
- `sequence` is `first_event_sequence + event_index`;
- `source_timestamp_ns` is `base_timestamp_ns + timestamp_delta_ns`;
- `payload` points directly into the packet buffer.

A packet does not need a second packet ID. The tuple:

```text
(session_id, packet_sequence)
```

uniquely identifies a DATA packet. Packet identity belongs to the decoded DATA-packet view, not the public application event. Internal metrics MAY retain the containing packet identity alongside an event view without encoding it again in every event frame.

## 4. Message types

The low four bits of the version/type byte provide 16 possible message types.

| Value | Name | Meaning |
|---:|---|---|
| 0 | `INVALID` | Reserved and rejected |
| 1 | `DATA` | Original packet containing one or more events |
| 2 | `RETRANSMITTED_DATA` | Retransmission of an earlier DATA packet |
| 3 | `NACK` | Request for one or more missing packet ranges |
| 4-14 | Reserved | Future standard message types |
| 15 | `PRIVATE` | Reserved; rejected by the standard version-1 decoder |

`DATA` and `RETRANSMITTED_DATA` have the same layout. A retransmission retains the original session ID, packet sequence, stream information, event information, and payload bytes.

## 5. Common LLEP header

Every LLEP message begins with an 8-byte common header.

```text
byte offset
 0               1               2               3
+---------------+---------------+-------------------------------+
| magic         | version/type  | packet_length (u16)           |
+---------------+---------------+-------------------------------+
 4                                                               7
+---------------------------------------------------------------+
| session_id (u32)                                              |
+---------------------------------------------------------------+

Common LLEP header: 8 bytes
```

| Offset | Size | Field | Encoding |
|---:|---:|---|---|
| 0 | 1 | `magic` | Constant `0x4c`, ASCII `L` |
| 1 | 1 | `version_and_type` | Version in high nibble, type in low nibble |
| 2 | 2 | `packet_length` | Total LLEP packet length, `u16` |
| 4 | 4 | `session_id` | Sender-session identifier, `u32` |

### 5.1 Version and type byte

```text
bit       7 6 5 4 | 3 2 1 0
         +--------+--------+
         |version |  type  |
         +--------+--------+
```

Equivalent encoding is:

```cpp
encoded = static_cast<std::uint8_t>((version << 4U) | message_type);
version = encoded >> 4U;
message_type = encoded & 0x0fU;
```

### 5.2 Packet length

`packet_length` includes every LLEP byte from `magic` through the final payload or alignment byte. It MUST equal the actual LLEP packet size and MUST satisfy:

```text
8 <= packet_length <= 1416
```

Type-specific minimum lengths are stricter.

### 5.3 Session ID

`session_id` identifies one sender lifetime:

- zero is invalid;
- a sender chooses a new nonzero value on startup;
- the value remains constant for that sender session;
- packet and event sequences are interpreted only inside that session;
- a NACK carries the session ID of the DATA packets it requests;
- stale NACKs and DATA packets from another session are not combined with the active session.

## 6. DATA packet

`DATA` and `RETRANSMITTED_DATA` use a 16-byte LLEP packet header followed by 20 bytes of DATA-level stream information and then event frames.

### 6.1 Sixteen-byte LLEP DATA header

```text
byte offset
 0               1               2               3
+---------------+---------------+-------------------------------+
| magic         | version/type  | packet_length (u16)           |
+---------------+---------------+-------------------------------+
 4                                                               7
+---------------------------------------------------------------+
| session_id (u32)                                              |
+---------------------------------------------------------------+
 8                                                              15
+---------------------------------------------------------------+
| packet_sequence (u64)                                        |
+---------------------------------------------------------------+

LLEP DATA header: 16 bytes
```

| Offset | Size | Field | Encoding |
|---:|---:|---|---|
| 0 | 8 | common LLEP header | Section 5 |
| 8 | 8 | `packet_sequence` | `u64` |

`packet_sequence` starts at 1 and increases by one for every new DATA packet in the session. It is global to the sender session, not per stream. Zero is invalid.

Because packet sequence is global, a NACK range is unambiguous and does not need a stream ID.

### 6.2 DATA-level stream information

Every DATA packet contains events from exactly one stream.

```text
 16                                                             23
+---------------------------------------------------------------+
| first_event_sequence (u64)                                   |
+---------------------------------------------------------------+
 24                                                             31
+---------------------------------------------------------------+
| base_timestamp_ns (u64)                                      |
+---------------------------------------------------------------+
 32                                             35
+-----------------------------------------------+
| stream_id (u32)                               |
+-----------------------------------------------+
 36
+-----------------------------------------------+
| event frame 0 ...                            |
+-----------------------------------------------+
```

| Offset | Size | Field | Encoding |
|---:|---:|---|---|
| 16 | 8 | `first_event_sequence` | First event sequence in this packet, `u64` |
| 24 | 8 | `base_timestamp_ns` | Timestamp of event frame 0, `u64` |
| 32 | 4 | `stream_id` | Stream shared by all packet events, `u32` |
| 36 | variable | `event_frames[]` | Sequential event records |

The physical field order keeps both 64-bit values naturally aligned. The logical meaning remains:

```text
stream_id
first_event_sequence
base_timestamp_ns
```

`first_event_sequence` is scoped to `(session_id, stream_id)`. Events in a packet are consecutive. If event frame 0 has sequence `S`, frame `i` has sequence:

```text
event_sequence(i) = S + i
```

The sender MUST start a new DATA packet when the next event belongs to another stream. This avoids repeating `stream_id` in every event frame.

## 7. Event frame

An event frame stores only information that cannot be reconstructed from the containing DATA packet.

```text
frame-relative offset
 0                                               3
+-----------------------------------------------+
| timestamp_delta_ns (u32)                      |
+-------------------------------+---------------+
 4                             5 6
+-------------------------------+-------------------------------+
| payload_length (u16)          | payload ...                   |
+-------------------------------+-------------------------------+
| optional zero padding, 0-3 bytes                              |
+---------------------------------------------------------------+
```

| Offset | Size | Field | Encoding |
|---:|---:|---|---|
| 0 | 4 | `timestamp_delta_ns` | Nanoseconds after the packet base timestamp, `u32` |
| 4 | 2 | `payload_length` | Number of payload bytes, `u16` |
| 6 | variable | `payload` | Opaque application bytes |
| variable | 0-3 | padding | Zero bytes to align the next frame to 4 bytes |

The frame size is:

```text
frame_size = align_up(6 + payload_length, 4)
```

Event frames begin on 4-byte boundaries. This naturally aligns `timestamp_delta_ns` while adding at most three bytes of padding per event.

For event index `i`:

```text
stream_id(i)           = DATA.stream_id
event_sequence(i)      = DATA.first_event_sequence + i
source_timestamp_ns(i) = DATA.base_timestamp_ns + timestamp_delta_ns(i)
```

The first event's timestamp delta MUST be zero. An encoder MUST start a new packet if a later timestamp is earlier than the base timestamp or its delta exceeds `UINT32_MAX` nanoseconds. Timestamp addition and event-sequence addition MUST be checked for overflow.

An empty payload is valid. Padding bytes MUST be written as zero and are not part of the payload.

There is no event-count field. The decoder starts at offset 36 and parses complete frames until it reaches `packet_length`. The final padded frame MUST end exactly at that boundary.

## 8. Complete DATA layout

```text
+================================================================+
| Common LLEP header                                      8 B    |
| magic | version/type | packet length | session ID              |
+----------------------------------------------------------------+
| packet_sequence                                        8 B    |
+================================================================+
| first_event_sequence                                   8 B    |
+----------------------------------------------------------------+
| base_timestamp_ns                                      8 B    |
+----------------------------------------------------------------+
| stream_id                                              4 B    |
+================================================================+
| Event frame 0                                                  |
| timestamp delta | payload length | payload | padding           |
+----------------------------------------------------------------+
| Event frame 1                                                  |
| timestamp delta | payload length | payload | padding           |
+----------------------------------------------------------------+
| ...                                                            |
+================================================================+

Fixed bytes before event frames: 36 B
```

## 9. NACK packet

A NACK contains the common 8-byte LLEP header followed directly by one or more 10-byte missing-range records. It does not contain `packet_sequence`, DATA-level fields, or a separate range count.

```text
byte offset
 0               1               2               3
+---------------+---------------+-------------------------------+
| magic         | version/type  | packet_length (u16)           |
+---------------+---------------+-------------------------------+
 4                                                               7
+---------------------------------------------------------------+
| session_id (u32)                                              |
+===============================================================+
 8                                                              15
+---------------------------------------------------------------+
| range 0: first_sequence (u64)                                 |
+-------------------------------+-------------------------------+
 16                            17 18
+-------------------------------+-------------------------------+
| range 0: packet_count (u16)   | range 1 begins ...            |
+-------------------------------+-------------------------------+
| range 1: first_sequence (u64) ...                             |
+-------------------------------+-------------------------------+
| range 1: packet_count (u16) ...                               |
+---------------------------------------------------------------+
| ...                                                            |
+===============================================================+
```

### 9.1 Missing range

| Range-relative offset | Size | Field | Encoding |
|---:|---:|---|---|
| 0 | 8 | `first_sequence` | First missing packet sequence, `u64` |
| 8 | 2 | `packet_count` | Number of consecutive missing packets, `u16` |

One range requests the inclusive interval:

```text
[first_sequence, first_sequence + packet_count - 1]
```

For example:

```text
first_sequence = 100
packet_count   = 3

requested packets = 100, 101, 102
```

### 9.2 Number of ranges

The number of ranges is calculated from the packet length:

```text
range_body_length = packet_length - 8
range_count       = range_body_length / 10
```

A valid NACK satisfies:

```text
packet_length >= 18
(packet_length - 8) % 10 == 0
1 <= range_count <= 64
sum(packet_count) <= 4096
```

Ranges MUST:

- use nonzero `first_sequence` and `packet_count`;
- be sorted by `first_sequence`;
- not overlap;
- not be adjacent—the encoder merges adjacent ranges;
- not overflow when calculating the inclusive final sequence;
- refer only to the session carried by the common header.

The maximum version-1 NACK has 64 ranges and is:

```text
8 + (64 * 10) = 648 bytes
```

## 10. Retransmission

`RETRANSMITTED_DATA` uses the exact DATA layout. Compared with the original packet:

- message type changes from `DATA` to `RETRANSMITTED_DATA`;
- session ID and packet sequence remain unchanged;
- DATA-level metadata and all event frames remain unchanged.

Receivers identify duplicates by `(session_id, packet_sequence)`. Message type describes how the packet was sent; it is not the packet identity.

## 11. Packet-size limits

```text
MAX_LLEP_PACKET_BYTES = 1416
```

For a DATA packet:

```text
packet_length =
    36
    + sum(align_up(6 + event.payload_length, 4))
```

Therefore:

- minimum DATA packet with one empty event: 44 bytes;
- maximum single-event payload: 1,374 bytes;
- an event is never split across LLEP packets;
- a DATA packet always contains at least one complete event;
- the sender stops adding events before the next complete frame would exceed 1,416 bytes.

For a NACK packet:

```text
packet_length = 8 + (10 * range_count)
```

## 12. Validation

A decoder validates the complete packet before exposing event payloads or processing NACK ranges.

### 12.1 Common validation

1. At least 8 bytes are present.
2. `magic == 0x4c`.
3. Protocol version is supported.
4. Message type is known.
5. `packet_length` equals the available LLEP bytes.
6. `packet_length <= 1416`.
7. `session_id != 0`.
8. All type-specific rules pass.

### 12.2 DATA validation

1. At least 44 bytes are present.
2. `packet_sequence != 0`.
3. `first_event_sequence != 0`.
4. At least one complete event frame is present.
5. Every payload and padded frame fits inside `packet_length`.
6. The first timestamp delta is zero.
7. Reconstructed timestamps and event sequences do not overflow.
8. The final frame ends exactly at `packet_length`.

### 12.3 NACK validation

1. At least one complete 10-byte range is present.
2. The body length is divisible by 10.
3. Range and requested-packet limits are respected.
4. Starts and counts are nonzero.
5. Ranges are sorted, non-overlapping, and non-adjacent.
6. Final-sequence calculation does not overflow.
7. The session matches DATA packets known to the sender.

A malformed packet is rejected as a whole. No event from a partially valid DATA packet is delivered.

## 13. Version behavior

Version zero is invalid. Version 1 is defined by this document. Other versions are rejected; version negotiation is not defined.

Within version 1:

- the common header, DATA fields, event frames, and NACK ranges have fixed layouts;
- there are no flag fields;
- reserved message types are not interpreted;
- changing a field's offset, width, meaning, byte order, or required validation requires a protocol-version increment;
- adding a new message type does not change existing message layouts, but an older decoder rejects that new type.

## 14. Constants

```text
LLEP_MAGIC                       0x4c
LLEP_VERSION                     1

MESSAGE_INVALID                  0
MESSAGE_DATA                     1
MESSAGE_RETRANSMITTED_DATA       2
MESSAGE_NACK                     3

COMMON_HEADER_BYTES              8
DATA_PACKET_HEADER_BYTES        16
DATA_LEVEL_BYTES                20
DATA_FIXED_BYTES                36
EVENT_FRAME_HEADER_BYTES         6
NACK_RANGE_BYTES                10

MAX_LLEP_PACKET_BYTES         1416
MAX_NACK_RANGES                 64
MAX_NACK_REQUESTED_PACKETS    4096
```
