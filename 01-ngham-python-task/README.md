# Task 1 — NGHam Protocol in Python (PyNGHam)

## What this task was actually asking

Before touching any C code or MSP430 firmware, my mentor wanted me to first understand NGHam
as a protocol — not as embedded code, but as pure logic. The idea was: use the existing
`pyngham` Python library (which already implements the full NGHam spec), feed it some fake
telemetry data, and watch what comes out the other end. This way I'd understand what "encoding
a packet" actually means before I try to write it myself in C for the MSP430.

So this isn't a from-scratch implementation — it's me poking at an existing library to learn:
- how a payload turns into a full NGHam frame
- how Reed-Solomon error correction actually recovers corrupted bytes (not just in theory)
- how the "extension packets" (id / status / time-of-hour) get packed together
- how a real ground station would decode this stuff byte-by-byte as it streams in from a radio

## Why Python and not C for this first step

Honestly — speed of iteration. If I had started directly in C on the MSP430 side, every small
mistake means a rebuild + reflash + serial monitor cycle. In Python I can just run the script
and immediately see printed output for every stage: original payload, encoded packet, decoded
packet, error positions, etc. It's basically a sandbox to build intuition before doing the real
firmware version (which is Task 3 below).

## Section-by-section walkthrough of `ngham_test.py`

### 1. Building a fake telemetry payload
```python
payload_bytes = struct.pack(">H B I I H H H H H H I I", ...)
```
This is just packing a bunch of telemetry fields (device id, firmware version, voltages,
currents, temperature, tx/rx counters) into raw bytes, in big-endian order (that's what the
`>` at the start means). I pulled the actual field list from **Table 3.3 of the TTC 2.0
documentation** so this isn't totally made-up data — it mirrors what the real telemetry frame
would contain.

`pyngham` wants a plain Python `list` of integers (not bytes), so I convert with `list(...)`
right after.

### 2. Basic encode → decode round trip
```python
packet = ngham.encode(payload)
decoded, errors, error_positions = ngham.decode(packet)
```
This is the simplest possible test: encode the payload, immediately decode it back, and check
`decoded == payload`. If this doesn't match, nothing else downstream matters, so this is the
first sanity check.

### 3. Corrupting bytes on purpose (testing Reed-Solomon)
```python
corrupted_packet[12] = 0xFF
corrupted_packet[18] = 0x00
corrupted_packet[25] = 0xAB
```
This is the part I found most interesting. NGHam wraps the payload in a Reed-Solomon RS(47,31)
code (for the smallest size class), which means it can correct up to **8 corrupted bytes** in
the encoded block. So I manually flipped 3 bytes to garbage values and ran `decode()` again —
and it still recovered the exact original payload. That's the whole point of RS coding on a
satellite link: bit flips from RF noise shouldn't kill the packet.

### 4. Pushing past the error-correction limit
```python
over_limit_packet[11] ^= 0xFF
...
over_limit_packet[27] ^= 0xFF
```
Here I corrupted **9 bytes**, one more than the RS(47,31) code can fix (max is 8). The decoder
correctly gives up and returns an empty payload instead of silently returning garbage — which
is exactly the behavior you want on a satellite. Better to know a packet failed than to trust
a payload full of undetected bit errors.

### 5. Streaming decode, byte-by-byte
```python
for i, byte in enumerate(packet[8:]):
    result, errs, positions = stream.decode_byte(byte)
```
Real radios don't hand you a complete packet all at once — they hand you a byte at a time as
it comes off the air. So `decode_byte()` simulates that: feed it one byte, it keeps internal
state, and eventually (once it's collected sync word + size tag + all data) it returns the full
decoded payload. I skip the first 8 bytes here (`packet[8:]`) because those are the preamble
and sync word, and the actual radio hardware strips those out before the microcontroller ever
sees the byte stream — so in a real firmware scenario, decode_byte would just start right where
this loop starts.

### 6. SPP packets — talking to the radio module itself
```python
spp = PyNGHamSPP()
rx_pkt = spp.encode_rx_pkt(-120, -85, 0, 0, payload)
```
SPP here stands for the "Serial Peripheral Protocol" that FloripaSat-2's radio module (S-band or
UHF) uses to talk to the onboard computer over UART/serial, separate from the actual over-the-air
NGHam frame. There are 4 packet types I tested:
- **RX packet** — radio telling the computer "I received this over the air," including signal
  strength (RSSI) and noise floor
- **TX packet** — computer telling the radio "please transmit this," e.g. a reset telecommand
  (parameter id 24, from Table 3.3)
- **Command packet** — plain text config strings sent to the radio hardware itself, like setting
  frequency (`FREQ 145900000` = 145.9 MHz, straight from TTC 2.0 Chapter 5 Step 4)
- **Local packet** — the radio reporting its own status back ("radio ok")

This matters because the satellite's onboard computer and the radio module are two separate
pieces of hardware talking over a wire — SPP is the "local" language between them, while NGHam
is the "over the air" language for talking to the ground station.

### 7. Extension packets — bundling multiple pieces of info into one frame
```python
ext = PyNGHamExtension()
ext_payload = ext.append_id_pkt(...)
ext_payload = ext.append_stat_pkt(...)
ext_payload = ext.append_toh_pkt(...)
```
Instead of sending one NGHam packet per piece of info, extension packets let you stack multiple
smaller "sub-packets" into a single payload before it goes through NGHam encoding:
- **ID packet** — which satellite is transmitting (callsign, e.g. `PY0EGO`)
- **Stat packet** — a full telemetry snapshot (voltage, temp, RSSI, noise floor, tx/rx counters)
- **TOH packet** — "time of hour," basically a lightweight timestamp in microseconds since the
  top of the current hour, so the ground station can line packets up in time

One annoying detail I ran into: `decode_stat_pkt` and the TOH decode both have a bug in the
version of the library I'm using where the normal `.decode()` call doesn't parse them properly.
The workaround was calling the internal method directly (`ext._decode_stat_pkt(stat_data)`) with
manually sliced byte ranges, and manually unpacking the TOH bytes with bit shifts instead of
relying on the library's decoder. Not elegant, but it works and it's clearly commented in the
code for exactly this reason.

One more thing worth noting: RSSI and noise floor are naturally negative dBm values (like -85
dBm), but they get stored as **unsigned bytes** in the packet. So `-85` becomes `256 - 85 = 171`
before encoding, and I convert it back with `stat['signal'] - 256 if stat['signal'] > 127 else
stat['signal']` after decoding.

### 8. Wrapping the whole extension bundle in an NGHam frame
```python
ngham_ext = PyNGHam()
ext_packet = ngham_ext.encode(ext_payload)
```
Last step — take the combined ID + Stat + TOH payload and run it through a *normal* NGHam
encode, same as step 2. This is the realistic end-to-end picture: on the satellite side you'd
build up one extension payload with everything you want to send, then NGHam-encode that whole
thing once before transmitting. On the ground station side, you'd NGHam-decode first, and only
*then* run the extension decoder on the result.

## What I took away from this before moving to C

- NGHam separates two jobs cleanly: the **outer layer** (preamble, sync word, size tag, RS
  parity) worries about surviving RF noise, and the **inner payload** is just whatever data you
  want to send — telemetry, commands, whatever.
- Reed-Solomon isn't just "extra bytes for safety," it has a hard mathematical limit on how many
  byte errors it can fix, and understanding that limit is important for later flagging when a
  downlink pass is too noisy.
- Streaming decode (`decode_byte`) is the realistic model for firmware — you never get a full
  packet as one clean array, you get one byte at a time from a UART interrupt, and that's exactly
  how I structured the C version in Task 3.
