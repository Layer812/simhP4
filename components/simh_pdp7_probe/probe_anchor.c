#include "simhp4_probe.h"
#include "sim_defs.h"

extern char sim_name[];
extern DEVICE *sim_devices[];

const char *simhp4_pdp7_compile_probe_version(void)
{
    return "R0A5-persistent-shell";
}

const char *simhp4_pdp7_machine_name(void)
{
    return sim_name;
}

int simhp4_pdp7_linked_device_count(void)
{
    int n = 0;
    while (sim_devices[n] != NULL)
        ++n;
    return n;
}
