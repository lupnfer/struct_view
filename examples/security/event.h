#pragma once
#include <stdint.h>
typedef struct { char name[32]; int age; } PersonInfo;
typedef struct { int x, y, w, h; int tags[2]; } Box;   // Box with tags[2]
typedef struct {
    uint64_t timestamp;
    PersonInfo* person;   // pointer member (key struct) — ptr=true
    Box rect;             // embedded member (key struct) — ptr=false (rect also has tags)
    int feature_ids[8];   // scalar array (new)
    Box boxes[4];         // struct array (new, each box has tags → nested combo §9a)
} Event;
