# Shared-memory layout and memory model

Status: shared-memory ABI version 1.

This document specifies LLE's local POSIX shared-memory SPSC queue. It is independent from the Low-Latency Event Protocol defined in `docs/protocol.md`; LLEP packet headers, message types, packet sequences, NACKs, and network session IDs are not stored in shared-memory event slots.

## Requirements

The queue is designed for:

- exactly one producer and one consumer per segment;
- fixed capacity and fixed slot stride;
- no allocation or system calls during publication and consumption;
- no silent overwrite of unread events;
- non-owning views into slot payload storage;
- cache-line separation of producer- and consumer-owned cursors;
- release/acquire publication;
- explicit creation, attachment, close, and unlink ownership.

The shared-memory ABI uses native fixed-width integer representation. Version 1 requires a little-endian Linux host and lock-free 32-bit and 64-bit integral atomics.

## Segment layout

```text
offset 0
+---------------------------------------------------------------+
| SharedMemoryPreamble                                  64 B    |
+---------------------------------------------------------------+
offset 64
+---------------------------------------------------------------+
| ProducerCursor                                        64 B    |
+---------------------------------------------------------------+
offset 128
+---------------------------------------------------------------+
| ConsumerCursor                                        64 B    |
+---------------------------------------------------------------+
offset header_bytes (version 1: 192)
+---------------------------------------------------------------+
| Slot 0                                      slot_stride B     |
+---------------------------------------------------------------+
| Slot 1                                      slot_stride B     |
+---------------------------------------------------------------+
| ...                                                            |
+---------------------------------------------------------------+
| Slot slot_count - 1                            slot_stride B   |
+---------------------------------------------------------------+
```

All major regions and every slot begin on a 64-byte boundary.

## Shared-memory preamble

| Offset | Size | Type            | Field                   |
|-------:|-----:|-----------------|-------------------------|
|      0 |    4 | bytes           | magic ASCII `LLE1`      |
|      4 |    2 | native `uint16` | `layout_version` = 1    |
|      6 |    2 | native `uint16` | `header_bytes` = 192    |
|      8 |    8 | native `uint64` | `segment_bytes`         |
|     16 |    4 | native `uint32` | `slot_count`            |
|     20 |    4 | native `uint32` | `slot_stride`           |
|     24 |    4 | native `uint32` | `slot_payload_capacity` |
|     28 |    4 | native `uint32` | reserved, zero          |
|     32 |    8 | native `uint64` | `instance_id`           |
|     40 |    4 | atomic `uint32` | `state`                 |
|     44 |   20 | bytes           | reserved, zero          |

The shared-memory magic is required because a named object can outlive a process or be opened using the wrong configured name.

`instance_id` is a random nonzero generation identifier chosen when the owner initializes a new segment. It is not an LLEP network session ID.

The reserved preamble field must be zero in layout version 1.

The mapped object size is exactly:

```text
segment_bytes = header_bytes + slot_count * slot_stride
```

Every addition and multiplication used to validate this expression must be checked for overflow and compared with the actual mapped-object size.

## 4. Segment state

| Value | Name            | Meaning                                           |
|------:|-----------------|---------------------------------------------------|
|     0 | `UNINITIALIZED` | Segment has not been initialized                  |
|     1 | `INITIALIZING`  | Owner is constructing metadata and atomic objects |
|     2 | `READY`         | Producer and consumer may operate                 |
|     3 | `CLOSED`        | Owner has stopped publication                     |

The creator is the only initializer. It initializes metadata, cursors, and slots before release-storing `READY`. Attachers acquire-load `state` and validate metadata after observing `READY`.

`INITIALIZING` is reported as not ready. It is never treated as an empty valid queue. The owner release-stores `CLOSED`; the consumer acquire-loads it when deciding whether an empty queue can receive more events.

## Cursor layout

```text
offset 64
+---------------------------------------------------------------+
| write_position (atomic u64)                            8 B    |
| reserved                                              56 B    |
+---------------------------------------------------------------+

offset 128
+---------------------------------------------------------------+
| read_position (atomic u64)                             8 B    |
| reserved                                              56 B    |
+---------------------------------------------------------------+
```

Only the producer writes `write_position`. Only the consumer writes `read_position`. Keeping them on separate cache lines prevents the routine writes from invalidating one shared cache line.

Both positions begin at zero and count logical slots rather than physical array indices.

`slot_count` must be a power of two and at least 2:

```text
slot_index = logical_position & (slot_count - 1)
```

Queue state is:

```text
empty when write_position == read_position
full  when write_position - read_position == slot_count
```

The distance between cursors must remain between zero and `slot_count`, inclusive. A segment is replaced before a 64-bit cursor wraps.

## Slot layout

Each slot has a 32-byte event metadata header followed by fixed-capacity payload storage.

```text
slot-relative offset
 0                                                               7
+---------------------------------------------------------------+
| event_sequence (native u64)                                  |
+---------------------------------------------------------------+
 8                                                              15
+---------------------------------------------------------------+
| source_timestamp_ns (native u64)                             |
+---------------------------------------------------------------+
 16                             19 20                            23
+--------------------------------+-------------------------------+
| stream_id (native u32)         | payload_length (native u32)  |
+--------------------------------+-------------------------------+
 24                             27 28                            31
+--------------------------------+-------------------------------+
| reserved = 0                   | reserved = 0                  |
+--------------------------------+-------------------------------+
 32
+---------------------------------------------------------------+
| payload storage: slot_payload_capacity bytes                  |
+---------------------------------------------------------------+
| unused stride padding                                         |
+---------------------------------------------------------------+
```

| Offset |     Size | Field                 |
|-------:|---------:|-----------------------|
|      0 |        8 | `event_sequence`      |
|      8 |        8 | `source_timestamp_ns` |
|     16 |        4 | `stream_id`           |
|     20 |        4 | `payload_length`      |
|     24 |        4 | reserved, zero        |
|     28 |        4 | reserved, zero        |
|     32 | variable | payload storage       |

Slot stride is:

```text
slot_stride = align_up(32 + slot_payload_capacity, 64)
```

`payload_length` cannot exceed `slot_payload_capacity`. Unused payload capacity and stride padding have no semantic value and do not need to be cleared for every publication.

Shared memory contains no pointers, `std::span`, `std::size_t`, compiler-dependent enums, or variable-size C++ objects.

## Producer publication

The producer performs one bounded attempt:

1. relaxed-load its locally owned `write_position`;
2. acquire-load `read_position`;
3. return `Full` if the queue is full;
4. validate the event before changing the selected slot;
5. write event metadata;
6. copy payload directly into slot storage;
7. release-store `write_position + 1`.

The release-store publishes all preceding slot writes.

Publication results are:

| Result            | Meaning                                        |
|-------------------|------------------------------------------------|
| `Ok`              | Event was written and published                |
| `Full`            | No slot is available; queue state is unchanged |
| `PayloadTooLarge` | Payload does not fit; queue state is unchanged |
| `Closed`          | Segment is not ready for publication           |

The queue never overwrites an unread slot. Spin, retry, or drop behavior belongs to the adapter above the one-attempt publication API.

## Consumer acquisition and release

The consumer:

1. relaxed-loads its locally owned `read_position`;
2. acquire-loads `write_position`;
3. reports empty if both positions are equal;
4. reads metadata and exposes a non-owning view into the slot;
5. finishes all use of the view;
6. release-stores `read_position + 1`.

The acquire-load of `write_position` makes the producer's slot writes visible. The release-store to `read_position` informs the producer that the slot may be reused.

A view remains valid until its slot is released. A consumer API must not advance `read_position` before its caller has finished reading or encoding the payload. This can be expressed with an explicit lease/release API or equivalent ownership.

## Lifecycle and ownership

- The creator owns sizing, initialization, transition to `READY`, transition to `CLOSED`, and `shm_unlink`.
- Attachers validate object size, magic, version, header size, reserved fields, capacity, stride, state, and lock-free atomic requirements.
- Attachers close and unmap their own handles but do not unlink an object they do not own.
- A stale or incompatible object is rejected. An attacher never silently reinitializes it.
- Creation and attachment may use system calls and allocation; event publication and consumption may not.

Each segment is SPSC. Fan-out uses a separate egress SPSC segment for each local consumer rather than sharing one read cursor among multiple consumers.

## Shared-memory constants

```text
SHM_MAGIC_BYTES              "LLE1"
SHM_LAYOUT_VERSION                1
SHM_HEADER_BYTES                192
SHM_CACHE_LINE_BYTES             64
SHM_SLOT_METADATA_BYTES          32
```

## Required tests

- create, attach, close, unmap, and unlink ownership;
- invalid magic, version, state, object size, capacity, and stride;
- empty and full states;
- exact-capacity publication;
- physical-index and logical-cursor wraparound;
- payload sizes from zero through configured capacity;
- no overwrite while a consumer view is held;
- close while empty and close with queued events;
- long-running monotonic sequence transfer without corruption or duplication;
- release/acquire stress testing;
- an in-process equivalent under ThreadSanitizer.
