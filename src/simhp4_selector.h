#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SIMHP4_MACHINE_PDP7 = 0,
    SIMHP4_MACHINE_MICROVAX2 = 1
} simhp4_machine_id_t;

typedef enum {
    SIMHP4_OS_UNIX_V0 = 0,
    SIMHP4_OS_BSD43 = 1
} simhp4_os_id_t;

typedef struct {
    simhp4_machine_id_t machine;
    simhp4_os_id_t os;
} simhp4_selector_choice_t;

simhp4_selector_choice_t simhp4_selector_run(void);
void simhp4_selector_show_not_ready(simhp4_selector_choice_t choice);

#ifdef __cplusplus
}
#endif
