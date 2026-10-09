# Current data path

Arrows show the direction of event data. Each SHM ring has one producer and one
consumer. Sender and receiver sides may run on the same host or on different hosts;
the current integration tests exercise loopback delivery through the library API.

```mermaid
flowchart TB
    accTitle: Current event distribution architecture
    accDescr: An external producer writes ingress shared memory. Sender encodes one event and sends it over UDP. Receiver decodes it and writes egress shared memory for an external consumer.

    subgraph senderSide["Sender side - local processes"]
        producer["External<br/>producer"]
        ingress[("Ingress SHM<br/>SpscRing")]
        sender["Sender<br/>encode_data / UdpSender"]
        producer -->|try_publish| ingress
        ingress -->|try_acquire| sender
    end

    subgraph receiverSide["Receiver side - local processes"]
        receiver["Receiver<br/>UdpReceiver / decode_data"]
        egress[("Egress SHM<br/>SpscRing")]
        consumer["External<br/>consumer"]
        receiver -->|try_publish| egress
        egress -->|try_acquire| consumer
    end

    sender -->|"LLEP DATA / UDP"| receiver

    classDef application fill:#f8fafc,stroke:#64748b,color:#0f172a
    classDef engine fill:#e0f2fe,stroke:#0369a1,color:#0f172a
    classDef sharedMemory fill:#fef3c7,stroke:#b45309,color:#0f172a
    class producer,consumer application
    class sender,receiver engine
    class ingress,egress sharedMemory
```

[Editable source](current-data-path.mmd) · [PNG preview](current-data-path.png) ·
[Architecture notes](../docs/architecture.md#current-data-path)
