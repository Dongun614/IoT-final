#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "payload.h"
#include "fsm.h"

/* ------------------------------------------------------------------ RSSI */

static int    g_mock_enabled = 0;
static int8_t g_mock_value   = -50;

static void rssi_set_mock(int enabled, int8_t value) {
    g_mock_enabled = enabled;
    g_mock_value   = value;
}

#ifdef __APPLE__
#define AIRPORT \
    "/System/Library/PrivateFrameworks/Apple80211.framework" \
    "/Versions/Current/Resources/airport"

static int8_t rssi_read(void) {
    if (g_mock_enabled) return g_mock_value;
    FILE *fp = popen(AIRPORT " -I 2>/dev/null", "r");
    if (!fp) return -127;
    char line[256];
    int8_t result = -127;
    while (fgets(line, sizeof(line), fp)) {
        int val;
        if (sscanf(line, " agrCtlRSSI: %d", &val) == 1) { result = (int8_t)val; break; }
    }
    pclose(fp);
    return result;
}
#else
static int8_t rssi_read(void) {
    if (g_mock_enabled) return g_mock_value;
    FILE *f = fopen("/proc/net/wireless", "r");
    if (!f) return -127;
    char line[256];
    fgets(line, sizeof(line), f);
    fgets(line, sizeof(line), f);
    int8_t result = -127;
    while (fgets(line, sizeof(line), f)) {
        char name[32]; int status; float link, level;
        if (sscanf(line, " %31[^:]: %d %f %f", name, &status, &link, &level) == 4) {
            result = (int8_t)level; break;
        }
    }
    fclose(f);
    return result;
}
#endif

static int rssi_wifi_connected(void) {
    if (g_mock_enabled) return g_mock_value != -127;
    return rssi_read() != -127;
}

/* ------------------------------------------------------------------ UART */

typedef struct { int fd; } uart_t;

static int uart_open(uart_t *u, const char *dev) {
    u->fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (u->fd < 0) { perror("uart open"); return -1; }
    struct termios tty;
    if (tcgetattr(u->fd, &tty) != 0) { perror("tcgetattr"); return -1; }
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    tty.c_cflag  = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CRTSCTS);
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_lflag  = 0;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL | IGNCR);
    tty.c_oflag  = 0;
    tty.c_cc[VMIN] = 0; tty.c_cc[VTIME] = 0;
    if (tcsetattr(u->fd, TCSANOW, &tty) != 0) { perror("tcsetattr"); return -1; }
    tcflush(u->fd, TCIOFLUSH);
    return 0;
}

static void uart_close(uart_t *u) {
    if (u->fd >= 0) { close(u->fd); u->fd = -1; }
}

static int uart_write_all(uart_t *u, const uint8_t *buf, int len) {
    int total = 0;
    while (total < len) {
        int n = (int)write(u->fd, buf + total, (size_t)(len - total));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(1000); continue; }
            perror("uart write"); return -1;
        }
        total += n;
    }
    return total;
}

static int uart_send_packet(uart_t *u, const robot_payload_t *pkt) {
    uint8_t cmd = UART_CMD_TX, nl = '\n';
    if (uart_write_all(u, &cmd, 1) < 0) return -1;
    if (uart_write_all(u, (const uint8_t *)pkt, PAYLOAD_SIZE) < 0) return -1;
    if (uart_write_all(u, &nl, 1) < 0) return -1;
    return 0;
}

static int uart_cmd_sleep(uart_t *u) {
    uint8_t buf[2] = { UART_CMD_SLEEP, '\n' };
    return uart_write_all(u, buf, 2) < 0 ? -1 : 0;
}

static int uart_cmd_wake(uart_t *u) {
    uint8_t buf[2] = { UART_CMD_WAKE, '\n' };
    return uart_write_all(u, buf, 2) < 0 ? -1 : 0;
}

static int uart_read_line(uart_t *u, char *buf, int maxlen) {
    static char ibuf[512];
    static int  ilen = 0;
    uint8_t tmp[64];
    int n = (int)read(u->fd, tmp, sizeof(tmp));
    if (n > 0)
        for (int i = 0; i < n && ilen < (int)sizeof(ibuf) - 1; i++)
            ibuf[ilen++] = (char)tmp[i];
    char *nl = (char *)memchr(ibuf, '\n', (size_t)ilen);
    if (!nl) return 0;
    int linelen = (int)(nl - ibuf);
    if (linelen >= maxlen) linelen = maxlen - 1;
    memcpy(buf, ibuf, (size_t)linelen);
    buf[linelen] = '\0';
    int remaining = ilen - (linelen + 1);
    memmove(ibuf, nl + 1, (size_t)remaining);
    ilen = remaining;
    return linelen;
}

/* --------------------------------------------------------------- RINGBUF */

#define RING_SIZE 64

typedef struct {
    robot_payload_t buf[RING_SIZE];
    int head, tail, count;
} ring_buf_t;

static void ring_init(ring_buf_t *r)  { memset(r, 0, sizeof(*r)); }
static int  ring_empty(ring_buf_t *r) { return r->count == 0; }
static int  ring_full(ring_buf_t *r)  { return r->count >= RING_SIZE; }

static int ring_push(ring_buf_t *r, const robot_payload_t *pkt) {
    if (ring_full(r)) return -1;
    r->buf[r->tail] = *pkt;
    r->tail = (r->tail + 1) % RING_SIZE;
    r->count++;
    return 0;
}

static int ring_pop(ring_buf_t *r, robot_payload_t *pkt) {
    if (ring_empty(r)) return -1;
    *pkt = r->buf[r->head];
    r->head = (r->head + 1) % RING_SIZE;
    r->count--;
    return 0;
}

/* ------------------------------------------------------------------- UDP */

static int                g_udp_fd   = -1;
static struct sockaddr_in g_udp_addr;

static void udp_init(const char *ip, int port) {
    g_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_fd < 0) { perror("udp socket"); return; }
    memset(&g_udp_addr, 0, sizeof(g_udp_addr));
    g_udp_addr.sin_family      = AF_INET;
    g_udp_addr.sin_port        = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &g_udp_addr.sin_addr);
    fprintf(stderr, "[UDP] Wi-Fi path → %s:%d\n", ip, port);
}

/* ------------------------------------------------------------------ MAIN */

static volatile int g_running = 1;
static pthread_mutex_t g_mock_mtx = PTHREAD_MUTEX_INITIALIZER;

static void sig_handler(int s) { (void)s; g_running = 0; }

static int16_t read_temp(void) {
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f) return 4000;
    int millideg = 0;
    fscanf(f, "%d", &millideg);
    fclose(f);
    return (int16_t)(millideg / 10);
}

typedef struct { fsm_t *fsm; } stdin_arg_t;

static void *stdin_thread(void *arg) {
    fsm_t *fsm = ((stdin_arg_t *)arg)->fsm;
    char line[64];
    while (g_running && fgets(line, sizeof(line), stdin)) {
        int val;
        if (sscanf(line, "rssi %d", &val) == 1) {
            pthread_mutex_lock(&g_mock_mtx);
            rssi_set_mock(1, (int8_t)val);
            pthread_mutex_unlock(&g_mock_mtx);
            fprintf(stderr, "[MOCK] RSSI → %d dBm\n", val);
        } else if (strncmp(line, "rssi off", 8) == 0) {
            pthread_mutex_lock(&g_mock_mtx);
            rssi_set_mock(0, 0);
            pthread_mutex_unlock(&g_mock_mtx);
            fprintf(stderr, "[MOCK] disabled, using real RSSI\n");
        } else if (sscanf(line, "t1 %d", &val) == 1) {
            fsm->params.t1_rssi = (int8_t)val;
            fprintf(stderr, "[FSM] T1 → %d dBm\n", val);
        } else if (sscanf(line, "t2 %d", &val) == 1) {
            fsm->params.t2_rssi = (int8_t)val;
            fprintf(stderr, "[FSM] T2 → %d dBm\n", val);
        } else if (strncmp(line, "quit", 4) == 0) {
            g_running = 0;
        }
    }
    return NULL;
}

typedef enum { TX_WIFI, TX_LORA } tx_path_t;

static tx_path_t decide_path(fsm_state_t state, int wifi_up) {
    if (state == STATE_LORA) return TX_LORA;
    if (state == STATE_WARNING && !wifi_up) return TX_LORA;
    return TX_WIFI;
}

static void send_wifi(const robot_payload_t *pkt, const char *tag) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"seq\":%u,\"ts\":%u,\"state\":\"%s\",\"rssi\":%d,"
        "\"temp\":%.2f,\"channel\":\"wifi\",\"robot_id\":%u%s%s}\n",
        pkt->seq, pkt->ts,
        pkt->state == 0 ? "NORMAL" : pkt->state == 1 ? "WARNING" : "LORA",
        (int)(int8_t)pkt->rssi, pkt->temp / 100.0,
        pkt->robot_id,
        tag[0] ? ",\"tag\":\"" : "",
        tag[0] ? tag : "");

    if (g_udp_fd >= 0)
        sendto(g_udp_fd, buf, (size_t)n, 0,
               (struct sockaddr *)&g_udp_addr, sizeof(g_udp_addr));

    /* 로컬 로그 */
    fwrite(buf, 1, (size_t)n, stdout);
    fflush(stdout);
}

static int send_lora(uart_t *uart, const robot_payload_t *pkt, const char *tag) {
    if (uart->fd < 0) {
        fprintf(stderr, "[WARN] LoRa UART unavailable, seq=%u dropped\n", pkt->seq);
        return -1;
    }
    if (uart_send_packet(uart, pkt) < 0) return -1;
    fprintf(stderr, "[LORA→] seq=%u state=%u rssi=%d%s%s\n",
            pkt->seq, pkt->state, (int)(int8_t)pkt->rssi,
            tag[0] ? " tag=" : "", tag);
    return 0;
}

int main(int argc, char **argv) {
    const char *uart_dev    = "/dev/tty.wchusbserial58DD0146281";
    const char *server_ip   = "127.0.0.1";
    int         server_port = 5000;
    uint8_t     robot_id    = 1;
    int         mock_arg    = 0;
    int8_t      mock_val    = -50;
    int         interval_ms = 1000;
    int         fast_test   = 0;
    int         stable_sec  = -1;
    int         sample_n    = -1;
    int         sample_k    = -1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--uart") && i+1 < argc)
            uart_dev = argv[++i];
        else if (!strcmp(argv[i], "--server-ip") && i+1 < argc)
            server_ip = argv[++i];
        else if (!strcmp(argv[i], "--server-port") && i+1 < argc)
            server_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--id") && i+1 < argc)
            robot_id = (uint8_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mock-rssi") && i+1 < argc) {
            mock_arg = 1;
            mock_val = (int8_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--interval") && i+1 < argc)
            interval_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fast-test"))
            fast_test = 1;
        else if (!strcmp(argv[i], "--stable-sec") && i+1 < argc)
            stable_sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sample-n") && i+1 < argc)
            sample_n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sample-k") && i+1 < argc)
            sample_k = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            fprintf(stderr,
                "Usage: %s [--uart dev] [--id n] [--mock-rssi dBm] [--interval ms]\n"
                "       [--fast-test] [--stable-sec n] [--sample-n n] [--sample-k n]\n"
                "stdin: rssi <dBm> | rssi off | t1 <dBm> | t2 <dBm> | quit\n",
                argv[0]);
            return 0;
        }
    }

    if (mock_arg) {
        rssi_set_mock(1, mock_val);
        fprintf(stderr, "[MOCK] starting with RSSI=%d dBm\n", mock_val);
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    udp_init(server_ip, server_port);

    uart_t uart = { .fd = -1 };
    if (uart_open(&uart, uart_dev) != 0)
        fprintf(stderr, "[WARN] UART %s unavailable, LoRa TX disabled\n", uart_dev);
    else
        fprintf(stderr, "[UART] opened %s\n", uart_dev);

    fsm_t fsm;
    fsm_init(&fsm);

    /* --fast-test: stable 3s/5샘플/4개 → 전체 사이클 ~15초 안에 완료 */
    if (fast_test) {
        fsm.params.stable_sec = 3;
        fsm.params.sample_n   = 5;
        fsm.params.sample_k   = 4;
        fprintf(stderr, "[FAST-TEST] stable=3s/5/4\n");
    }
    if (stable_sec >= 0) fsm.params.stable_sec = stable_sec;
    if (sample_n   >= 0) fsm.params.sample_n   = sample_n;
    if (sample_k   >= 0) fsm.params.sample_k   = sample_k;

    ring_buf_t ring;
    ring_init(&ring);

    stdin_arg_t sarg = { .fsm = &fsm };
    pthread_t tid;
    pthread_create(&tid, NULL, stdin_thread, &sarg);

    fprintf(stderr,
        "[ROBOT] id=%u  T1=%d T2=%d dBm  stable=%ds/%d/%d\n"
        "[ROBOT] stdin: \"rssi -85\" to force LORA, \"rssi -50\" to recover\n",
        robot_id,
        fsm.params.t1_rssi, fsm.params.t2_rssi,
        fsm.params.stable_sec, fsm.params.sample_n, fsm.params.sample_k);

    uint16_t    seq        = 0;
    fsm_state_t prev_state = STATE_NORMAL;
    tx_path_t   prev_path  = TX_WIFI;

    while (g_running) {
        struct timespec ts_start;
        clock_gettime(CLOCK_MONOTONIC, &ts_start);

        int8_t      rssi    = rssi_read();
        int         wifi_up = rssi_wifi_connected();
        fsm_state_t state   = fsm_update(&fsm, rssi, wifi_up);
        tx_path_t   path    = decide_path(state, wifi_up);

        if (state != prev_state) {
            if (state == STATE_WARNING || state == STATE_LORA) {
                if (uart.fd >= 0) uart_cmd_wake(&uart);
            } else if (state == STATE_NORMAL && prev_state != STATE_NORMAL) {
                if (uart.fd >= 0) uart_cmd_sleep(&uart);
            }
            prev_state = state;
        }

        robot_payload_t pkt = {
            .robot_id = robot_id,
            .seq      = seq++,
            .ts       = (uint32_t)time(NULL),
            .state    = (uint8_t)state,
            .rssi     = rssi,
            .temp     = read_temp(),
        };

        if (path != prev_path && !ring_empty(&ring))
            fprintf(stderr, "[BUF] flushing %d buffered packets via %s\n",
                    ring.count, path == TX_WIFI ? "wifi" : "lora");

        robot_payload_t buffered;
        while (!ring_empty(&ring)) {
            ring_pop(&ring, &buffered);
            if (path == TX_WIFI) send_wifi(&buffered, "flush");
            else                 send_lora(&uart, &buffered, "flush");
        }
        prev_path = path;

        if (path == TX_WIFI) {
            send_wifi(&pkt, "");
        } else {
            if (send_lora(&uart, &pkt, "") < 0)
                if (ring_push(&ring, &pkt) < 0)
                    fprintf(stderr, "[WARN] ring full, seq=%u lost\n", pkt.seq);
        }

        if (seq % 5 == 0)
            fprintf(stderr, "[STATUS] state=%-7s rssi=%4d wifi=%d buf=%d seq=%u\n",
                    fsm_state_str(state), rssi, wifi_up, ring.count, seq);

        if (uart.fd >= 0) {
            char resp[256];
            if (uart_read_line(&uart, resp, sizeof(resp)) > 0)
                fprintf(stderr, "[TTGO←] %s\n", resp);
        }

        struct timespec ts_end;
        clock_gettime(CLOCK_MONOTONIC, &ts_end);
        long elapsed_ms = (ts_end.tv_sec  - ts_start.tv_sec)  * 1000L
                        + (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000L;
        long sleep_ms = interval_ms - elapsed_ms;
        if (sleep_ms > 0) usleep((useconds_t)(sleep_ms * 1000));
    }

    uart_close(&uart);
    g_running = 0;
    pthread_join(tid, NULL);
    fprintf(stderr, "[ROBOT] stopped. sent %u packets total.\n", seq);
    return 0;
}
