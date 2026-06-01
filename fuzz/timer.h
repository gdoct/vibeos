#pragma once
#include <stdint.h>
extern uint64_t g_now_ticks;
static inline uint64_t timer_ticks(void){ return g_now_ticks; }
