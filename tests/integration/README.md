# Integration tests

The current tests connect the library components using real POSIX SHM and loopback
UDP. Sender and receiver command-line applications are still scaffolds.

| File | What it checks |
|------|----------------|
| [ring_concurrency_test.cpp](ring_concurrency_test.cpp) | Ordered SPSC transfer between threads and between parent/child processes, slot reuse, payload integrity, and final closure |
| [udp_transport_test.cpp](udp_transport_test.cpp) | Binary UDP transfer, DATA encode/decode round trip, oversized datagrams, and invalid endpoints |
| [sender_test.cpp](sender_test.cpp) | Ingress consumption, UDP transmission, slot release, stop requests, and error accounting |
| [receiver_test.cpp](receiver_test.cpp) | UDP validation, egress publication, drops, cleanup, idle stop, and the complete producer -> sender -> receiver -> consumer path |

From the repository root, after building the development preset:

```bash
ctest --preset dev -R 'lle.(shm.concurrent|transport|sender|receiver).' --output-on-failure
```

The full local path is
`ReceiverTest.ProducerSenderReceiverConsumerPreserveEvent` in `receiver_test.cpp`.
It runs inside one test process with a receiver worker and separate SHM
attachments. The dedicated SHM concurrency test covers transfer between processes.
Two-host full-path, fan-out, and recovery fault-injection tests remain planned.

See [testing instructions](../../docs/testing.md) for builds, individual filters,
sanitizer commands, and the TSan launcher. See the
[architecture guide](../../docs/architecture.md) for component connections and
event-buffer lifetimes.

