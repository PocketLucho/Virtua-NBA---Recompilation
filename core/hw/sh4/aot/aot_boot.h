#pragma once
#include "types.h"
// BIOS-free boot (see aot_boot.cpp)
bool aot_boot_snapshot_available();
bool aot_boot_use_fallback();
bool aot_boot_pending();
void aot_boot_rearm();
bool aot_boot_hook(u32 pc);
bool aot_boot_bios_free();
bool aot_bios_window(u32 addr);
