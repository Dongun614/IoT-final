/*
 * lora_bridge.c
 * 서버용 TTGO UART → Mosquitto MQTT 브리지
 *
 * server.ino 출력 포맷 (JSON 라인):
 *   {"type":"rx","rssi":-72,"snr":8.5,"len":11,"data":"0102aabbcc..."}
 *   {"type":"ready","role":"server"}   ← 무시
 *   {"type":"error","msg":"..."}       ← stderr 출력
 *
 * 빌드: gcc -o lora_bridge lora_bridge.c -lmosquitto
 * 실행: ./lora_bridge /dev/lora_server
 *       (udev rule 미설정 시: ./lora_bridge /dev/ttyUSB0)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <mosquitto.h>

/* ─── 설정 ─────────────────────────────────────────── */
#define MQTT_HOST       "localhost"
#define MQTT_PORT       1883
#define MQTT_KEEPALIVE  60
#define BAUD_RATE       B115200
#define TOPIC_FMT       "robot/%d/status"
#define TOPIC_BUF       64

/* ─── 페이로드 구조체 (로봇 RPi와 동일하게 맞춰야 함) ─── */
/*
 * | robot_id | seq    | ts     | state | rssi  | temp   |
 * | uint8 1B | u16 2B | u32 4B | u8 1B | i8 1B | i16 2B |
 * 총 11 byte
 *
 * TTGO 아두이노가 UART로 내려줄 때:
 *   [0xAA][11바이트 페이로드][lora_rssi int8][lora_snr int8][0x0A(\n)]
 *   → 총 15 byte + newline
 */
#pragma pack(push, 1)
typedef struct {
    uint8_t  robot_id;
    uint16_t seq;
    uint32_t ts;
    uint8_t  state;      /* 0=NORMAL 1=WARNING 2=LORA_ONLY */
    int8_t   wifi_rssi;
    int16_t  temp;       /* °C × 100, 예: 2350 = 23.50°C */
} RobotPayload;
#pragma pack(pop)

/* UART 라인 버퍼 크기 */
#define LINE_BUF_SIZE  512

/* ─── 전역 ─────────────────────────────────────────── */
static volatile int g_running = 1;
static struct mosquitto *g_mosq = NULL;

/* ─── 시그널 핸들러 ─────────────────────────────────── */
static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* ─── UART 초기화 ───────────────────────────────────── */
static int uart_open(const char *dev) {
    int fd = open(dev, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "[bridge] UART open 실패: %s — %s\n", dev, strerror(errno));
        return -1;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        fprintf(stderr, "[bridge] tcgetattr 실패: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    cfsetospeed(&tty, BAUD_RATE);
    cfsetispeed(&tty, BAUD_RATE);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_iflag &= ~IGNBRK;
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN]  = 1;
    tty.c_cc[VTIME] = 5;   /* 0.5초 타임아웃 */

    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "[bridge] tcsetattr 실패: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    printf("[bridge] UART 열림: %s @ 115200\n", dev);
    return fd;
}

/* ─── MQTT 콜백 ─────────────────────────────────────── */
static void on_connect(struct mosquitto *mosq, void *obj, int rc) {
    (void)mosq; (void)obj;
    if (rc == 0) {
        printf("[bridge] Mosquitto 연결 성공\n");
    } else {
        fprintf(stderr, "[bridge] Mosquitto 연결 실패: %d\n", rc);
    }
}

static void on_disconnect(struct mosquitto *mosq, void *obj, int rc) {
    (void)mosq; (void)obj;
    if (rc != 0) {
        fprintf(stderr, "[bridge] Mosquitto 연결 끊김 (rc=%d), 재연결 시도 중...\n", rc);
    }
}

/* ─── hex 문자열 → 바이너리 디코딩 ─────────────────── */
/* hex: "0102aabb..." , out: 바이너리 버퍼, max_len: 버퍼 크기
 * 반환: 디코딩된 바이트 수, 실패 시 -1 */
static int hex_decode(const char *hex, uint8_t *out, int max_len) {
    int len = (int)strlen(hex);
    if (len % 2 != 0) return -1;
    int n = len / 2;
    if (n > max_len) return -1;
    for (int i = 0; i < n; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%02x", &byte) != 1) return -1;
        out[i] = (uint8_t)byte;
    }
    return n;
}

/* ─── JSON 라인에서 값 추출 (외부 라이브러리 없이) ──── */
/* {"key":value} 에서 정수/실수 값 추출.
 * 반환: 1=성공, 0=키 없음 */
static int json_get_int(const char *json, const char *key, long *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    char *end;
    *out = strtol(p, &end, 10);
    return (end != p) ? 1 : 0;
}

static int json_get_float(const char *json, const char *key, float *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    char *end;
    *out = strtof(p, &end);
    return (end != p) ? 1 : 0;
}

/* "data":"hexstring" 에서 hex 문자열 추출 */
static int json_get_str(const char *json, const char *key, char *out, int max_len) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    int i = 0;
    while (*p && *p != '"' && i < max_len - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return i;
}

/* ─── JSON 라인 파싱 + 페이로드 디코딩 + MQTT publish ── */
static void process_line(const char *line) {
    /* type 필드 확인 */
    if (strstr(line, "\"type\":\"ready\"")) {
        printf("[bridge] TTGO 준비 완료: %s\n", line);
        return;
    }
    if (strstr(line, "\"type\":\"error\"")) {
        fprintf(stderr, "[bridge] TTGO 오류: %s\n", line);
        return;
    }
    if (!strstr(line, "\"type\":\"rx\"")) {
        /* rx 이외 메시지 무시 */
        return;
    }

    /* rssi, snr 추출 */
    long  lora_rssi_l = 0;
    float lora_snr    = 0.0f;
    json_get_int  (line, "rssi", &lora_rssi_l);
    json_get_float(line, "snr",  &lora_snr);
    int8_t lora_rssi = (int8_t)lora_rssi_l;

    /* data hex 문자열 추출 */
    char hex[512] = {0};
    if (json_get_str(line, "data", hex, sizeof(hex)) <= 0) {
        fprintf(stderr, "[bridge] data 필드 없음: %s\n", line);
        return;
    }

    /* hex → 바이너리 */
    uint8_t raw[256];
    int raw_len = hex_decode(hex, raw, sizeof(raw));
    if (raw_len < (int)sizeof(RobotPayload)) {
        fprintf(stderr, "[bridge] 페이로드 길이 부족: %d bytes (필요: %d)\n",
                raw_len, (int)sizeof(RobotPayload));
        return;
    }

    /* 바이너리 → RobotPayload (little-endian, packed) */
    RobotPayload p;
    memcpy(&p, raw, sizeof(RobotPayload));

    /* MQTT publish용 JSON 생성 */
    char json[256];
    snprintf(json, sizeof(json),
        "{\"robot_id\":%d,\"seq\":%d,\"ts\":%u,"
        "\"state\":%d,\"wifi_rssi\":%d,\"temp\":%.2f,"
        "\"lora_rssi\":%d,\"lora_snr\":%.1f}",
        p.robot_id, p.seq, p.ts,
        p.state, p.wifi_rssi, p.temp / 100.0f,
        lora_rssi, lora_snr
    );

    char topic[TOPIC_BUF];
    snprintf(topic, sizeof(topic), TOPIC_FMT, p.robot_id);

    int ret = mosquitto_publish(g_mosq, NULL, topic,
                                (int)strlen(json), json, 0, false);
    if (ret != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[bridge] MQTT publish 실패: %s\n", mosquitto_strerror(ret));
    } else {
        const char *state_str[] = {"NORMAL", "WARNING", "LORA_ONLY"};
        printf("[bridge] → %s | seq=%d state=%s wifi_rssi=%d lora_rssi=%d snr=%.1f temp=%.2f\n",
               topic, p.seq,
               p.state < 3 ? state_str[p.state] : "?",
               p.wifi_rssi, lora_rssi, lora_snr, p.temp / 100.0f);
    }
}

/* ─── 메인 루프 ─────────────────────────────────────── */
int main(int argc, char *argv[]) {
    const char *uart_dev = (argc > 1) ? argv[1] : "/dev/lora_server";

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* UART 열기 */
    int uart_fd = uart_open(uart_dev);
    if (uart_fd < 0) return 1;

    /* Mosquitto 초기화 */
    mosquitto_lib_init();
    g_mosq = mosquitto_new("lora_bridge", true, NULL);
    if (!g_mosq) {
        fprintf(stderr, "[bridge] mosquitto_new 실패\n");
        close(uart_fd);
        return 1;
    }

    mosquitto_connect_callback_set(g_mosq, on_connect);
    mosquitto_disconnect_callback_set(g_mosq, on_disconnect);

    if (mosquitto_connect(g_mosq, MQTT_HOST, MQTT_PORT, MQTT_KEEPALIVE) != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[bridge] Mosquitto 초기 연결 실패 — Mosquitto 브로커가 실행 중인지 확인\n");
        mosquitto_destroy(g_mosq);
        mosquitto_lib_cleanup();
        close(uart_fd);
        return 1;
    }

    /* MQTT 루프 스레드 시작 */
    mosquitto_loop_start(g_mosq);

    printf("[bridge] 시작. UART=%s → MQTT %s:%d\n", uart_dev, MQTT_HOST, MQTT_PORT);
    printf("[bridge] 종료: Ctrl+C\n");

    /* ── UART 수신 루프 — '\n' 단위 JSON 라인 ── */
    char line[LINE_BUF_SIZE];
    int  line_len = 0;

    while (g_running) {
        uint8_t byte;
        int n = read(uart_fd, &byte, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[bridge] UART read 오류: %s\n", strerror(errno));
            break;
        }
        if (n == 0) continue;

        if (byte == '\r') continue;  /* CR 무시 */

        if (byte == '\n') {
            /* 라인 완성 → 파싱 */
            if (line_len > 0) {
                line[line_len] = '\0';
                process_line(line);
            }
            line_len = 0;
            continue;
        }

        /* 라인 버퍼에 추가 */
        if (line_len < (int)sizeof(line) - 1) {
            line[line_len++] = (char)byte;
        } else {
            /* 버퍼 오버플로 — 라인 버림 */
            fprintf(stderr, "[bridge] 라인 버퍼 오버플로, 버림\n");
            line_len = 0;
        }
    }

    printf("[bridge] 종료 중...\n");
    mosquitto_loop_stop(g_mosq, true);
    mosquitto_disconnect(g_mosq);
    mosquitto_destroy(g_mosq);
    mosquitto_lib_cleanup();
    close(uart_fd);
    return 0;
}