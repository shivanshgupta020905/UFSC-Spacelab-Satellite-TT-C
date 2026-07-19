from pyngham import PyNGHam
from pyngham import PyNGHamSPP
from pyngham import PyNGHamExtension
import struct

# create the ngham object
ngham = PyNGHam()

# this is my payload - just some numbers representing telemetry data
# taken from table 3.3 of the TTC 2.0 documentation
# device id, hw version, fw version, time, reset count, mcu voltage,
# mcu current, mcu temp, radio voltage, radio current, tx count, rx count
payload_bytes = struct.pack(">H B I I H H H H H H I I",
    0xCC2A,   # device id
    0x01,     # hardware version
    0x00010203, # firmware version v1.2.3
    2880000,  # time counter in ms
    3,        # reset counter
    3300,     # mcu voltage in mV
    45,       # mcu current in mA
    299,      # mcu temperature in K
    5012,     # radio voltage in mV
    312,      # radio current in mA
    127,      # tx packet count
    58        # rx packet count
)
# convert to list because pyngham wants a list
payload = list(payload_bytes)
print("original payload:", payload)
print("payload length:", len(payload), "bytes")

# encode the payload into a ngham packet
packet = ngham.encode(payload)
print("\nencoded packet:", packet)
print("encoded packet length:", len(packet), "bytes")
# decode the packet back
decoded, errors, error_positions = ngham.decode(packet)
print("\ndecoded payload:", decoded)
print("number of errors:", errors)
print("error positions:", error_positions)

# check if we got back what we sent
if decoded == payload:
    print("success - decoded payload matches original")
else:
    print("something went wrong")
    # now lets test error correction
# make a copy of the packet and mess up some bytes
corrupted_packet = list(packet)
corrupted_packet[12] = 0xFF  # mess up byte 12
corrupted_packet[18] = 0x00  # mess up byte 18
corrupted_packet[25] = 0xAB  # mess up byte 25
print("\ncorrupted 3 bytes in the packet")

# try to decode the corrupted packet
decoded2, errors2, error_positions2 = ngham.decode(corrupted_packet)
print("decoded payload:", decoded2)
print("number of errors corrected:", errors2)
print("error positions:", error_positions2)

if decoded2 == payload:
    print("success - reed solomon fixed the errors")
else:
    print("could not fix errors")
# now lets see what happens when we add too many errors
# rs(47,31) can only fix 8 errors max
over_limit_packet = list(packet)
over_limit_packet[11] ^= 0xFF
over_limit_packet[13] ^= 0xFF
over_limit_packet[15] ^= 0xFF
over_limit_packet[17] ^= 0xFF
over_limit_packet[19] ^= 0xFF
over_limit_packet[21] ^= 0xFF
over_limit_packet[23] ^= 0xFF
over_limit_packet[25] ^= 0xFF
over_limit_packet[27] ^= 0xFF
print("\ncorrupted 9 bytes - more than the rs limit of 8")

decoded3, errors3, error_positions3 = ngham.decode(over_limit_packet)
print("decoded payload:", decoded3)
print("errors:", errors3)

if len(decoded3) == 0:
    print("packet was discarded because too many errors")

# stream decode - feed bytes one at a time like the firmware does
# note: skip first 8 bytes (preamble + sync word) because the radio
# hardware already removes those before the mcu sees the data
stream = PyNGHam()
for i, byte in enumerate(packet[8:]):
    result, errs, positions = stream.decode_byte(byte)
    if result:
        print("\nstream decode finished")
        print("recovered payload:", result)
        if result == payload:
            print("success - stream decode matches original")
        break
spp = PyNGHamSPP()

# rx packet - radio received something from satellite, tells computer
# includes signal strength and noise floor as extra info
rx_pkt = spp.encode_rx_pkt(-120, -85, 0, 0, payload)
rx_decoded = spp.decode(rx_pkt)
print("\nspp rx packet encoded:", rx_pkt)
print("spp rx decoded type:", rx_decoded['type'], "(0 = rx)")
print("spp rx decoded rssi:", rx_decoded['rssi'], "dBm")
print("spp rx decoded noise floor:", rx_decoded['noise_floor'], "dBm")
print("spp rx decoded payload:", rx_decoded['payload'])

# tx packet - computer tells radio to transmit a command to the satellite
# sending reset command: parameter id 24, value 1 from Table 3.3
telecommand = list(struct.pack(">B I", 24, 1))
tx_pkt = spp.encode_tx_pkt(0, telecommand)
tx_decoded = spp.decode(tx_pkt)
print("\nspp tx packet encoded:", tx_pkt)
print("spp tx decoded type:", tx_decoded['type'], "(1 = tx)")
print("spp tx decoded payload:", tx_decoded['payload'])

# command packet - computer sends text config command to radio hardware
# setting frequency to 145.9 MHz from TTC 2.0 Chapter 5 Step 4
cmd_pkt = spp.encode_cmd_pkt(list("FREQ 145900000".encode()))
cmd_decoded = spp.decode(cmd_pkt)
print("\nspp cmd packet encoded:", cmd_pkt)
print("spp cmd decoded type:", cmd_decoded['type'], "(3 = command)")
print("spp cmd decoded command:", bytes(cmd_decoded['payload']).decode())

# local packet - radio sends its own status report to computer
local_pkt = spp.encode_local_pkt(0, list(b"radio ok"))
local_decoded = spp.decode(local_pkt)
print("\nspp local packet encoded:", local_pkt)
print("spp local decoded type:", local_decoded['type'], "(2 = local)")
print("spp local decoded message:", bytes(local_decoded['payload']).decode())
ext = PyNGHamExtension()

# start with empty list, append sub-packets one by one
ext_payload = list()

# id extension - who is transmitting (satellite callsign)
ext_payload = ext.append_id_pkt(ext_payload, ext.encode_callsign("PY0EGO", 1), 127)
print("\nextension payload after id packet:", ext_payload)

# status extension - telemetry stats from Table 3.3
# rssi and noise must be unsigned bytes: -85 = 171, -120 = 136
ext_payload = ext.append_stat_pkt(ext_payload,
    1,    # hw version
    1,    # serial number
    258,  # sw version
    2880, # uptime in seconds
    33,   # voltage in decivolts (33 = 3.3V)
    26,   # temperature celsius
    171,  # rssi -85dBm as unsigned byte (256-85=171)
    136,  # noise -120dBm as unsigned byte (256-120=136)
    58,   # rx ok count
    3,    # rx corrected count
    1,    # rx failed count
    127   # tx count
)
print("extension payload after stat packet:", ext_payload)

# toh extension - time of hour in microseconds
# 900000000 us = 900 seconds = 15 minutes into current hour
ext_payload = ext.append_toh_pkt(ext_payload, 900000000, 1)
print("extension payload after toh packet:", ext_payload)

# decode id packet - works correctly through normal decode()
raw_ext = ext.decode(ext_payload)
id_pkt = raw_ext[0]
print("\nid decoded - callsign:", id_pkt['call_ssid'][0])
print("id decoded - ssid:", id_pkt['call_ssid'][1])
print("id decoded - sequence:", id_pkt['sequence'])

# decode stat packet - call internal method directly due to library bug
# id packet is 11 bytes, stat data starts at index 13 (skip type+length)
stat_data = ext_payload[13:35]
stat = ext._decode_stat_pkt(stat_data)
print("\nstat decoded - hw version:", stat['hw_ver'])
print("stat decoded - uptime:", stat['uptime_s'], "seconds")
print("stat decoded - voltage:", stat['voltage'] / 10, "V")
print("stat decoded - temperature:", stat['temp'], "C")
print("stat decoded - tx count:", stat['cntr_tx'])
print("stat decoded - rx ok:", stat['cntr_rx_ok'])
# convert unsigned bytes back to signed dBm values
signal_dbm = stat['signal'] - 256 if stat['signal'] > 127 else stat['signal']
noise_dbm = stat['noise'] - 256 if stat['noise'] > 127 else stat['noise']
print("stat decoded - rssi:", signal_dbm, "dBm")
print("stat decoded - noise floor:", noise_dbm, "dBm")

# decode toh packet - manual decode due to library bug
# stat packet is 24 bytes, toh data starts at index 37 (skip type+length)
toh_data = ext_payload[37:]
toh_us = (toh_data[0] << 24) | (toh_data[1] << 16) | (toh_data[2] << 8) | toh_data[3]
toh_val = toh_data[4]
print("\ntoh decoded -", toh_us, "microseconds")
print("toh decoded -", toh_us / 1000000, "seconds into the hour")
print("toh decoded - valid:", toh_val)

# encode the full extension payload as a normal ngham packet
# on receiving end: ngham decode first, then extension decode
ngham_ext = PyNGHam()
ext_packet = ngham_ext.encode(ext_payload)
print("\nextension ngham packet:", ext_packet)
print("extension ngham packet length:", len(ext_packet), "bytes")

ext_dec, ext_err, _ = ngham_ext.decode(ext_packet)
print("ngham decode errors:", ext_err)
if ext_dec == ext_payload:
    print("success - extension payload survived ngham encode and decode")
