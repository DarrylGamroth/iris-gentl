#ifndef IRIS_TEST_ALLOCATIONS_H
#define IRIS_TEST_ALLOCATIONS_H
#include <stdint.h>
typedef struct {
    uint64_t consumer_calls, consumer_bytes;
    uint64_t other_calls, other_bytes;
} allocation_counts;
void allocation_begin(void);
void allocation_pause_thread(int pause);
allocation_counts allocation_end(void);
#endif
