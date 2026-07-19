#ifndef PAYLOAD_CHUNK_H
#define PAYLOAD_CHUNK_H

#include <stdint.h>

#define CHUNK_DATA_SIZE 32   /* placeholder size, matches Amrit's approach */

typedef struct {
    uint16_t occultation_id;     /* which event generated this data */
    uint8_t  sequence_number;    /* which chunk is this (1-indexed) */
    uint32_t timestamp;       /* timestamp for this chunk */
    uint8_t  total_chunks;
    uint8_t  data_size;          /* how many valid bytes in 'data' */
    uint8_t  data[CHUNK_DATA_SIZE]; /* the actual payload data */
} PayloadChunk_t;

#endif /* PAYLOAD_CHUNK_H */
