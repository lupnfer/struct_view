#pragma once
#include <stdint.h>
typedef struct { char name[32]; int age; } PersonInfo;
typedef struct { int x, y, w, h; } Box;
typedef struct {
    uint64_t timestamp;
    PersonInfo* person;   // pointer member (key struct) — ptr=true
    Box rect;             // embedded member (key struct) — ptr=false
} Event;
