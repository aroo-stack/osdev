#ifndef PIT_H
#define PIT_H
#include <stdint.h>
void pit_init(uint32_t hz);
void pit_set_phase(int hz);
#endif
