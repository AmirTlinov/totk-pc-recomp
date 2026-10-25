// Weak stand-ins for everything gfx.h declares, so the runtime links while M4 tasks T2-T6 land one by one. Each
// function stops with its name; each back-end table is zero-filled with the name "unimplemented". A task's strong
// definition replaces the stand-in at link time. Deleted by M4 T7.
#include <stdio.h>

#include "gfx.h"

#define WEAK __attribute__((weak))

WEAK void oe_push_message(uint32_t message) { (void)message; rt_fatal("gfx: oe_push_message is not implemented"); }
WEAK void oe_set_focus(int in_focus) { (void)in_focus; rt_fatal("gfx: oe_set_focus is not implemented"); }
WEAK const OvNvState *ov_nv_state(void) { rt_fatal("gfx: ov_nv_state is not implemented"); }


WEAK const GfxBackend gfx_null_backend = {.name = "unimplemented"};

#define STUB_PROBE(fn, name) WEAK int fn(void) { printf(name ": not built yet\n"); return 2; }
STUB_PROBE(probe_nvn_tex, "probe-nvn-tex")
STUB_PROBE(probe_vkmem, "probe-vkmem")
STUB_PROBE(probe_astc, "probe-astc")
