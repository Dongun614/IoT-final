#pragma once
#include <stdint.h>

#pragma pack(1)
typedef struct {
    uint8_t  robot_id;
    uint16_t seq;
    uint32_t ts;
    uint8_t  state;
    int8_t   rssi;
    int16_t  temp;
} robot_payload_t;
#pragma pack()

#define PAYLOAD_SIZE ((int)sizeof(robot_payload_t))  /* 11 bytes */

/* UART command bytes: Mac/RPi → TTGO */
#define UART_CMD_TX     0x01
#define UART_CMD_TXPOW  0x02
#define UART_CMD_SLEEP  0x03
#define UART_CMD_WAKE   0x04
