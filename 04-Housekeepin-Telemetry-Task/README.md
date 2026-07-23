# Task 4 — Housekeeping / Telemetry Module

## What this task was actually asking

After Task 3's pipeline was working, the team call feedback was specific: this module is
only responsible for **transmitting payload data**, so housekeeping (system health) data
should be updated continuously, but *transmitted periodically by the TT&C module* — not by
this module. The suggested approach was a shared struct, similar to `radio_data_t` in the
TTC 2.0 firmware (`firmware/devices/radio/radio_data.h`), where each field holds one
parameter — buffer occupancy, generation rate, transmission rate, lost packets — and the
TT&C module reads it whenever it assembles a housekeeping packet.

So the actual deliverable here isn't "send housekeeping packets" — it's "keep an accurate,
thread-safe status struct that something else can read at any time."

## Why a separate module, not code bolted onto Task 1/2/3

This follows the same pattern as `payload_chunk.h` and `ngham.h` from Task 3: a `.h` that
declares the interface, a `.c` that owns the implementation and state. Nothing outside
`housekeeping.c` needs to know *how* the data is stored — every other file only ever calls
`Housekeeping_Init()`, `vTask4_HousekeepingGenerator()`, or `Housekeeping_GetSnapshot()`.
That matters specifically because the eventual reader (TT&C) is a module I don't own or
control the internals of — the interface is the actual contract between the two.

## Shared files from Task 3

This task builds directly on top of Task 3's pipeline and reuses two of its files
unmodified — rather than duplicate them here, they're linked below. Task 4 didn't need to
change either one; the housekeeping module only reads from the existing queues and global
counters that Task 3 already exposes.

- [`ngham.h`](../03-payload-memory-reading-task/ngham.h) — the NGHam packet encoder from
  Task 3. Untouched here; `main.c` still includes it because Task 3's encoder task
  (`vTask3_NGHamEncoder`) is unchanged.
- [`payload_chunk.h`](../03-payload-memory-reading-task/payload_chunk.h) — the
  `PayloadChunk_t` struct definition. Untouched here for the same reason — Task 1 and Task 2
  still pass this struct through the queues exactly as before.

Only `housekeeping.h`, `housekeeping.c`, and the updated `main.c` are new to this folder.

## Struct design

```c
typedef struct
{
    uint8_t  raw_fifo_occupancy;
    uint8_t  ordered_fifo_occupancy;
    uint32_t chunk_generation_rate;
    uint32_t chunk_tx_rate;
    uint32_t chunk_overflow_count;
} HousekeepingData_t;
```

- Occupancy fields are `uint8_t` because both queues are created with a fixed depth of 10
  (`xQueueCreate(10, ...)`), so occupancy can never exceed that — a full byte is already
  overkill. `radio_data_t` makes the same call for `rx_fifo_counter` / `tx_fifo_counter`.
- Rate and overflow fields are `uint32_t` since they can climb into the thousands over a
  mission's lifetime, same reasoning as `radio_data_t`'s packet counters.
- Units matter here and are documented per-field: occupancy and overflow are plain
  **chunk counts**, generation/tx rate are **chunks/s** (Task4 samples on a fixed 1-second
  period, so "per period" and "per second" are numerically identical).

## Why a length-1 mailbox queue instead of a mutex

The first version of this used a mutex around the struct. A mutex protects a region of
code, and every caller is responsible for correctly taking and giving it back — miss a
`xSemaphoreGive()` on an error path and every future caller blocks forever.

What's actually needed here is narrower: one task publishes a value, other tasks read the
*latest* value. FreeRTOS has a purpose-built pattern for exactly that — a queue of length 1
(a "mailbox"). `xQueueCreate(1, sizeof(HousekeepingData_t))` creates a queue that holds
exactly one item, and the locking that would otherwise be manual with a mutex happens
inside the queue's own implementation instead.

- **`xQueueOverwrite()`** replaces whatever's in the single slot instead of requiring it to
  be empty first — a normal `xQueueSend()` would only succeed once on a length-1 queue.
- **`xQueuePeek()`** copies the item out *without* removing it, so the same reading stays
  available for every future reader until Task4 overwrites it with the next period's data —
  unlike `xQueueReceive()`, which would empty the mailbox after the first read.
- **0-tick timeout on the peek** — if TT&C reads before Task4's first period completes, the
  mailbox is genuinely empty; returning `pdFALSE` immediately (instead of blocking) lets the
  caller decide to skip that field this cycle rather than stall waiting for data that isn't
  there yet.

## Task4's sampling loop

```c
vTaskDelayUntil(&xLastWakeTime, xPeriod);
```
`vTaskDelayUntil()` targets a fixed point in time rather than "N ms from whenever the call
happens," which matters here specifically because `chunk_generation_rate` is a delta
computed against a fixed period length — if the period itself drifted, the rate wouldn't
mean anything consistent.

```c
data.chunk_generation_rate = generated - prev_generated;
```
`g_chunks_generated` only ever increases, so subtracting last period's value from this
period's gives "how many chunks were generated in the last second" — the simplest
calculation that satisfies "rate," easy to defend in a report.

## Why sampling stops entirely once the pipeline goes idle

The first pass at this printed a line every second forever, even once the mock pipeline
had nothing left to do — pure console noise, since occupancy/rate/overflow all settle to a
fixed value once the run is over. The fix isn't just suppressing prints: it's detecting
"nothing has changed for two consecutive periods" and breaking out of the sampling loop
entirely, so the task stops doing real work (re-reading counters, calling
`xQueueOverwrite()`, comparing structs) rather than just going quiet on the console while
still running every second in the background.

```c
if (have_sample && memcmp(&data, &prev_data, sizeof(data)) == 0)
{
    idle_periods++;
}
...
if (idle_periods >= IDLE_PERIODS_BEFORE_STOP)
{
    break;
}
```
Two consecutive unchanged periods (not one) before declaring the pipeline idle, as a small
safety margin against a brief pause between mock events being mistaken for the true end.
After `break`, the task parks in its own `for (;;) vTaskDelay(...)` loop — the same pattern
Task1/Task2/Task3 already use once they're done — since a FreeRTOS task function must never
return.

**Worth calling out as a design tradeoff**: this permanently retires housekeeping once the
mock `mock_events[]` array is exhausted, which is correct for this fixed, finite test
dataset. A flight version would more likely reset `idle_periods` and resume sampling on new
activity, since real payload events could arrive at any time.

## The one change needed in Task 1

`chunk_overflow_count` could never become non-zero as long as Task 1 sent with
`portMAX_DELAY`, since that blocks forever instead of ever failing. Task 1's send is now
bounded (`pdMS_TO_TICKS(50)`), and a failed send increments `g_chunks_lost` and drops the
chunk rather than stalling the whole pipeline waiting for space that might not come — a
more realistic model of what a congested onboard buffer does on real hardware.

## What I took away

- A shared status struct works well as a *pull-based* contract between two modules that
  don't otherwise need to know about each other — the writer never needs to know who (if
  anyone) is reading, and the reader never blocks waiting for the writer.
- FreeRTOS's length-1 queue "mailbox" pattern is a genuinely better fit than a mutex for
  "always expose the latest value" — it removes an entire class of lock-management bugs
  (forgotten unlocks, deadlocks) by construction, not just by discipline.
- Sampling *and* reporting are two different things worth handling separately — throttling
  the print without also stopping the sampling work still leaves a task doing pointless
  work every period; the idle-detection had to gate both.