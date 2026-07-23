#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "housekeeping.h"

/* These four are defined in main.c (not 'static' there), so 'extern' here
 * just tells the compiler "this exists elsewhere, link against it." */
extern QueueHandle_t xQueue1_RawChunks;
extern QueueHandle_t xQueue2_OrderedChunks;
extern volatile uint32_t g_chunks_generated;
extern volatile uint32_t g_chunks_encoded;
extern volatile uint32_t g_chunks_lost;

/* Length-1 "mailbox" queue: it only ever holds the single most recent
 * housekeeping snapshot. Writing to it (xQueueOverwrite) and reading from
 * it (xQueuePeek) from different tasks is already safe -- FreeRTOS does
 * the locking inside the queue implementation, so no separate mutex is
 * needed on our side. */
static QueueHandle_t xHousekeepingQueue = NULL;

void Housekeeping_Init(void)
{
    xHousekeepingQueue = xQueueCreate(1, sizeof(HousekeepingData_t));
}
void vTask4_HousekeepingGenerator(void *pvParameters)
{
    (void) pvParameters;

    const TickType_t xPeriod = pdMS_TO_TICKS(1000); /* sample once per second */
    TickType_t xLastWakeTime = xTaskGetTickCount();

    uint32_t prev_generated = 0; /* g_chunks_generated at the end of the previous period */
    uint32_t prev_encoded   = 0; /* g_chunks_encoded at the end of the previous period */

    /* Last reading we COMPUTED (not just last one printed) -- used purely
     * to detect "nothing changed since last period," i.e. pipeline idle. */
    HousekeepingData_t prev_data;
    memset(&prev_data, 0, sizeof(prev_data));
    uint8_t have_sample = 0; /* false until the very first period has run */

    #define IDLE_PERIODS_BEFORE_STOP 2 /* consecutive unchanged periods = "done" */
    uint8_t  idle_periods = 0;
    uint32_t total_reports = 0;

    printf("[Task4-Housekeeping] started.\n");
    fflush(stdout);

    for (;;)
    {
        vTaskDelayUntil(&xLastWakeTime, xPeriod);

        uint32_t generated = g_chunks_generated;
        uint32_t encoded   = g_chunks_encoded;
        uint32_t lost      = g_chunks_lost;

        UBaseType_t raw_waiting     = uxQueueMessagesWaiting(xQueue1_RawChunks);
        UBaseType_t ordered_waiting = uxQueueMessagesWaiting(xQueue2_OrderedChunks);

        HousekeepingData_t data;
        data.raw_fifo_occupancy     = (uint8_t) raw_waiting;
        data.ordered_fifo_occupancy = (uint8_t) ordered_waiting;
        data.chunk_generation_rate  = generated - prev_generated;
        data.chunk_tx_rate          = encoded - prev_encoded;
        data.chunk_overflow_count   = lost;

        /* Still publish every period the loop actually runs -- TT&C must
         * see the true current state whenever this loop is live. Once we
         * break out below, this line simply never executes again, which
         * is fine: the mailbox is left holding the last real, correct
         * reading (all zeros / idle), and that IS the true current state. */
        xQueueOverwrite(xHousekeepingQueue, &data);

        prev_generated = generated;
        prev_encoded   = encoded;

        /* Only print -- and count -- a report when something changed. */
        if (have_sample && memcmp(&data, &prev_data, sizeof(data)) == 0)
        {
            idle_periods++;
        }
        else
        {
            idle_periods = 0;
            printf("============[Task4-Housekeeping]============\n");
            printf(" raw_q=%u chunks \n ord_q=%u chunks \n gen_rate=%lu chunks/s \n tx_rate=%lu chunks/s \n overflow=%lu chunks\n",
                   data.raw_fifo_occupancy,
                   data.ordered_fifo_occupancy,
                   (unsigned long) data.chunk_generation_rate,
                   (unsigned long) data.chunk_tx_rate,
                   (unsigned long) data.chunk_overflow_count);
            fflush(stdout);
            total_reports++;
        }

        prev_data   = data;
        have_sample = 1;

        /* Two unchanged periods in a row => pipeline is genuinely idle.
         * Announce it once, then leave this loop for good. */
        if (idle_periods >= IDLE_PERIODS_BEFORE_STOP)
        {
            printf("[Task4-Housekeeping] pipeline idle -- stopping periodic sampling after %lu report(s).\n",
                   (unsigned long) total_reports);
            fflush(stdout);
            break; /* exits the sampling loop entirely */
        }
    }

    /* Same idle pattern Task1/Task2/Task3 already use: a FreeRTOS task
     * function must never return, so once there's nothing left to do,
     * park here forever instead of doing any more work. */
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

BaseType_t Housekeeping_GetSnapshot(HousekeepingData_t *out)
{
    if (out == NULL) return pdFALSE;

    /* xQueuePeek() copies the item out WITHOUT removing it, so the value
     * stays there for the next reader too (unlike xQueueReceive(), which
     * would empty the queue). Timeout of 0 = don't block if Task4 hasn't
     * written anything yet. */
    return xQueuePeek(xHousekeepingQueue, out, 0);
}