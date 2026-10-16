/* falcon-bios: the one variable modrt.h's inline delay reads (see there).
 * Every module links its own copy, and so does the core, where it stays
 * NULL. */

#include <hw/modrt.h>

void (*modrt_tick)(void);
