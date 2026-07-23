# SpaceLab UFSC Summer Internship — Work Log

This repo holds the work I've done so far during my remote internship with **SpaceLab UFSC**
(Florianópolis, Brazil), contributing to the **GOLDS-UFSC / FloripaSat-2** CubeSat mission on
the telecommunications (TTC 2.0) subsystem.

I'm organizing this chronologically, task by task, since that's genuinely the order I learned
things in — each task builds directly on the one before it. 

## Task Overview

### [01 — NGHam Python Task](./01-ngham-python-task)
The very first task. Before writing any embedded C code, I used the existing `pyngham` Python
library to actually understand the NGHam protocol — how a payload gets encoded into a full
frame, how Reed-Solomon error correction recovers corrupted bytes, and how extension packets
(id / status / time-of-hour) get bundled together. This was pure learning-by-experimenting: no
firmware yet, just Python scripts and printed output to build real intuition about what NGHam
actually does under the hood.

### [02 — Learning FreeRTOS (Notes)](./02-freertos-learning-notes)
Once I knew *what* NGHam does, I needed to learn *how* to actually run this logic on the
MSP430 — which meant learning FreeRTOS. This folder is my own study notes (not a polished
tutorial) covering tasks, task states, priorities, and queues, each one tied directly back to
how I ended up using it in Task 3's pipeline. Written the way I actually studied it: read a
concept, connect it to the real pipeline, write down what clicked.

### [03 — Payload Memory Reading + Packet Transmission Task](./03-payload-memory-reading-task)
This is where everything comes together — a 3-task FreeRTOS pipeline in C that simulates
reading payload memory (mock occultation event data for now), fragmenting it into fixed-size
chunks, passing those chunks through a queue manager, and encoding each one into a real NGHam
frame (preamble, sync word, size tag, header, CRC, padding, Reed-Solomon parity) using an
NGHam implementation I wrote from scratch in C. Currently runs on the FreeRTOS POSIX simulator
(via WSL) so the logic can be fully tested before it needs to run on actual MSP430 hardware in
Code Composer Studio.

### [04 — Housekeeping / Telemetry Task](./04-Housekeepin-Telemetry-Task)
Task 3's pipeline moves payload data, but says nothing about its own health — how full the
queues are, how fast data is actually flowing, whether anything's being dropped. This task
adds a separate module that watches the pipeline from the outside and keeps a live status
struct up to date, following the same pattern as `radio_data_t` in the TTC 2.0 firmware.
Per direct team feedback, this module only *maintains* the telemetry continuously — it's
the TT&C module's job to transmit it periodically, not this one's. Uses a FreeRTOS length-1
"mailbox" queue instead of a mutex to publish the latest snapshot safely between tasks.

---

## How the three tasks connect

```
Task 1 (NGHam in Python)          ─┐
-- "what does NGHam actually do?"  │
                                   ├─► gave me the protocol understanding
Task 2 (FreeRTOS notes)           ─┤    needed to build...
-- "how do tasks/queues work?"     │
                                   │
Task 3 (C pipeline)               ─┘
-- actual FreeRTOS tasks + queues
   + a real NGHam encoder in C
            │
            ▼
Task 4 (Housekeeping module)
-- watches the pipeline from outside,
   publishes live telemetry for TT&C
   to include in its own downlink
```

## A note on where this stands

This is very much a work in progress, not a finished flight-ready implementation. Task 3's
Reed-Solomon encoder, for example, is structurally correct but currently uses a common
textbook GF(256) field polynomial rather than the exact one from the official NGHam spec — so
frames it builds aren't yet guaranteed to be byte-identical to what a real ground station
decoder would expect. That's called out directly in Task 3's README along with the other
next steps I'm planning to tackle.
