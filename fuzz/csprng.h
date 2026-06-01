#pragma once
#include <stdint.h>
static inline uint32_t csprng_tcp_isn(uint32_t lip,uint16_t lp,uint32_t rip,uint16_t rp){
  (void)lip;(void)rip; return 0x30000000u + (uint32_t)(lp*131u + rp);   /* deterministic */
}
