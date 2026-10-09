# Event data representations

The current single-event path changes between an application view, a native SHM
slot, and encoded LLEP bytes. The UDP hop uses separate sender and receiver packet
buffers with the same LLEP layout. Acquiring or decoding returns a payload view;
publishing and encoding copy payload bytes.

```mermaid
%%{init: {"flowchart": {"wrappingWidth": 380, "rankSpacing": 35}}}%%
flowchart TB
    accTitle: How event data changes representation
    accDescr: EventView refers to application bytes. Publication writes a SHM slot. Acquisition gives Sender a view, encoding builds LLEP bytes, decoding gives Receiver a view, and publication writes an egress SHM slot.

    applicationView["Application EventView<br/>stream_id / sequence / timestamp<br/>payload span into application memory"]
    ingressSlot["Ingress SHM slot<br/>32 B RingSlotHeader<br/>payload storage and stride padding"]
    packetBytes["LLEP DATA bytes in packet buffers<br/>16 B DATA header and 20 B DATA information<br/>6 B frame header, payload, zero padding"]
    decodedView["Receiver DecodedDataView<br/>session_id / packet_sequence / event<br/>event.payload views the receive buffer"]
    egressSlot["Egress SHM slot<br/>32 B RingSlotHeader<br/>payload storage and stride padding"]
    consumerView["Consumer EventView<br/>stream_id / sequence / timestamp<br/>payload span into egress SHM"]

    applicationView -->|"SpscRing::try_publish<br/>write slot metadata, copy payload"| ingressSlot
    ingressSlot -->|"SpscRing::try_acquire gives a view<br/>encode_data writes LLEP, copies payload"| packetBytes
    packetBytes -->|"UdpSender::send_to / UdpReceiver::receive<br/>decode_data validates and returns a view"| decodedView
    decodedView -->|"SpscRing::try_publish<br/>write slot metadata, copy payload"| egressSlot
    egressSlot -->|"SpscRing::try_acquire<br/>read metadata, return payload view"| consumerView

    classDef view fill:#f8fafc,stroke:#64748b,color:#0f172a
    classDef sharedMemory fill:#fef3c7,stroke:#b45309,color:#0f172a
    classDef wire fill:#e0f2fe,stroke:#0369a1,color:#0f172a
    class applicationView,decodedView,consumerView view
    class ingressSlot,egressSlot sharedMemory
    class packetBytes wire
```

[Mermaid source](data-representation-flow.mmd) · [PNG preview](data-representation-flow.png) ·
[Field offsets and example](../docs/data-flow.md)
