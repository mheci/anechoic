#include "AnechoicLadspaPlugin.h"

/*
 * LADSPA entry points, as C functions so hosts with C linkage can call them.
 * Do not define _init/_fini: those symbols belong to crti.o on modern GNU ld.
 */

extern "C" {

const LADSPA_Descriptor *
ladspa_descriptor(plugin_index_t index) {
    return collection<AnechoicMono, AnechoicStereo>::get_ladspa_descriptor(index);
}

} // extern "C"
