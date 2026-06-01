#pragma once
#include <stdint.h>
extern uint64_t g_now_ticks;
typedef struct task task_t;
typedef struct wait_queue { int _x; } wait_queue_t;
static inline void sched_lock(void){}
static inline void sched_unlock(void){}
static inline void wait_queue_sleep_locked(wait_queue_t *){ g_now_ticks++; } /* time passes while blocked */
static inline void wait_queue_wake_all_locked(wait_queue_t *){}
static inline task_t *task_current(void){ return (task_t*)1; }
static inline void task_create(const char*, void(*)(void*), void*){}
static inline void ksleep_ms(unsigned){}
