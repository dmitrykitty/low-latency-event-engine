# Memory model

Status: SPSC proof and implementation scheduled for week 1.

The queue will use single-producer/single-consumer ownership, fixed-capacity storage, and release/acquire publication. This document will record the happens-before relationship, full/empty semantics, index wraparound argument, cache-line ownership, and why each non-default memory order is sufficient.

