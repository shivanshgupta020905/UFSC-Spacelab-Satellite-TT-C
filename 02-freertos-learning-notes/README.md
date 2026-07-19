# Task 2 — Learning FreeRTOS (Notes)

## Why this task existed

Before I could write the actual payload-reading/packet pipeline (Task 3), I needed to
actually understand FreeRTOS — not just copy-paste `xTaskCreate()` calls without knowing
what's happening underneath. This file is basically my own notes from going through the
official FreeRTOS documentation and connecting each concept to what I'd actually need for
the CubeSat pipeline (tasks talking to each other through queues).

I'm keeping this in "notes + code snippet" form rather than a polished writeup, because
that's genuinely how I studied it — read a concept, try it in a tiny throwaway test, write
down what I understood.

---

## 1. What even is an RTOS, and why does a CubeSat need one?

A normal microcontroller program (like basic Arduino `loop()` style code) runs one thing
after another, in one big loop, forever. That's fine for simple stuff, but on FloripaSat-2
the MCU has to do several things that all "feel" like they're happening at once:
- reading payload data out of memory
- managing a queue of chunks waiting to be sent
- encoding chunks into NGHam packets
- (eventually) actually transmitting over the radio

An RTOS (Real-Time Operating System) — and FreeRTOS specifically — lets you write each of
these as a separate **task**, and the **scheduler** decides which task gets CPU time and
when. It's not true parallelism (there's still one CPU core doing MSP430 work), it's fast
switching between tasks so it *looks* simultaneous.

## 2. Tasks and task states

A task in FreeRTOS is created with:
```c
xTaskCreate(vTask1_PayloadMemReader, "Task1", configMINIMAL_STACK_SIZE * 2, NULL, 1, NULL);
```
The arguments (in order) are: the function to run, a name for debugging, how much stack
space to give it, a parameter pointer (I don't use this, hence `NULL`), the task's
**priority**, and a handle pointer (also `NULL` since I don't need to reference the task
later).

Every task in FreeRTOS is always in one of these states:
- **Running** — actually executing on the CPU right now
- **Ready** — able to run, just waiting for its turn
- **Blocked** — waiting on something, like a delay timer or a queue being empty/full.
  <cite index="4-1">A task will block, entering the Blocked state, if it calls a function like vTaskDelay() until its delay period has expired, or if it's waiting for a queue, semaphore, event group, or notification event.</cite>
- **Suspended** — explicitly paused, won't run until resumed (I don't use this in the
  pipeline, but it exists)

This mattered a lot for my pipeline design: Task 2 and Task 3 both call
`xQueueReceive(..., pdMS_TO_TICKS(3000))`. That means if no data has arrived in 3 seconds,
they were sitting Blocked, not busy-looping and wasting CPU cycles. This is a much more
power-efficient pattern than checking "is there new data?" over and over in a tight loop —
which matters a lot for a satellite running off limited battery power.

## 3. Priorities

I gave all three of my tasks priority `1` (out of `configMAX_PRIORITIES = 5` set in
`FreeRTOSConfig.h`). At first I didn't fully get why this mattered, but the scheduler always
picks the **highest priority Ready task** to actually run. If two tasks have equal priority,
they time-slice between each other (each gets a turn, round-robin style) — which is exactly
why priority `1` for all three worked fine for my pipeline: they're not fighting each other
for urgency, they just each get a turn to check their queue and do their bit of work.

If, say, the NGHam encoder task (Task 3) had a lower priority than the memory reader (Task 1),
and both were Ready at the same time, Task 1 would always win, potentially starving Task 3.
Something to be careful of once this project gets more complex (e.g. if a radio-transmit task
needs to be more time-critical than a background housekeeping task).

## 4. Queues — how tasks talk to each other safely

This is the concept that mattered most for my code. A queue is basically a thread-safe FIFO
buffer — one task can push data in, another task can pop data out, and FreeRTOS handles all
the "what if both tasks try to touch it at the same time" problems for you.

Creating a queue:
```c
xQueue1_RawChunks = xQueueCreate(10, sizeof(PayloadChunk_t));
```
This makes room for 10 items, where each item is the size of one `PayloadChunk_t` struct.

Sending data in:
```c
xQueueSend(xQueue1_RawChunks, &chunk, portMAX_DELAY);
```
`portMAX_DELAY` means "if the queue happens to be full, just wait forever until there's
room" — for my pipeline this is fine since I'm not worried about a queue overflow scenario
yet.

Reading data out:
```c
xQueueReceive(xQueue1_RawChunks, &chunk, pdMS_TO_TICKS(3000));
```
This blocks the calling task for up to 3000 ms waiting for something to appear in the queue.
If something arrives, it copies it into `chunk` and returns `pdTRUE`. If nothing arrives in
time, it returns `pdFALSE` — which I use as my signal that "no more data is coming, this
task's job is basically done for now."

One important detail I had to actually think about: <cite index="9-1">FreeRTOS copies data into a queue by value, not by reference — so calling xQueueSend() copies the entire struct into the queue atomically</cite>. That's actually really convenient for my `PayloadChunk_t` struct, because I don't have to worry about the original `chunk` variable being overwritten or going out of scope after I send it — the queue already has its own copy.

## 5. How this maps onto my actual pipeline (Task 3's code)

Once these two ideas (tasks + queues) clicked, the pipeline design basically wrote itself:

```
Task1 (Payload Reader)  --xQueue1_RawChunks-->  Task2 (Queue Manager)  --xQueue2_OrderedChunks-->  Task3 (NGHam Encoder)
```

- **Task 1** fragments a big mock payload into small chunks (`PayloadChunk_t`) and pushes each
  one into `xQueue1_RawChunks`.
- **Task 2** just receives from Queue 1 and forwards into `xQueue2_OrderedChunks` — right now
  it's basically a pass-through "FIFO manager," but the structure is there so that in a more
  advanced version it could reorder out-of-sequence chunks or check IDs before forwarding.
- **Task 3** receives from Queue 2, and this is where the actual NGHam encoding happens —
  serializing the chunk struct into bytes and building the full NGHam frame (preamble, sync
  word, size tag, header, payload, CRC, padding, RS parity).

None of this would work safely without queues acting as the "hand-off point" between tasks —
without them, I'd have to use shared global variables and manually protect them with mutexes,
which is way more error-prone (race conditions, torn reads, etc.).

## 6. Other config values I had to understand from `FreeRTOSConfig.h`

A few settings I looked up specifically because they're used in the pipeline:
- `configUSE_PREEMPTION 1` — the scheduler can interrupt a running task if a higher-priority
  task becomes Ready, rather than waiting for the running task to yield on its own.
- `configTICK_RATE_HZ 1000` — the system tick happens every 1 ms, which is what
  `pdMS_TO_TICKS()` uses internally to convert my millisecond delays into actual tick counts.
- `configTOTAL_HEAP_SIZE (128 * 1024)` — FreeRTOS's dynamic memory allocator gets 128 KB of
  heap to work with, which is where task stacks and queue storage actually get allocated from
  since `configSUPPORT_DYNAMIC_ALLOCATION` is set to 1.
- `configCHECK_FOR_STACK_OVERFLOW 2` and `vApplicationStackOverflowHook()` — if a task's
  stack size (like `configMINIMAL_STACK_SIZE * 2` for Task 1) isn't big enough and it
  overflows, this hook fires and prints which task blew its stack, instead of silently
  corrupting memory. Genuinely useful for debugging embedded code where a plain segfault
  message doesn't exist.

## Sources I used

- [FreeRTOS Task States documentation](https://www.freertos.org/Documentation/02-Kernel/02-Kernel-features/01-Tasks-and-co-routines/02-Task-states)
- [FreeRTOS Queue Management documentation](https://freertos.org/Documentation/02-Kernel/04-API-references/06-Queues/00-QueueManagement)
- [FreeRTOS Queues concept page](https://freertos.org/Documentation/02-Kernel/02-Kernel-features/02-Queues-mutexes-and-semaphores/01-Queues)
