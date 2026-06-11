#include <stdio.h>
#include <string.h>
#include <time.h>
#include "fsm.h"

void fsm_init(fsm_t *f) {
    memset(f, 0, sizeof(*f));
    f->state             = STATE_NORMAL;
    f->params.t1_rssi    = -65;
    f->params.t2_rssi    = -80;
    f->params.stable_sec = 10;
    f->params.sample_n   = 20;
    f->params.sample_k   = 15;
    f->state_entered     = time(NULL);
}

static void push_sample(fsm_t *f, int8_t rssi) {
    int cap = f->params.sample_n < FSM_MAX_SAMPLES
            ? f->params.sample_n : FSM_MAX_SAMPLES;
    f->samples[f->sample_head % cap] = rssi;
    f->sample_head++;
    if (f->sample_count < cap) f->sample_count++;
}

static int count_above(fsm_t *f, int8_t thresh) {
    int cap = f->params.sample_n < FSM_MAX_SAMPLES
            ? f->params.sample_n : FSM_MAX_SAMPLES;
    int n = 0;
    for (int i = 0; i < f->sample_count && i < cap; i++)
        if (f->samples[i] >= thresh) n++;
    return n;
}

static int stabilized(fsm_t *f) {
    if ((time(NULL) - f->state_entered) < (time_t)f->params.stable_sec)
        return 0;
    if (f->sample_count < f->params.sample_n)
        return 0;
    return count_above(f, f->params.t1_rssi) >= f->params.sample_k;
}

static void enter_state(fsm_t *f, fsm_state_t next) {
    fprintf(stderr, "[FSM] %s → %s\n",
            fsm_state_str(f->state), fsm_state_str(next));
    f->state         = next;
    f->state_entered = time(NULL);
    f->sample_count  = 0;
    f->sample_head   = 0;
}

fsm_state_t fsm_update(fsm_t *f, int8_t rssi, int wifi_up) {
    push_sample(f, rssi);

    switch (f->state) {
    case STATE_NORMAL:
        if (!wifi_up || rssi < f->params.t1_rssi)
            enter_state(f, STATE_WARNING);
        break;

    case STATE_WARNING:
        if (!wifi_up || rssi < f->params.t2_rssi)
            enter_state(f, STATE_LORA);
        else if (wifi_up && rssi >= f->params.t1_rssi && stabilized(f))
            enter_state(f, STATE_NORMAL);
        break;

    case STATE_LORA:
        /* Wi-Fi 신호 복구 감지 → WARNING 진입해서 안정화 후 NORMAL */
        if (wifi_up && rssi >= f->params.t1_rssi)
            enter_state(f, STATE_WARNING);
        break;
    }
    return f->state;
}

void fsm_set_params(fsm_t *f, const fsm_params_t *p) {
    f->params = *p;
}

const char *fsm_state_str(fsm_state_t s) {
    switch (s) {
    case STATE_NORMAL:  return "NORMAL";
    case STATE_WARNING: return "WARNING";
    case STATE_LORA:    return "LORA";
    default:            return "?";
    }
}
