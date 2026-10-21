#include "modules.h"

#include <string.h>

static const struct {
    const char *id;
    const char *label;
    bool        removable;
    /* On for everything that existed before modules did, so an old
       config keeps meaning what it meant. A module that writes
       somebody's words to disk starts off. */
    bool        default_on;
} MODULES[] = {
    /* Listed though it cannot be switched off: an operator looking at
       the list should see what the phone is built on, not only the
       parts that happen to be optional. */
    { SP_MODULE_SIP_CORE,      "SIP",             false, true  },
    { SP_MODULE_RECORDING,     "Recording",       true,  true  },
    { SP_MODULE_AUDIO_QUALITY, "Audio quality",   true,  true  },
    { SP_MODULE_SIP_TRACE,     "SIP trace",       true,  true  },
    { SP_MODULE_TRANSCRIPT,    "Call transcript", true,  false },
};

#define N_MODULES ((int)(sizeof MODULES / sizeof MODULES[0]))

int sp_module_count(void) {
    return N_MODULES;
}

const char *sp_module_id_at(int i) {
    return (i < 0 || i >= N_MODULES) ? NULL : MODULES[i].id;
}

const char *sp_module_label_at(int i) {
    return (i < 0 || i >= N_MODULES) ? NULL : MODULES[i].label;
}

bool sp_module_removable_at(int i) {
    return (i < 0 || i >= N_MODULES) ? false : MODULES[i].removable;
}

bool sp_module_default_at(int i) {
    return (i < 0 || i >= N_MODULES) ? false : MODULES[i].default_on;
}

int sp_module_index_of(const char *id) {
    if (!id)
        return -1;
    for (int i = 0; i < N_MODULES; i++)
        if (strcmp(MODULES[i].id, id) == 0)
            return i;
    return -1;
}
