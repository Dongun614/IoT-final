#pragma once
#include <stdint.h>
#include <time.h>

typedef enum {
    STATE_NORMAL  = 0,
    STATE_WARNING = 1,
    STATE_LORA    = 2
} fsm_state_t;

typedef struct {
    int8_t t1_rssi;    /* NORMAL → WARNING  (default -65 dBm) */
    int8_t t2_rssi;    /* WARNING → LORA    (default -80 dBm) */
    int    stable_sec; /* stabilization window in seconds      */
    int    sample_n;   /* total samples required in window     */
    int    sample_k;   /* min samples above T1 to stabilize    */
} fsm_params_t;

#define FSM_MAX_SAMPLES 64

typedef struct {
    fsm_state_t  state;
    fsm_params_t params;
    int8_t       samples[FSM_MAX_SAMPLES];
    int          sample_head;
    int          sample_count;
    time_t       state_entered;
} fsm_t;

void        fsm_init(fsm_t *f);
fsm_state_t fsm_update(fsm_t *f, int8_t rssi, int wifi_up);
void        fsm_set_params(fsm_t *f, const fsm_params_t *p);
const char *fsm_state_str(fsm_state_t s);
