#ifndef HOUSEKEEPING_H
#define HOUSEKEEPING_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"

/* ---------------------------------------------------------------------
 * Housekeeping (telemetry) data structure.
 *
 * Plays the same role here as radio_data_t does in the TTC 2.0 firmware
 * (firmware/devices/radio/radio_data.h): one struct that this module
 * keeps continuously up to date, and that another module (TT&C) reads
 * whenever it assembles a housekeeping packet. This module never
 * transmits anything itself -- it only maintains the numbers.
 * --------------------------------------------------------------------- */
typedef struct
{
    uint8_t  raw_fifo_occupancy;     /* chunks currently waiting in xQueue1_RawChunks (Task1 -> Task2) */
    uint8_t  ordered_fifo_occupancy; /* chunks currently waiting in xQueue2_OrderedChunks (Task2 -> Task3) */
    uint32_t chunk_generation_rate;  /* chunks produced by Task1 during the last housekeeping period */
    uint32_t chunk_tx_rate;          /* chunks encoded/handed off by Task3 during the last housekeeping period */
    uint32_t chunk_overflow_count;   /* cumulative count of chunks dropped because a queue was full */
} HousekeepingData_t;

/* Call this once in main(), BEFORE vTaskStartScheduler(). It creates the
 * length-1 queue that carries the latest housekeeping snapshot. */
void Housekeeping_Init(void);

/* The periodic task that keeps the housekeeping data current. Registered
 * with xTaskCreate() in main.c exactly like Task1/Task2/Task3 already are. */
void vTask4_HousekeepingGenerator(void *pvParameters);

/* Thread-safe read for any other task (e.g. TT&C) that wants the latest
 * housekeeping snapshot to drop into a packet. Returns pdTRUE if a value
 * was available (Task4 has run at least once), pdFALSE otherwise. */
BaseType_t Housekeeping_GetSnapshot(HousekeepingData_t *out);

#endif /* HOUSEKEEPING_H */