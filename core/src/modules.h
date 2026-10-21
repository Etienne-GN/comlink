/*
 * The module registry.
 *
 * A module is a named capability that can be switched off: an id as
 * spelled in the config, a label for a person, and whether it may be
 * removed at all. Not a loadable plugin -- recording and audio
 * quality thread through call.c and land inside sp_call_record, so
 * what this provides is one place that knows which capabilities
 * exist, rather than isolation.
 *
 * Everything that gates on a module reads this table. A second list
 * anywhere is the bug this file exists to prevent.
 */
#ifndef SP_MODULES_H
#define SP_MODULES_H

#include "comlink/core.h"

/* Ids, in the order a frontend should show them. */
#define SP_MODULE_SIP_CORE      "sip_core"
#define SP_MODULE_RECORDING     "recording"
#define SP_MODULE_AUDIO_QUALITY "audio_quality"
#define SP_MODULE_SIP_TRACE     "sip_trace"
#define SP_MODULE_TRANSCRIPT    "transcript"

int         sp_module_count(void);
const char *sp_module_id_at(int i);
const char *sp_module_label_at(int i);
bool        sp_module_removable_at(int i);
/* What a config that never mentions the module means. */
bool        sp_module_default_at(int i);

/* -1 when no module has this id. */
int sp_module_index_of(const char *id);

#endif /* SP_MODULES_H */
