#include "mod_struct.h"

/* -------------------------------------------------------
 * Global player instance (single player)
 * ------------------------------------------------------- */
#pragma bss-name(push, "MODBSS")
ModPlayer mod;
#pragma bss-name(pop)
