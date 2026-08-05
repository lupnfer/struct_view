#pragma once
#include <stdint.h>
typedef struct {
    uint64_t timestamp;
    int feature_ids[8];
} DriftEvent;
