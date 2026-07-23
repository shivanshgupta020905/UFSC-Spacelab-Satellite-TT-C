#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "payload_chunk.h"
#include "ngham.h"
#include "housekeeping.h"

QueueHandle_t xQueue1_RawChunks;
QueueHandle_t xQueue2_OrderedChunks;

/* Pipeline health counters, shared across Task1/Task2/Task3 and read by
 * Task4 (housekeeping) to report on. 'volatile' because they're written by
 * one task and read by another - it tells the compiler not to cache these
 * in a register, so Task4 always sees the latest value another task wrote. */
volatile uint32_t g_chunks_generated = 0;
volatile uint32_t g_chunks_forwarded = 0;
volatile uint32_t g_chunks_encoded   = 0;
volatile uint32_t g_chunks_lost      = 0;

typedef struct {
    uint16_t occultation_id;
    uint16_t data_size;
} MockEvent_t;

static const MockEvent_t mock_events[] = {
    { .occultation_id = 1, .data_size = 100 },
    { .occultation_id = 2, .data_size = 45  },
    { .occultation_id = 3, .data_size = 70  },
};

#define NUM_MOCK_EVENTS (sizeof(mock_events) / sizeof(mock_events[0]))

/* ---------- TASK 1: Payload Reader / Fragmenter ---------- */
void vTask1_PayloadMemReader(void *pvParameters)
{
    (void) pvParameters;

    for (int e = 0; e < (int)NUM_MOCK_EVENTS; e++)
    {
        uint16_t occ_id   = mock_events[e].occultation_id;
        uint16_t data_len = mock_events[e].data_size;
        uint8_t mock_payload[256];
        for (int i = 0; i < data_len; i++)
        {
            mock_payload[i] = (uint8_t)(0x10 + (e * 0x30) + i);
        }

        uint8_t total_chunks = (data_len + CHUNK_DATA_SIZE - 1) / CHUNK_DATA_SIZE;

        printf("[Task1-PayloadMemReader] occultation %d: %d bytes -> %d chunk(s)\n",
               occ_id, data_len, total_chunks);
        fflush(stdout);

        int bytes_remaining = data_len;
        int offset = 0;

        for (uint8_t seq = 1; seq <= total_chunks; seq++)
        {
            PayloadChunk_t chunk;
            memset(&chunk, 0, sizeof(chunk));

            uint8_t this_chunk_size = (bytes_remaining < CHUNK_DATA_SIZE) ? (uint8_t)bytes_remaining: CHUNK_DATA_SIZE;

            chunk.occultation_id  = occ_id;
            chunk.sequence_number = seq;
            chunk.total_chunks    = total_chunks;
            chunk.data_size       = this_chunk_size;
            chunk.timestamp       = xTaskGetTickCount();
            memcpy(chunk.data, &mock_payload[offset], this_chunk_size);

            if (xQueueSend(xQueue1_RawChunks, &chunk, pdMS_TO_TICKS(50)) == pdTRUE)
            {
                g_chunks_generated++;
            }
            else
            {
                g_chunks_lost++;
                printf("[Task1-PayloadMemReader] WARNING: raw queue full, dropped occ=%d seq=%d\n",
                       occ_id, seq);
                fflush(stdout);
            }

            offset          += this_chunk_size;
            bytes_remaining -= this_chunk_size;

            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }

    printf("[Task1-PayloadMemReader] all occultations fragmented. Task finished.\n");
    fflush(stdout);

    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

/* ---------- TASK 2: FIFO Queue Manager ---------- */
void vTask2_QueueManager(void *pvParameters)
{
    (void) pvParameters;
    PayloadChunk_t chunk;

    printf("[Task2-QueueManager] started.\n");
    fflush(stdout);

    for (;;)
    {
        if (xQueueReceive(xQueue1_RawChunks, &chunk, pdMS_TO_TICKS(3000)) == pdTRUE)
        {
            g_chunks_forwarded++;
            printf("[Task2-FifoManager] forwarding occ=%d seq=%d/%d bytes=%d time=%lu\n",chunk.occultation_id,chunk.sequence_number,chunk.total_chunks,chunk.data_size, (unsigned long)chunk.timestamp);
            fflush(stdout);

            xQueueSend(xQueue2_OrderedChunks, &chunk, portMAX_DELAY);
        }
        else
        {
            printf("[Task2-QueueManager] no more chunks arriving. Total forwarded: %lu\n",
                   (unsigned long)g_chunks_forwarded);
            fflush(stdout);
            for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

/* ---------- TASK 3: NGHam Encoder ---------- */
void vTask3_NGHamEncoder(void *pvParameters)
{
    (void) pvParameters;
    PayloadChunk_t chunk;

    printf("[Task3-NGHamEncoder] started.\n");
    fflush(stdout);

    for (;;)
    {
        if (xQueueReceive(xQueue2_OrderedChunks, &chunk, pdMS_TO_TICKS(3000)) == pdTRUE)
        {
            g_chunks_encoded++;

            /* Serialize chunk metadata + data into the NGHam payload */
            uint8_t ngham_payload[64];
            int p = 0;
            ngham_payload[p++] = (uint8_t)(chunk.occultation_id >> 8);
            ngham_payload[p++] = (uint8_t)(chunk.occultation_id & 0xFF);
            ngham_payload[p++] = chunk.sequence_number;
            ngham_payload[p++] = chunk.total_chunks;
            ngham_payload[p++] = chunk.data_size;
            /* Timestamp (MSB first) */
            ngham_payload[p++] = (uint8_t)(chunk.timestamp >> 24);
            ngham_payload[p++] = (uint8_t)(chunk.timestamp >> 16);
            ngham_payload[p++] = (uint8_t)(chunk.timestamp >> 8);
            ngham_payload[p++] = (uint8_t)(chunk.timestamp);
            memcpy(&ngham_payload[p], chunk.data, chunk.data_size);
            p += chunk.data_size;

            uint8_t ngham_packet[300];
            int packet_len = ngham_build_packet(ngham_payload, (uint8_t)p, ngham_packet);

            if (packet_len < 0)
            {
                printf("[Task3-NGHamEncoder] ERROR: payload too large to encode!\n");
                fflush(stdout);
                continue;
            }

            printf("\n=====================================================\n");
            printf("NGHam Frame\n");
            printf("=====================================================\n");

            printf("Occultation ID : %u\n", chunk.occultation_id);
            printf("Sequence       : %u/%u\n",
                   chunk.sequence_number,
                   chunk.total_chunks);
            printf("Payload Size   : %u bytes\n", chunk.data_size);
            printf("Frame Length   : %d bytes\n\n", packet_len);

            /* ---------- Preamble ---------- */
            printf("Preamble : ");
            for(int i = 0; i < 4; i++)
                printf("%02X ", ngham_packet[i]);
            printf("\n");

            /* ---------- Sync Word ---------- */
            printf("Sync Word: ");
            for(int i = 4; i < 8; i++)
                printf("%02X ", ngham_packet[i]);
            printf("\n");

            /* ---------- Size Tag ---------- */
            printf("Size Tag : ");
            for(int i = 8; i < 11; i++)
                printf("%02X ", ngham_packet[i]);
            printf("\n");

            /* ---------- Header ---------- */
            printf("Header   : %02X\n", ngham_packet[11]);

            /* ---------- Payload ---------- */
            printf("Payload  : ");
            for(int i = 12; i < 12 + p; i++)
               printf("%02X ", ngham_packet[i]);
            printf("\n");

            /* ---------- CRC ---------- */
            printf("CRC      : %02X %02X\n",
                   ngham_packet[12 + p],
                   ngham_packet[13 + p]);

            /* ---------- Padding ---------- */
            int padding = ngham_size_table[1].rs_k - (1 + p + 2);

            printf("Padding  : ");
            for(int i = 0; i < padding; i++)
                printf("%02X ", ngham_packet[14 + p + i]);
            printf("\n");

            /* ---------- Reed-Solomon Parity ---------- */
            int parity_start = 12 + ngham_size_table[1].rs_k;
            int parity_bytes = ngham_size_table[1].rs_n - ngham_size_table[1].rs_k;

            printf("RS Parity: ");
            for(int i = 0; i < parity_bytes; i++)
                printf("%02X ", ngham_packet[parity_start + i]);
            printf("\n");

            printf("=====================================================\n\n");
            fflush(stdout);
        }
        else
        {
            printf("[Task3-NGHamEncoder] no more chunks arriving. Total encoded: %lu\n",
                   (unsigned long)g_chunks_encoded);
            fflush(stdout);
            for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

int main(void)
{
    printf("=== CubeSat Packet Pipeline - Task 1 + 2 + 3 + 4 Test ===\n\n");
    fflush(stdout);

    xQueue1_RawChunks     = xQueueCreate(10, sizeof(PayloadChunk_t));
    xQueue2_OrderedChunks = xQueueCreate(10, sizeof(PayloadChunk_t));

    Housekeeping_Init(); // initialize the housekeeping module and its queue for Task 4

    xTaskCreate(vTask1_PayloadMemReader,       "Task1", configMINIMAL_STACK_SIZE * 2, NULL, 1, NULL);
    xTaskCreate(vTask2_QueueManager,           "Task2", configMINIMAL_STACK_SIZE * 2, NULL, 1, NULL);
    xTaskCreate(vTask3_NGHamEncoder,           "Task3", configMINIMAL_STACK_SIZE * 4, NULL, 1, NULL);
    xTaskCreate(vTask4_HousekeepingGenerator,  "Task4", configMINIMAL_STACK_SIZE * 2, NULL, 1, NULL);

    vTaskStartScheduler();

    for (;;);
    return 0;
}

void vAssertCalled(const char *pcFile, unsigned long ulLine)
{
    printf("ASSERT FAILED: %s, line %lu\n", pcFile, ulLine);
    fflush(stdout);
    for (;;);
}

void vApplicationMallocFailedHook(void)
{
    printf("MALLOC FAILED! Out of heap memory.\n");
    fflush(stdout);
    for (;;);
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void) xTask;
    printf("STACK OVERFLOW in task: %s\n", pcTaskName);
    fflush(stdout);
    for (;;);
}
