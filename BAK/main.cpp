#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <arpa/inet.h>
#include "mdns.h"
#include <sys/poll.h>


static const char *TAG = "keyer";

// ============================================================
// PIN CONFIG (come main.cpp attuale - breadboard già cablata)
// ============================================================
#define PIN_DOT_NUM        2
#define PIN_DASH_NUM       3
#define PIN_OUT_RADIO_NUM  7
#define PIN_BUZZER_NUM     15
#define PIN_WPM_UP_NUM     4
#define PIN_WPM_DOWN_NUM   5
#define PIN_MENU_NUM       6
#define PIN_BYPASS_NUM     10
#define I2C_SDA_PIN_NUM    18
#define I2C_SCL_PIN_NUM    19

#define PIN_DOT       ((gpio_num_t)PIN_DOT_NUM)
#define PIN_DASH      ((gpio_num_t)PIN_DASH_NUM)
#define PIN_OUT_RADIO ((gpio_num_t)PIN_OUT_RADIO_NUM)
#define PIN_BUZZER    ((gpio_num_t)PIN_BUZZER_NUM)
#define PIN_WPM_UP    ((gpio_num_t)PIN_WPM_UP_NUM)
#define PIN_WPM_DOWN  ((gpio_num_t)PIN_WPM_DOWN_NUM)
#define PIN_MENU      ((gpio_num_t)PIN_MENU_NUM)
#define PIN_BYPASS    ((gpio_num_t)PIN_BYPASS_NUM)
#define I2C_SDA_PIN   ((gpio_num_t)I2C_SDA_PIN_NUM)
#define I2C_SCL_PIN   ((gpio_num_t)I2C_SCL_PIN_NUM)

#define PIN_MASK_INPUT ((1ULL<<PIN_DOT_NUM)|(1ULL<<PIN_DASH_NUM)|(1ULL<<PIN_WPM_UP_NUM)|(1ULL<<PIN_WPM_DOWN_NUM)|(1ULL<<PIN_MENU_NUM)|(1ULL<<PIN_BYPASS_NUM))

// ============================================================
// MODES
// ============================================================
#define MODE_STRAIGHT  0
#define MODE_IAMBIC_A  1
#define MODE_IAMBIC_B  2
#define MODE_BUG       3

// ============================================================
// IAMBIC B STATE MACHINE
// ============================================================
#define ELT_IDLE 0
#define ELT_DASH 1
#define ELT_DOT  2
#define ELT_DELAY 3

// ============================================================
// NVS KEYS
// ============================================================
#define NVS_KEY_WPM     "wpm"
#define NVS_KEY_MODE    "mode"
#define NVS_KEY_BYPASS  "bypass"
#define NVS_KEY_VOL     "vol"
#define NVS_KEY_ROLE    "role"
#define NVS_KEY_IP      "rip"
#define NVS_KEY_PORT    "rport"
#define NVS_KEY_DISP    "disp"
#define NVS_KEY_JBLAT   "jblat"
#define NVS_KEY_STA_CNT "stacnt"
#define NVS_KEY_STA_SSID "stassid"
#define NVS_KEY_STA_PW  "stapw"

// ============================================================
// SETTINGS (volatile per accesso cross-task)
// ============================================================
typedef struct {
    int32_t wpm;
    int32_t mode;
    int32_t volume;
    int32_t bypass;
    int32_t role;
    char    remote_ip[16];
    int32_t remote_port;
    int32_t display_enabled;
    int32_t jb_latency;
} keyer_settings_t;

static volatile keyer_settings_t cfg = {
    .wpm = 20,
    .mode = MODE_STRAIGHT,
    .volume = 5,
    .bypass = 0,
    .role = 0,
    .remote_ip = "192.168.1.100",
    .remote_port = 7373,
    .display_enabled = 0,
    .jb_latency = 200,
};

// cfg_mutex will be added in Fase 2 (WiFi)

// ============================================================
// WIFI CREDENTIALS (multi-SSID per STA)
// ============================================================
#define MAX_STA_NETS 4
typedef struct { char ssid[33]; char password[65]; } wifi_cred_t;
static wifi_cred_t sta_creds[MAX_STA_NETS] = {};
static int sta_cred_count = 0;
static volatile bool sta_connected = false;
static volatile bool sta_got_ip = false;
static char sta_ssid_connected[33] = "";
static char sta_ip_str[16] = "";
extern int sta_ap_index;

// ============================================================
// VOLUME TABLE (come .ino originale)
// ============================================================
static const uint32_t vol_table[] = {0, 2, 5, 12, 28, 60, 120, 250, 450, 650, 833};

// ============================================================
// UDP PROTOCOL DEFINITION (usato da send_key e udp_rx)
// ============================================================
typedef struct __attribute__((packed)) {
    uint8_t type;     // 0x00=OFF, 0x01=ON
    uint32_t ts_ms;   // timestamp mittente (ms)
    uint16_t dur_ms;  // durata impulso in ms (0 per ON, valore effettivo per OFF)
    uint8_t seq;      // sequenziale
} cw_packet_t;

// ============================================================
// KEYER STATE
// ============================================================
static volatile int32_t dot_length_us = 60000;
static volatile bool dot_memory = false;
static volatile bool dash_memory = false;
static volatile bool last_sent_was_dot = false;

// Iambic B state
static volatile int curr_elt = ELT_IDLE;
static volatile int next_elt = ELT_IDLE;
static volatile int last_elt = ELT_IDLE;
static volatile int64_t curr_elt_end_time = 0;
static volatile int64_t idle_since = 0;

// ============================================================
// BUZZER (LEDC PWM)
// ============================================================
static void buzzer_init(void) {
    ledc_timer_config_t lt = {};
    lt.speed_mode = LEDC_LOW_SPEED_MODE;
    lt.duty_resolution = LEDC_TIMER_10_BIT;
    lt.timer_num = LEDC_TIMER_0;
    lt.freq_hz = 600;
    lt.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&lt);

    ledc_channel_config_t lc = {};
    lc.speed_mode = LEDC_LOW_SPEED_MODE;
    lc.channel = LEDC_CHANNEL_0;
    lc.timer_sel = LEDC_TIMER_0;
    lc.gpio_num = PIN_BUZZER;
    lc.duty = 0;
    lc.hpoint = 0;
    lc.deconfigure = false;
    ledc_channel_config(&lc);
}

static void buzzer_on(void) {
    uint32_t v = cfg.volume;
    if (v <= 0 || v > 10) v = 0;
    uint32_t duty = vol_table[v];
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void buzzer_off(void) {
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
}

// ============================================================
// RADIO OUTPUT
// ============================================================
static void radio_out(bool state) {
    int32_t b = cfg.bypass;
    if (b) {
        gpio_set_level(PIN_OUT_RADIO, 0);
    } else {
        gpio_set_level(PIN_OUT_RADIO, state ? 1 : 0);
    }
}

static int udp_tx_sock = -1;
static uint8_t udp_tx_seq = 0;
static int64_t udp_tx_on_us = 0;
static volatile bool net_ready = false;
static volatile int tcp_state = 0; // 0=disconnesso 1=connecting(TCP) 2=WS upgrade sent 3=WS connesso
static int64_t tcp_retry_us = 0;   // timestamp prossimo tentativo reconnect
static int64_t tcp_connect_us = 0; // qnd è iniziato il tentativo di connect (per timeout)

// RX: coda per pacchetti ricevuti via WebSocket (net_rx_task la consuma)
static QueueHandle_t rx_pkt_queue = NULL;
static volatile bool ws_client_connected = false;

// Debug counters (atomic per evitare warning -Wvolatile su C++17)
static volatile uint32_t dbg_tx_total;
static volatile uint32_t dbg_tx_sendto_ok;
static volatile uint32_t dbg_tx_sendto_fail;
static volatile uint32_t dbg_rx_total;
static volatile uint32_t dbg_jb_in;
static volatile uint32_t dbg_jb_out;
static volatile uint32_t dbg_jb_drop;

// Event log per debug lato RX
#define EVT_LOG_SIZE 64
typedef struct { int64_t abs_us; uint8_t seq; uint8_t type; } evt_entry_t;
static evt_entry_t evt_log[EVT_LOG_SIZE];
static volatile int evt_log_pos;

#define EVT_LOG(t,s,n) do { \
    int _i = __atomic_fetch_add(&evt_log_pos, 1, __ATOMIC_RELAXED) % EVT_LOG_SIZE; \
    evt_log[_i].abs_us = (n); \
    evt_log[_i].seq = (s); \
    evt_log[_i].type = (t); \
} while(0)

#define DBG_INC(x) __atomic_fetch_add(&(x), 1, __ATOMIC_RELAXED)
#define DBG_ADD(x,v) __atomic_fetch_add(&(x), (v), __ATOMIC_RELAXED)

// Invia frame WebSocket binario con masking (RFC 6455)
static bool ws_send_bin(int sock, const void *data, size_t len) {
    uint8_t frame[32];
    uint8_t mask[4];
    int64_t t = esp_timer_get_time();
    mask[0] = (uint8_t)t; mask[1] = (uint8_t)(t>>8);
    mask[2] = (uint8_t)(t>>16); mask[3] = (uint8_t)(t>>24);
    int off;
    frame[0] = 0x82;
    if (len < 126) {
        frame[1] = 0x80 | (uint8_t)len;
        memcpy(frame+2, mask, 4); off = 6;
    } else {
        frame[1] = 0x80 | 126;
        frame[2] = (uint8_t)(len>>8); frame[3] = (uint8_t)len;
        memcpy(frame+4, mask, 4); off = 8;
    }
    memcpy(frame+off, data, len);
    for (size_t i = 0; i < len; i++) frame[off+i] ^= mask[i&3];
    int ret = send(sock, frame, off + (int)len, 0);
    return ret == off + (int)len;
}

static void send_key(bool state) {
    static bool last_state = false;

    if (state) buzzer_on(); else buzzer_off();
    radio_out(state);

    // TX via WebSocket: invia cambio stato se ruolo=TX, bypass=OFF e WS connesso
    if (cfg.role == 1 && cfg.bypass == 0 && tcp_state == 3 && state != last_state) {
        cw_packet_t pkt;
        pkt.type = state ? 1 : 0;
        int64_t now_us = esp_timer_get_time();
        pkt.ts_ms = (uint32_t)(now_us / 1000);
        pkt.dur_ms = state ? 0 : (uint32_t)((now_us - udp_tx_on_us) / 1000);
        pkt.seq = udp_tx_seq;
        if (state) udp_tx_on_us = now_us;
        DBG_INC(dbg_tx_total);
        if (ws_send_bin(udp_tx_sock, &pkt, sizeof(pkt))) {
            last_state = state;
            udp_tx_seq++;
            DBG_INC(dbg_tx_sendto_ok);
        } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOTCONN || errno == EINPROGRESS || errno == EALREADY) {
            DBG_INC(dbg_tx_sendto_fail);
        } else {
            ESP_LOGW(TAG, "send_key errno=%d (%s), closing WS", errno, strerror(errno));
            DBG_INC(dbg_tx_sendto_fail);
            close(udp_tx_sock); udp_tx_sock = -1; tcp_state = 0;
        }
    }

    // Chiudi socket se non più in ruolo TX
    if (cfg.role != 1 && udp_tx_sock >= 0) {
        close(udp_tx_sock);
        udp_tx_sock = -1;
        tcp_state = 0;
        last_state = false;
        udp_tx_seq = 0;
    }
}

// ============================================================
// NVS
// ============================================================
static void settings_load(void) {
    nvs_handle_t h;
    if (nvs_open("keyer", NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed, using defaults");
        return;
    }

    int32_t v_wpm = cfg.wpm, v_mode = cfg.mode, v_byp = cfg.bypass;
    int32_t v_vol = cfg.volume, v_role = cfg.role, v_port = cfg.remote_port;
    int32_t v_disp = cfg.display_enabled, v_jbl = cfg.jb_latency;
    char v_ip[16]; strcpy(v_ip, (const char*)cfg.remote_ip);

    nvs_get_i32(h, NVS_KEY_WPM, &v_wpm);
    nvs_get_i32(h, NVS_KEY_MODE, &v_mode);
    nvs_get_i32(h, NVS_KEY_BYPASS, &v_byp);
    nvs_get_i32(h, NVS_KEY_VOL, &v_vol);
    nvs_get_i32(h, NVS_KEY_ROLE, &v_role);
    nvs_get_i32(h, NVS_KEY_PORT, &v_port);
    nvs_get_i32(h, NVS_KEY_DISP, &v_disp);
    nvs_get_i32(h, NVS_KEY_JBLAT, &v_jbl);

    size_t len = sizeof(v_ip);
    nvs_get_str(h, NVS_KEY_IP, v_ip, &len);

    // Carica credenziali WiFi STA
    int32_t sta_cnt = 0;
    nvs_get_i32(h, NVS_KEY_STA_CNT, &sta_cnt);
    if (sta_cnt > MAX_STA_NETS) sta_cnt = MAX_STA_NETS;
    sta_cred_count = sta_cnt;
    for (int i = 0; i < sta_cnt; i++) {
        char key[24];
        size_t sz;
        snprintf(key, sizeof(key), "%s%d", NVS_KEY_STA_SSID, i);
        sz = sizeof(sta_creds[i].ssid);
        nvs_get_str(h, key, sta_creds[i].ssid, &sz);
        snprintf(key, sizeof(key), "%s%d", NVS_KEY_STA_PW, i);
        sz = sizeof(sta_creds[i].password);
        nvs_get_str(h, key, sta_creds[i].password, &sz);
    }

    nvs_close(h);

    if (v_wpm < 5 || v_wpm > 70) v_wpm = 20;
    if (v_mode < 0 || v_mode > 3) v_mode = MODE_STRAIGHT;
    if (v_vol < 0 || v_vol > 10) v_vol = 5;
    if (v_byp < 0) v_byp = 0;
    if (v_role < 0 || v_role > 2) v_role = 0;
    if (v_port < 1 || v_port > 65535) v_port = 7373;
    if (v_disp < 0) v_disp = 0;
    if (v_jbl < 50 || v_jbl > 1000) v_jbl = 200;

    cfg.wpm = v_wpm; cfg.mode = v_mode; cfg.bypass = v_byp;
    cfg.volume = v_vol; cfg.role = v_role; cfg.remote_port = v_port;
    cfg.display_enabled = v_disp; cfg.jb_latency = v_jbl;
    strcpy((char*)cfg.remote_ip, v_ip);

    dot_length_us = (1200000 / cfg.wpm);
    ESP_LOGI(TAG, "Loaded: WPM=%ld MODE=%ld VOL=%ld BYP=%ld ROLE=%ld DISP=%ld JBLAT=%ld",
             (long)cfg.wpm, (long)cfg.mode, (long)cfg.volume, (long)cfg.bypass, (long)cfg.role,
             (long)cfg.display_enabled);
}

static void settings_save(void) {
    nvs_handle_t h;
    if (nvs_open("keyer", NVS_READWRITE, &h) != ESP_OK) return;

    int32_t v_wpm = cfg.wpm, v_mode = cfg.mode, v_byp = cfg.bypass;
    int32_t v_vol = cfg.volume, v_role = cfg.role, v_port = cfg.remote_port;

    nvs_set_i32(h, NVS_KEY_WPM, v_wpm);
    nvs_set_i32(h, NVS_KEY_MODE, v_mode);
    nvs_set_i32(h, NVS_KEY_BYPASS, v_byp);
    nvs_set_i32(h, NVS_KEY_VOL, v_vol);
    nvs_set_i32(h, NVS_KEY_ROLE, v_role);
    nvs_set_i32(h, NVS_KEY_PORT, v_port);
    nvs_set_i32(h, NVS_KEY_DISP, cfg.display_enabled);
    nvs_set_i32(h, NVS_KEY_JBLAT, cfg.jb_latency);
    nvs_set_str(h, NVS_KEY_IP, (const char*)cfg.remote_ip);

    // Salva credenziali WiFi STA
    nvs_set_i32(h, NVS_KEY_STA_CNT, sta_cred_count);
    for (int i = 0; i < sta_cred_count; i++) {
        char key[24];
        snprintf(key, sizeof(key), "%s%d", NVS_KEY_STA_SSID, i);
        nvs_set_str(h, key, sta_creds[i].ssid);
        snprintf(key, sizeof(key), "%s%d", NVS_KEY_STA_PW, i);
        nvs_set_str(h, key, sta_creds[i].password);
    }

    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Settings saved");
}

// ============================================================
// PLAY ELEMENT (Iambic A / Bug dot)
// ============================================================
static void play_element(int multiplier) {
    ESP_LOGD(TAG, "play_elt mult=%d dot_us=%ld", multiplier, (long)dot_length_us);
    send_key(true);
    int64_t start = esp_timer_get_time();
    int64_t duration = dot_length_us * multiplier;

    while ((esp_timer_get_time() - start) < duration) {
        esp_task_wdt_reset();
        if (gpio_get_level(PIN_DOT) == 0) dot_memory = true;
        if (gpio_get_level(PIN_DASH) == 0) dash_memory = true;
        vTaskDelay(1);
    }

    send_key(false);

    start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < dot_length_us) {
        esp_task_wdt_reset();
        if (gpio_get_level(PIN_DOT) == 0) dot_memory = true;
        if (gpio_get_level(PIN_DASH) == 0) dash_memory = true;
        vTaskDelay(1);
    }

    last_sent_was_dot = (multiplier == 1);

    if (gpio_get_level(PIN_DOT) == 1 && gpio_get_level(PIN_DASH) == 1) {
        dot_memory = false;
        dash_memory = false;
    }
}

// ============================================================
// KEYER TASK
// ============================================================
static void disp_sleep(void);
static void disp_wake(void);
static bool disp_sleeping = false;
static uint32_t last_activity_ms = 0;
#define DISP_TIMEOUT_MS 15000

static void keyer_task(void *arg) {
    int64_t now;
    int hb_counter = 0;

    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "Keyer task started (WDT subscribed)");

    while (1) {
        esp_task_wdt_reset();
        hb_counter++;
        if (hb_counter % 100 == 0) {
            ESP_LOGI(TAG, "KEYER ALIVE: mode=%ld wpm=%ld vol=%ld byp=%ld tick=%lld",
                     (long)cfg.mode, (long)cfg.wpm, (long)cfg.volume, (long)cfg.bypass,
                     (long long)esp_timer_get_time());
        }

        // RX mode: paddle locale ignorato, l'output arriva via TCP
        if (cfg.role == 2) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        now = esp_timer_get_time();

        // WS reconnect ogni 3s: TCP a remote_ip:80, poi HTTP upgrade
        if (net_ready && cfg.role == 1 && udp_tx_sock < 0) {
            if (now > tcp_retry_us) {
                tcp_retry_us = now + 3000000;
                int sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock >= 0) {
                    int flags = fcntl(sock, F_GETFL, 0);
                    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
                    struct sockaddr_in dest = {};
                    dest.sin_family = AF_INET;
                    dest.sin_port = htons(80);
                    inet_pton(AF_INET, (const char*)cfg.remote_ip, &dest.sin_addr);
                    int ret = connect(sock, (struct sockaddr*)&dest, sizeof(dest));
                    ESP_LOGI(TAG, "WS connect(%s:80) ret=%d errno=%d %s",
                             (const char*)cfg.remote_ip,
                             ret, errno, ret==0?"OK":(ret<0&&errno==EINPROGRESS?"EINPROGRESS":strerror(errno)));
                    if (ret == 0 || (ret < 0 && errno == EINPROGRESS)) {
                        udp_tx_sock = sock;
                        tcp_connect_us = esp_timer_get_time();
                        tcp_state = 1;
                    } else {
                        close(sock);
                    }
                }
            }
        }

        dot_length_us = (1200000 / cfg.wpm);

        bool dot_pressed = (gpio_get_level(PIN_DOT) == 0);
        bool dash_pressed = (gpio_get_level(PIN_DASH) == 0);
        if (dot_pressed || dash_pressed) last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
        int32_t cur_mode = cfg.mode;

        // ============================================================
        // STRAIGHT MODE
        // ============================================================
        if (cur_mode == MODE_STRAIGHT) {
            bool key = dot_pressed || dash_pressed || dot_memory || dash_memory;
            send_key(key);
            dot_memory = false;
            dash_memory = false;
            vTaskDelay(1);
            continue;
        }

        // ============================================================
        // IAMBIC A
        // ============================================================
        if (cur_mode == MODE_IAMBIC_A) {
            if (dot_pressed && dash_pressed) {
                // Squeeze: alterna dot/dash finché tieni entrambi
                last_sent_was_dot = !last_sent_was_dot;
                play_element(last_sent_was_dot ? 1 : 3);
            } else if (dot_pressed || dot_memory) {
                play_element(1);
                dot_memory = false;
            } else if (dash_pressed || dash_memory) {
                play_element(3);
                dash_memory = false;
            } else {
                send_key(false);
                vTaskDelay(1);
            }
            continue;
        }

        // ============================================================
        // IAMBIC B
        // ============================================================
        if (cur_mode == MODE_IAMBIC_B) {
            switch (curr_elt) {
                case ELT_IDLE:
                    send_key(false);
                    vTaskDelay(1);
                    idle_since = now;
                    if (dot_pressed && !dash_pressed) {
                        curr_elt = ELT_DOT;
                        curr_elt_end_time = now + dot_length_us;
                    } else if (!dot_pressed && dash_pressed) {
                        curr_elt = ELT_DASH;
                        curr_elt_end_time = now + 3 * dot_length_us;
                    } else if (dot_pressed && dash_pressed) {
                        curr_elt = ELT_DOT;
                        next_elt = ELT_DASH;
                        curr_elt_end_time = now + dot_length_us;
                    }
                    break;

                case ELT_DOT:
                    send_key(true);
                    if (dash_pressed) next_elt = ELT_DASH;
                    else next_elt = ELT_IDLE;
                    if (now >= curr_elt_end_time) {
                        last_elt = ELT_DOT;
                        curr_elt = ELT_DELAY;
                        curr_elt_end_time = now + dot_length_us;
                        send_key(false);
                        last_sent_was_dot = true;
                    }
                    break;

                case ELT_DASH:
                    send_key(true);
                    if (dot_pressed) next_elt = ELT_DOT;
                    else next_elt = ELT_IDLE;
                    if (now >= curr_elt_end_time) {
                        last_elt = ELT_DASH;
                        curr_elt = ELT_DELAY;
                        curr_elt_end_time = now + dot_length_us;
                        send_key(false);
                        last_sent_was_dot = false;
                    }
                    break;

                case ELT_DELAY:
                    send_key(false);
                    if (now >= curr_elt_end_time) {
                        curr_elt = next_elt;
                        if (curr_elt == ELT_DOT) {
                            curr_elt_end_time = now + dot_length_us;
                        } else if (curr_elt == ELT_DASH) {
                            curr_elt_end_time = now + 3 * dot_length_us;
                        }
                        next_elt = ELT_IDLE;
                    } else {
                        if (last_elt == ELT_DOT && dash_pressed) next_elt = ELT_DASH;
                        if (last_elt == ELT_DASH && dot_pressed) next_elt = ELT_DOT;
                    }
                    break;
            }
            vTaskDelay(1);
            continue;
        }

        // ============================================================
        // BUG
        // ============================================================
        if (cur_mode == MODE_BUG) {
            if (dot_pressed || dot_memory) {
                play_element(1);
                dot_memory = false;
            }
            if (dash_pressed) {
                send_key(true);
            } else if (!dot_pressed && !dot_memory) {
                send_key(false);
            }
            vTaskDelay(1);
            continue;
        }

        // Poll WS socket: POLLIN solo durante upgrade (tcp_state==2)
        if (cfg.role == 1 && udp_tx_sock >= 0) {
            struct pollfd pfd = {}; pfd.fd = udp_tx_sock; pfd.events = POLLOUT;
            if (tcp_state == 2) pfd.events |= POLLIN;
            int pr = poll(&pfd, 1, 0);
            if (pr > 0 && (pfd.revents & (POLLERR|POLLHUP))) {
                ESP_LOGW(TAG, "WS POLLERR/POLLHUP, closing");
                close(udp_tx_sock); udp_tx_sock = -1; tcp_state = 0;
            } else if (pr > 0 && (pfd.revents & POLLOUT) && tcp_state == 1) {
                // TCP connect OK → invio upgrade HTTP
                char req[512];
                int n = snprintf(req, sizeof(req),
                    "GET /ws HTTP/1.1\r\n"
                    "Host: %s:80\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                    "Sec-WebSocket-Version: 13\r\n"
                    "\r\n", (const char*)cfg.remote_ip);
                int ret = send(udp_tx_sock, req, n, 0);
                if (ret == n) {
                    ESP_LOGI(TAG, "WS upgrade request sent");
                    tcp_state = 2;
                } else {
                    ESP_LOGW(TAG, "WS upgrade send failed, closing");
                    close(udp_tx_sock); udp_tx_sock = -1; tcp_state = 0;
                }
            } else if (pr > 0 && (pfd.revents & POLLIN) && tcp_state == 2) {
                char buf[512];
                int n = recv(udp_tx_sock, buf, sizeof(buf)-1, 0);
                if (n > 0) {
                    buf[n] = 0;
                    if (strstr(buf, "101") && strstr(buf, "Switching")) {
                        ESP_LOGI(TAG, "WS connected");
                        tcp_state = 3;
                    } else {
                        ESP_LOGW(TAG, "WS upgrade rejected: %s", buf);
                        close(udp_tx_sock); udp_tx_sock = -1; tcp_state = 0;
                    }
                } else if (n == 0) {
                    close(udp_tx_sock); udp_tx_sock = -1; tcp_state = 0;
                }
            } else if (pr == 0) {
                int so_err = 0; socklen_t sl = sizeof(so_err);
                if (getsockopt(udp_tx_sock, SOL_SOCKET, SO_ERROR, &so_err, &sl) == 0 && so_err != 0) {
                    ESP_LOGW(TAG, "WS SO_ERROR=%d (%s)", so_err, strerror(so_err));
                    close(udp_tx_sock); udp_tx_sock = -1; tcp_state = 0;
                } else if (now - tcp_connect_us > 15000000) {
                    ESP_LOGW(TAG, "WS connect timeout 15s");
                    close(udp_tx_sock); udp_tx_sock = -1; tcp_state = 0;
                }
            } else if (pr < 0) {
                ESP_LOGW(TAG, "WS poll error pr=%d", pr);
                close(udp_tx_sock); udp_tx_sock = -1; tcp_state = 0;
            }
        } else {
            tcp_state = 0;
        }
        vTaskDelay(1);
    }
}

// ============================================================
// MENU / BUTTONS TASK
// ============================================================
// Forward declarations per display (definite dopo startup_test)
static bool disp_init(void);
static void disp_redraw(void);
static i2c_master_bus_handle_t bus_handle = NULL;
static i2c_master_dev_handle_t disp_dev = NULL;
static bool disp_show_ip = true;
static int disp_saved_timer = 0;

static void menu_task(void *arg) {
    uint32_t bypass_start = 0;
    bool bypass_was = false;
    bool bypass_saved = false;
    uint32_t disp_counter = 0;

    while (1) {
        // Decrementa timer messaggio "Saved!"
        if (disp_saved_timer > 0) disp_saved_timer--;

        // Aggiornamento periodico display (~100ms, flush ~26ms @400kHz)
        if (disp_dev) {
            disp_counter++;
            if (disp_counter >= 10) {
                disp_counter = 0;
                disp_redraw();
            }
        }
        bool menu_pressed = (gpio_get_level(PIN_MENU) == 0);
        bool up_pressed = (gpio_get_level(PIN_WPM_UP) == 0);
        bool down_pressed = (gpio_get_level(PIN_WPM_DOWN) == 0);
        bool bypass_pressed = (gpio_get_level(PIN_BYPASS) == 0);

        // Gestione sleep display (come Arduino: 15s inactivity)
        // Prima pressione: solo risveglia, non esegue l'azione del pulsante
        if (menu_pressed || up_pressed || down_pressed || bypass_pressed) {
            last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (disp_sleeping) {
                disp_wake();
                disp_redraw();
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
        }
        if (!disp_sleeping && disp_dev && !menu_pressed && !up_pressed && !down_pressed && !bypass_pressed) {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (now_ms - last_activity_ms > DISP_TIMEOUT_MS) {
                // Controlla paddle rilasciati (dot/dash letti da variabili condivise)
                // keyer_task aggiorna last_activity_ms se paddle premuti
                disp_sleep();
                disp_counter = 0;
            }
        }

        // ============================================================
        // MODE handler (come .ino originale: entra su MODE, resta in
        // attesa di combo UP/DOWN per volume, o rilascia per ciclo mode)
        // ============================================================
        if (menu_pressed) {
            vTaskDelay(pdMS_TO_TICKS(50));  // debounce
            bool combo = false;
            int64_t mode_start = esp_timer_get_time();

            while (gpio_get_level(PIN_MENU) == 0 &&
                   (esp_timer_get_time() - mode_start) < 3000000) {

                if (gpio_get_level(PIN_WPM_UP) == 0) {
                    if (cfg.volume < 10) cfg.volume = cfg.volume + 1;
                    combo = true;
                    ESP_LOGI(TAG, "VOL=%ld", (long)cfg.volume);
                    while (gpio_get_level(PIN_WPM_UP) == 0) vTaskDelay(1);
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                if (gpio_get_level(PIN_WPM_DOWN) == 0) {
                    if (cfg.volume > 0) cfg.volume = cfg.volume - 1;
                    combo = true;
                    ESP_LOGI(TAG, "VOL=%ld", (long)cfg.volume);
                    while (gpio_get_level(PIN_WPM_DOWN) == 0) vTaskDelay(1);
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                vTaskDelay(1);
            }
            if (!combo) {
                cfg.mode = (cfg.mode + 1) % 4;
                const char *modes[] = {"STRAIGHT", "IAMBIC A", "IAMBIC B", "BUG"};
                ESP_LOGI(TAG, "MODE=%s", modes[cfg.mode]);
            }
            vTaskDelay(pdMS_TO_TICKS(200));  // anti-rimbalzo
            continue;
        }

        // ============================================================
        // WPM UP (solo se MODE NON premuto)
        // ============================================================
        if (up_pressed) {
            if (cfg.wpm < 70) cfg.wpm = cfg.wpm + 1;
            dot_length_us = (1200000 / cfg.wpm);
            ESP_LOGI(TAG, "WPM=%ld", (long)cfg.wpm);
            vTaskDelay(pdMS_TO_TICKS(150));
        }

        // ============================================================
        // WPM DOWN (solo se MODE NON premuto)
        // ============================================================
        if (down_pressed) {
            if (cfg.wpm > 5) cfg.wpm = cfg.wpm - 1;
            dot_length_us = (1200000 / cfg.wpm);
            ESP_LOGI(TAG, "WPM=%ld", (long)cfg.wpm);
            vTaskDelay(pdMS_TO_TICKS(150));
        }

        // ============================================================
        // BYPASS handler
        // ============================================================
        if (bypass_pressed && !bypass_was) {
            bypass_start = (uint32_t)(esp_timer_get_time() / 1000);
            bypass_was = true;
            bypass_saved = false;
        } else if (bypass_pressed && bypass_was && !bypass_saved) {
            uint32_t duration = (uint32_t)(esp_timer_get_time() / 1000) - bypass_start;
            if (duration > 2000) {
                settings_save();
                disp_saved_timer = 20;
                bypass_saved = true;
                ESP_LOGI(TAG, "Settings saved");
            }
        } else if (!bypass_pressed && bypass_was && !bypass_saved) {
            if (disp_show_ip) {
                disp_show_ip = false;
                ESP_LOGI(TAG, "IP line hidden, bypass unchanged");
            } else {
                cfg.bypass = !cfg.bypass;
                ESP_LOGI(TAG, "BYPASS=%s", cfg.bypass ? "ON" : "OFF");
            }
            bypass_was = false;
        } else if (!bypass_pressed && bypass_was) {
            bypass_was = false;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ============================================================
// GPIO INIT
// ============================================================
static void gpio_init(void) {
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = PIN_MASK_INPUT;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    gpio_reset_pin(PIN_OUT_RADIO);
    gpio_set_direction(PIN_OUT_RADIO, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_OUT_RADIO, 0);
}

// ============================================================
// I2C INIT
// ============================================================
static void i2c_init(void) {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = I2C_SDA_PIN;
    bus_cfg.scl_io_num = I2C_SCL_PIN;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = 1;
    if (i2c_new_master_bus(&bus_cfg, &bus_handle) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed, display disabled");
        bus_handle = NULL;
        return;
    }
    ESP_LOGI(TAG, "I2C bus ready");
}

// ============================================================
// SELFTEST ALL'AVVIO
// ============================================================
static void startup_test(void) {
    ESP_LOGI(TAG, "=== STARTUP SELFTEST ===");

    // Legge e stampa lo stato dei pin
    int dp = gpio_get_level(PIN_DOT);
    int dsh = gpio_get_level(PIN_DASH);
    ESP_LOGI(TAG, "Paddle: DOT=%d DASH=%d (0=PRESSED)", dp, dsh);

    // Test radio output (pin 7) - lampeggio
    ESP_LOGI(TAG, "Test PIN 7 (OUT_RADIO): ON 500ms");
    gpio_set_level(PIN_OUT_RADIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(PIN_OUT_RADIO, 0);
    ESP_LOGI(TAG, "Test PIN 7: OFF");

    // Test buzzer (pin 15) - 500ms 600Hz
    ESP_LOGI(TAG, "Test PIN 15 (BUZZER): 600Hz 500ms vol=%ld", (long)cfg.volume);
    uint32_t duty = vol_table[cfg.volume];
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    vTaskDelay(pdMS_TO_TICKS(500));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ESP_LOGI(TAG, "Test PIN 15: OFF");

    // Test radio output again (pin 7) - altro lampeggio
    ESP_LOGI(TAG, "Test PIN 7 (OUT_RADIO): ON 200ms");
    gpio_set_level(PIN_OUT_RADIO, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(PIN_OUT_RADIO, 0);
    ESP_LOGI(TAG, "=== SELFTEST END ===");
}

// ============================================================
// SSD1306 OLED DISPLAY (7x14 bold, come .ino originale)
// ============================================================

#define SSD1306_ADDR  0x3C
#define SSD1306_CMD   0x00
#define SSD1306_DATA  0x40

static uint8_t fb[1024];
static bool disp_dirty = false;

static void disp_send(uint8_t *buf, size_t len) {
    if (disp_dev) i2c_master_transmit(disp_dev, buf, len, 50);
}

static void disp_cmd(uint8_t cmd) {
    uint8_t buf[2] = {SSD1306_CMD, cmd};
    disp_send(buf, 2);
}

static bool disp_init(void) {
    if (!bus_handle) return false;
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = SSD1306_ADDR;
    dev_cfg.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(bus_handle, &dev_cfg, &disp_dev) != ESP_OK)
        return false;
    // Probe: prova a inviare un comando
    uint8_t probe[] = {SSD1306_CMD, 0xAE};
    if (i2c_master_transmit(disp_dev, probe, 2, 50) != ESP_OK) {
        disp_dev = NULL;
        return false;
    }
    // Init sequenza completa
    disp_cmd(0xD5); disp_cmd(0xF0);
    disp_cmd(0xA8); disp_cmd(0x3F);
    disp_cmd(0xD3); disp_cmd(0x00);
    disp_cmd(0x40);
    disp_cmd(0x8D); disp_cmd(0x14); vTaskDelay(pdMS_TO_TICKS(10));
    disp_cmd(0x20); disp_cmd(0x00);
    disp_cmd(0xA1); disp_cmd(0xC8);
    disp_cmd(0xDA); disp_cmd(0x12);
    disp_cmd(0x81); disp_cmd(0xCF);
    disp_cmd(0xD9); disp_cmd(0xF1);
    disp_cmd(0xDB); disp_cmd(0x40);
    disp_cmd(0xA4); disp_cmd(0xA6);
    disp_cmd(0xAF);
    ESP_LOGI(TAG, "SSD1306 initialized");
    return true;
}



static void disp_sleep(void) {
    if (!disp_dev || disp_sleeping) return;
    disp_cmd(0xAE);
    disp_sleeping = true;
    ESP_LOGI(TAG, "Display sleep");
}

static void disp_wake(void) {
    if (!disp_dev || !disp_sleeping) return;
    disp_cmd(0xAF);
    disp_sleeping = false;
    ESP_LOGI(TAG, "Display wake");
}

static void disp_clear(void) {
    memset(fb, 0, sizeof(fb));
    disp_dirty = true;
}

static void disp_flush(void) {
    disp_cmd(0x21); disp_cmd(0); disp_cmd(127);
    disp_cmd(0x22); disp_cmd(0); disp_cmd(7);
    // Unica transazione I2C per tutto il framebuffer (1024 + 1 byte controllo)
    static uint8_t txb[1025];
    txb[0] = SSD1306_DATA;
    memcpy(txb + 1, fb, 1024);
    disp_send(txb, 1025);
    disp_dirty = false;
}

// 7x14 bold font: 14 byte per char, bit 6 = col0 (sinistra)
// Indice: (ch - 0x20) * 14
static const uint8_t font_7x14_bold[] = {
    // 0x20 ' '
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x21 '!'  bold
    0x00,0x1C,0x1C,0x1C,0x1C,0x1C,0x1C,0x1C,0x1C,0x00,0x1C,0x1C,0x00,0x00,
    // 0x22 '"' (non usato)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x23 '#' (non usato)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x24 '$' (non usato)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x25 '%' (non usato)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x26 '&' (non usato)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x27 ''' (non usato)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x28 '(' (non usato)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x29 ')' (non usato)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x2A '*' (non usato)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x2B '+' (non usato)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x2C ',' (non usato)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x2D '-'
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x2E '.' (non usato ma definito)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1C,0x1C,0x00,0x00,
    // 0x2F '/'
    0x00,0x02,0x06,0x04,0x0C,0x08,0x18,0x10,0x30,0x20,0x60,0x40,0x00,0x00,
    // 0x30 '0'
    0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,
    // 0x31 '1'
    0x00,0x18,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,
    // 0x32 '2'
    0x00,0x3C,0x66,0x06,0x06,0x0C,0x18,0x30,0x60,0x60,0x60,0x7E,0x00,0x00,
    // 0x33 '3'
    0x00,0x3C,0x66,0x06,0x06,0x1C,0x06,0x06,0x06,0x06,0x66,0x3C,0x00,0x00,
    // 0x34 '4'
    0x00,0x0C,0x1C,0x3C,0x2C,0x4C,0x4C,0x7E,0x0C,0x0C,0x0C,0x0C,0x00,0x00,
    // 0x35 '5'
    0x00,0x7E,0x60,0x60,0x60,0x7C,0x06,0x06,0x06,0x06,0x66,0x3C,0x00,0x00,
    // 0x36 '6'
    0x00,0x3C,0x66,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,
    // 0x37 '7'
    0x00,0x7E,0x06,0x06,0x0C,0x0C,0x18,0x18,0x30,0x30,0x30,0x30,0x00,0x00,
    // 0x38 '8'
    0x00,0x3C,0x66,0x66,0x66,0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,
    // 0x39 '9'
    0x00,0x3C,0x66,0x66,0x66,0x66,0x3E,0x06,0x06,0x06,0x66,0x3C,0x00,0x00,
    // 0x3A ':'
    0x00,0x00,0x00,0x00,0x1C,0x1C,0x00,0x00,0x00,0x1C,0x1C,0x00,0x00,0x00,
    // 0x3B ';' (non usato)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x3C '<'
    0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,
    // 0x3D '=' (non usato)
    0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,
    // 0x3E '>'
    0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,
    // 0x3F '?' (non usato)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x40 '@' (non usato)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x41 'A'
    0x00,0x18,0x3C,0x3C,0x66,0x66,0x66,0x7E,0x7E,0xC3,0xC3,0xC3,0x00,0x00,
    // 0x42 'B'
    0x00,0xFC,0xC6,0xC6,0xC6,0xFC,0xC6,0xC6,0xC6,0xC6,0xC6,0xFC,0x00,0x00,
    // 0x43 'C'
    0x00,0x3C,0x66,0xC6,0xC0,0xC0,0xC0,0xC0,0xC0,0xC6,0x66,0x3C,0x00,0x00,
    // 0x44 'D'
    0x00,0xFC,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xFC,0x00,0x00,
    // 0x45 'E'
    0x00,0x7E,0x60,0x60,0x60,0x7C,0x60,0x60,0x60,0x60,0x60,0x7E,0x00,0x00,
    // 0x46 'F'
    0x00,0x7E,0x60,0x60,0x60,0x7C,0x60,0x60,0x60,0x60,0x60,0x60,0x00,0x00,
    // 0x47 'G'
    0x00,0x3C,0x66,0xC6,0xC0,0xC0,0xCE,0xC6,0xC6,0xC6,0x66,0x3E,0x00,0x00,
    // 0x48 'H'
    0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,
    // 0x49 'I'
    0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,
    // 0x4A 'J'
    0x00,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0xC6,0x6C,0x38,0x00,0x00,
    // 0x4B 'K'
    0x00,0xC6,0xCC,0xD8,0xF0,0xE0,0xF0,0xD8,0xCC,0xC6,0xC6,0xC6,0x00,0x00,
    // 0x4C 'L'
    0x00,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00,0x00,
    // 0x4D 'M'
    0x00,0xE6,0xE6,0xFF,0xFF,0xDB,0xDB,0xDB,0xC3,0xC3,0xC3,0xC3,0x00,0x00,
    // 0x4E 'N'
    0x00,0xC6,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,
    // 0x4F 'O'
    0x00,0x3C,0x66,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0x00,0x00,
    // 0x50 'P'
    0x00,0xFC,0xC6,0xC6,0xC6,0xC6,0xFC,0xC0,0xC0,0xC0,0xC0,0xC0,0x00,0x00,
    // 0x51 'Q' (non usato)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x52 'R'
    0x00,0xFC,0xC6,0xC6,0xC6,0xC6,0xFC,0xD8,0xCC,0xC6,0xC6,0xC6,0x00,0x00,
    // 0x53 'S'
    0x00,0x3C,0x66,0xC0,0xC0,0x60,0x38,0x0C,0x06,0x06,0xC6,0x7C,0x00,0x00,
    // 0x54 'T'
    0x00,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,
    // 0x55 'U'
    0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,
    // 0x56 'V'
    0x00,0xC3,0xC3,0xC3,0x66,0x66,0x66,0x3C,0x3C,0x3C,0x18,0x18,0x00,0x00,
    // 0x57 'W'
    0x00,0xC3,0xC3,0xC3,0xC3,0xDB,0xDB,0xDB,0xFF,0xFF,0x66,0x66,0x00,0x00,
    // 0x58 'X'
    0x00,0xC3,0xC3,0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0xC3,0xC3,0x00,0x00,
    // 0x59 'Y'
    0x00,0xC3,0xC3,0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,
    // 0x5A 'Z'
    0x00,0x7E,0x06,0x06,0x0C,0x18,0x18,0x30,0x60,0x60,0x60,0x7E,0x00,0x00,
    // 0x5B '['
    0x00,0x3E,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3E,0x00,0x00,
    // 0x5C '\' (non usato ma per allineamento)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x5D ']'
    0x00,0x7C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x7C,0x00,0x00,
};

#define FONT_STRIDE 14
#define FONT_BASE(c) ((c) >= 0x20 && (c) <= 0x5D ? &font_7x14_bold[((c) - 0x20) * FONT_STRIDE] : &font_7x14_bold[0])

static void disp_draw_char(int px, int py, char ch) {
    const uint8_t *g = FONT_BASE((uint8_t)ch);
    if (px < 0 || px > 128 - 7 || py < 0 || py > 64 - 14) return;
    int page = py / 8;
    int shift = py % 8;
    for (int col = 0; col < 7; col++) {
        // Costruisce colonna verticale di 14 bit dal glifo
        uint16_t col_data = 0;
        for (int row = 0; row < 14; row++) {
            if (g[row] & (0x40 >> col)) col_data |= (1 << row);
        }
        int x = px + col;
        // pagina alta (rows 0-7)
        fb[x + page * 128] |= (col_data << shift) & 0xFF;
        if (shift) {
            // pagina media/bassa - overflow bit da shift
            fb[x + (page + 1) * 128] |= (col_data >> (8 - shift)) & 0xFF;
        } else {
            fb[x + (page + 1) * 128] |= (col_data >> 8) & 0xFF;
        }
    }
}

static void disp_draw_str(int px, int py, const char *str) {
    while (*str) {
        disp_draw_char(px, py, *str);
        px += 8; // 7px char + 1px gap
        str++;
    }
}

static void disp_redraw(void) {
    if (disp_sleeping) return;
    disp_clear();
    char buf[24];
    if (disp_saved_timer > 0) {
        disp_draw_str(36, 20, "SAVED!");
        disp_flush();
        return;
    }
    const char *modes[] = {"STRAIGHT", "IAMBIC A", "IAMBIC B", "BUG-SIM"};
    if (disp_show_ip && sta_got_ip && sta_ip_str[0]) {
        // 5 righe compatte: mostra IP all'avvio
        snprintf(buf, sizeof(buf), "MOD:%s", modes[cfg.mode >= 0 && cfg.mode <= 3 ? cfg.mode : 0]);
        disp_draw_str(0, 0, buf);
        if (cfg.role == 1) {
            const char *t = cfg.role == 2 ? (ws_client_connected ? "WS:OK" : "WS:--") : (tcp_state == 3 ? "WS:OK" : (tcp_state >= 1 ? "WS:.." : "WS:--"));
            snprintf(buf, sizeof(buf), "%s %s", cfg.bypass ? "PRACTICE" : "ONLINE", t);
        } else {
            snprintf(buf, sizeof(buf), "%s", cfg.bypass ? "TX:PRACTICE" : "TX:ONLINE");
        }
        disp_draw_str(0, 12, buf);
        snprintf(buf, sizeof(buf), "SPD:%ldWPM", (long)cfg.wpm);
        disp_draw_str(0, 24, buf);
        if (cfg.volume == 0) snprintf(buf, sizeof(buf), "VOL:OFF");
        else                snprintf(buf, sizeof(buf), "VOL:%ld/10", (long)cfg.volume);
        disp_draw_str(0, 36, buf);
        snprintf(buf, sizeof(buf), "IP:%-15s", sta_ip_str);
        disp_draw_str(0, 48, buf);
    } else {
        // 4 righe classiche (16px spaziatura)
        snprintf(buf, sizeof(buf), "MOD: %s", modes[cfg.mode >= 0 && cfg.mode <= 3 ? cfg.mode : 0]);
        disp_draw_str(0, 0, buf);
        if (sta_got_ip && sta_ip_str[0])
            snprintf(buf, sizeof(buf), "STA: %s", sta_ip_str);
        else if (sta_connected)
            snprintf(buf, sizeof(buf), "STA: waiting IP...");
        else if (sta_cred_count > 0)
            snprintf(buf, sizeof(buf), "STA: connecting %s", sta_creds[sta_ap_index].ssid);
        else
            snprintf(buf, sizeof(buf), "STA: no WiFi cfg");
        disp_draw_str(0, 16, buf);
        snprintf(buf, sizeof(buf), "SPEED: %ld WPM", (long)cfg.wpm);
        if (cfg.role == 1) {
            const char *t = cfg.role == 2 ? (ws_client_connected ? "WS:OK" : "WS:--") : (tcp_state == 3 ? "WS:OK" : (tcp_state >= 1 ? "WS:.." : "WS:--"));
            snprintf(buf, sizeof(buf), "SPD:%ld %s", (long)cfg.wpm, t);
        } else {
            snprintf(buf, sizeof(buf), "SPEED: %ld WPM", (long)cfg.wpm);
        }
        disp_draw_str(0, 32, buf);
        if (cfg.volume == 0) snprintf(buf, sizeof(buf), "VOL: OFF");
        else                snprintf(buf, sizeof(buf), "VOL: %ld/10", (long)cfg.volume);
        disp_draw_str(0, 48, buf);
    }
    disp_flush();
}

// ============================================================
// WEB PAGE HTML
// ============================================================
static const char *web_html = R"rawliteral(
<!DOCTYPE html>
<html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>C6 Keyer</title>
<style>
body{background:#121212;color:#eee;font-family:sans-serif;text-align:center;margin:0;padding:10px}
h3{color:#00adb5}
.card{background:#1e1e1e;border-radius:12px;padding:15px;margin:10px 0}
.row{display:flex;justify-content:space-between;align-items:center;margin:8px 0}
label{font-size:14px;color:#aaa}
.val{font-size:18px;font-weight:bold;color:#fff}
input,select{width:100%;padding:10px;border:none;border-radius:8px;font-size:16px;background:#333;color:#fff;margin:5px 0;box-sizing:border-box}
#info{font-size:13px;color:#4caf50;margin:5px 0;min-height:18px}
</style></head><body>
<h3>C6 ESP32-C6 KEYER</h3>
<div class="card">
<div class="row"><label>WPM</label><span class="val" id="v_wpm">...</span></div>
<input type="range" name="w" min="5" max="70" value="20" oninput="v_wpm.textContent=this.value;autoSave()">
<div class="row"><label>Volume</label><span class="val" id="v_vol">...</span></div>
<input type="range" name="l" min="0" max="10" value="5" oninput="v_vol.textContent=this.value;autoSave()">
<div class="row"><label>Mode</label><span class="val" id="v_mode">...</span></div>
<select name="m" onchange="v_mode.textContent=this.options[this.selectedIndex].text;autoSave()">
<option value="0">STRAIGHT</option>
<option value="1">IAMBIC A</option>
<option value="2">IAMBIC B</option>
<option value="3">BUG</option>
</select>
<div class="row"><label>Bypass</label><span class="val" id="v_byp">...</span></div>
<select name="b" onchange="v_byp.textContent=this.value==1?'ON (practice)':'OFF (TX active)';autoSave()">
<option value="0">OFF (TX active)</option>
<option value="1">ON (practice)</option>
</select>
<div class="row"><label>Role</label><span class="val" id="v_role">...</span></div>
<select name="r" onchange="v_role.textContent=['Standalone','TX','RX'][this.value];autoSave()">
<option value="0">Standalone</option>
<option value="1">TX</option>
<option value="2">RX</option>
</select>
<div id="jblat_section">
<div class="row"><label>RX Latency</label><span class="val" id="v_jbl">...</span></div>
<input type="range" name="j" min="50" max="1000" value="200" oninput="v_jbl.textContent=this.value+'ms';autoSave()">
</div>
<div class="row"><label>STA IP (WiFi)</label><span class="val" id="v_staip">not connected</span></div>
<div class="row"><label>TCP Link</label><span class="val" id="v_tcp" style="font-size:14px">...</span></div>
<button id="reconnect_btn" onclick="reconnect()" style="width:100%;padding:8px;border:none;border-radius:8px;font-size:14px;background:#00adb5;color:#fff;margin:4px 0;cursor:pointer">⟳ Reconnect</button>
</div>
<div id="info"></div>
<div id="wifi_status" style="font-size:13px;color:#aaa;margin:4px 0"></div>
<p><a href="/wifi" style="color:#00adb5;font-size:13px">⚙ WiFi Network Settings →</a></p>
<script>
var saveTimer=0;
function autoSave(){
clearTimeout(saveTimer);
saveTimer=setTimeout(function(){
var q='w='+document.querySelector('[name="w"]').value;
q+='&l='+document.querySelector('[name="l"]').value;
q+='&m='+document.querySelector('[name="m"]').value;
q+='&b='+document.querySelector('[name="b"]').value;
	q+='&r='+document.querySelector('[name="r"]').value;
	q+='&j='+document.querySelector('[name="j"]').value;
fetch('/save?'+q).then(function(r){return r.text()}).then(function(){
document.getElementById('info').textContent='Saved at '+new Date().toLocaleTimeString();
});
},300);
}
function updateUI(d){
document.querySelector('[name="w"]').value=d.w; v_wpm.textContent=d.w;
document.querySelector('[name="l"]').value=d.l; v_vol.textContent=d.l;
document.querySelector('[name="m"]').value=d.m; v_mode.textContent=['STRAIGHT','IAMBIC A','IAMBIC B','BUG'][d.m];
document.querySelector('[name="b"]').value=d.b; v_byp.textContent=d.b?'ON (practice)':'OFF (TX active)';
document.querySelector('[name="r"]').value=d.r; v_role.textContent=['Standalone','TX','RX'][d.r];
document.getElementById('jblat_section').style.display=(d.r==2)?'':'none';
if(document.querySelector('[name="j"]')){document.querySelector('[name="j"]').value=d.jblat;v_jbl.textContent=d.jblat+'ms';}
var sts=document.getElementById('wifi_status');
if(d.stai) sts.innerHTML='Connected to <b>'+d.stai+'</b> ('+d.staip+')';
else sts.innerHTML='Not connected to any WiFi';
var tcpLabels=['Disconnected','Connecting...','Upgrading...','Connected'];
var tcpColors=['#ff4444','#ffaa00','#ffaa00','#4caf50'];
var tcpEl=document.getElementById('v_tcp');
if(tcpEl){tcpEl.textContent=tcpLabels[d.tcp]||'?';tcpEl.style.color=tcpColors[d.tcp]||'#fff';}
var btn=document.getElementById('reconnect_btn');
if(btn)btn.style.display=(d.r==1)?'':'none';
}
function reconnect(){
var btn=document.getElementById('reconnect_btn');
if(btn){btn.textContent='⟳ Reconnecting...';btn.disabled=true;}
fetch('/reconnect?'+Date.now()).then(function(){setTimeout(function(){if(btn){btn.textContent='⟳ Reconnect';btn.disabled=false;}},1000);});
}
fetch('/status?'+Date.now()).then(r=>r.json()).then(function(d){
updateUI(d); document.getElementById('info').textContent='Ready';
});
setInterval(function(){
fetch('/status?'+Date.now()).then(r=>r.json()).then(updateUI);
},2000);
</script>
</body></html>)rawliteral";

// ============================================================
// HTTP HANDLERS
// ============================================================
static esp_err_t web_root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_send(req, web_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t web_status_handler(httpd_req_t *req) {
    char buf[1536];
    // Campi STA connessione
    char sta_json[256];
    if (sta_got_ip)
        snprintf(sta_json, sizeof(sta_json), "\"stai\":\"%s\",\"staip\":\"%s\"",
                 sta_ssid_connected, sta_ip_str);
    else
        snprintf(sta_json, sizeof(sta_json), "\"stai\":\"\",\"staip\":\"\"");
    // Aggiunge SSID salvati (password omesse per sicurezza)
    char ssid_json[256];
    int pos = 0;
    for (int i = 0; i < MAX_STA_NETS && pos < (int)sizeof(ssid_json) - 48; i++)
        pos += snprintf(ssid_json + pos, sizeof(ssid_json) - pos,
                        ",\"s%d\":\"%s\"", i, sta_creds[i].ssid);
    // Event log (ultimi EVT_LOG_SIZE eventi suonati)
    char evt_str[512] = "";
    int epos = 0;
    int ecnt = evt_log_pos;
    int estart = ecnt > EVT_LOG_SIZE ? ecnt - EVT_LOG_SIZE : 0;
    int64_t base = 0;
    for (int i = estart; i < ecnt && epos < (int)sizeof(evt_str) - 12; i++) {
        evt_entry_t *e = &evt_log[i % EVT_LOG_SIZE];
        if (base == 0) base = e->abs_us;
        int ms = (int)((e->abs_us - base) / 1000);
        epos += snprintf(evt_str + epos, sizeof(evt_str) - epos,
                         "%c%d@%d ", e->type ? '+' : '-', e->seq, ms);
    }
    int len = snprintf(buf, sizeof(buf),
        "{\"w\":%ld,\"l\":%ld,\"m\":%ld,\"b\":%ld,\"r\":%ld,\"i\":\"%s\",\"p\":%ld,\"jblat\":%ld,"
        "\"tcp\":%d,"
        "\"txtot\":%lu,\"txok\":%lu,\"txfail\":%lu,\"rxtot\":%lu,"
        "\"jbin\":%lu,\"jbout\":%lu,\"jbdrop\":%lu,"
        "\"log\":\"%s\","
        "%s%s}",
        (long)cfg.wpm, (long)cfg.volume, (long)cfg.mode,
        (long)cfg.bypass, (long)cfg.role,
        (const char*)cfg.remote_ip, (long)cfg.remote_port, (long)cfg.jb_latency,
        (int)(cfg.role == 2 ? (ws_client_connected ? 3 : 0) : tcp_state),
        (unsigned long)dbg_tx_total,
        (unsigned long)dbg_tx_sendto_ok, (unsigned long)dbg_tx_sendto_fail,
        (unsigned long)dbg_rx_total,
        (unsigned long)dbg_jb_in, (unsigned long)dbg_jb_out, (unsigned long)dbg_jb_drop,
        evt_str,
        sta_json, ssid_json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t web_reconnect_handler(httpd_req_t *req) {
    if (cfg.role == 1 && udp_tx_sock >= 0) {
        close(udp_tx_sock);
        udp_tx_sock = -1;
    }
    tcp_retry_us = 0; // resetta timer per reconnect immediato
    tcp_state = 0;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

// URL-decode in-place (gestisce %XX e +)
static void url_decode(char *s) {
    char *r = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            char hex[3] = {s[1], s[2], 0};
            *r++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else if (*s == '+') {
            *r++ = ' ';
            s++;
        } else {
            *r++ = *s++;
        }
    }
    *r = 0;
}

static esp_err_t web_save_handler(httpd_req_t *req) {
    char qbuf[512];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char tmp[64], pwd[64];
        int32_t old_role = cfg.role;

        if (httpd_query_key_value(qbuf, "w", tmp, sizeof(tmp)) == ESP_OK) {
            int v = atoi(tmp);
            if (v >= 5 && v <= 70) cfg.wpm = v;
        }
        if (httpd_query_key_value(qbuf, "l", tmp, sizeof(tmp)) == ESP_OK) {
            int v = atoi(tmp);
            if (v >= 0 && v <= 10) cfg.volume = v;
        }
        if (httpd_query_key_value(qbuf, "m", tmp, sizeof(tmp)) == ESP_OK) {
            int v = atoi(tmp);
            if (v >= 0 && v <= 3) cfg.mode = v;
        }
        if (httpd_query_key_value(qbuf, "b", tmp, sizeof(tmp)) == ESP_OK) {
            cfg.bypass = (atoi(tmp) != 0);
        }
        if (httpd_query_key_value(qbuf, "r", tmp, sizeof(tmp)) == ESP_OK) {
            int v = atoi(tmp);
            if (v >= 0 && v <= 2) cfg.role = v;
        }
        if (httpd_query_key_value(qbuf, "i", tmp, sizeof(tmp)) == ESP_OK) {
            strcpy((char*)cfg.remote_ip, tmp);
        }
        if (httpd_query_key_value(qbuf, "p", tmp, sizeof(tmp)) == ESP_OK) {
            int v = atoi(tmp);
            if (v >= 1 && v <= 65535) cfg.remote_port = v;
        }
        if (httpd_query_key_value(qbuf, "j", tmp, sizeof(tmp)) == ESP_OK) {
            int v = atoi(tmp);
            if (v >= 50 && v <= 1000) cfg.jb_latency = v;
        }

        // Legge credenziali WiFi STA (merge: sovrascrive solo gli slot inviati)
        wifi_cred_t merged_creds[MAX_STA_NETS];
        memcpy(merged_creds, sta_creds, sizeof(merged_creds));
        int merged_cnt = sta_cred_count;
        bool wifi_changed = false;
        for (int i = 0; i < MAX_STA_NETS; i++) {
            char key[8];
            snprintf(key, sizeof(key), "s%d", i);
            if (httpd_query_key_value(qbuf, key, tmp, sizeof(tmp)) == ESP_OK && strlen(tmp) > 0) {
                url_decode(tmp);
                bool ssid_changed = (strcmp(merged_creds[i].ssid, tmp) != 0);
                if (ssid_changed) wifi_changed = true;
                strcpy(merged_creds[i].ssid, tmp);
                snprintf(key, sizeof(key), "p%d", i);
                if (httpd_query_key_value(qbuf, key, pwd, sizeof(pwd)) == ESP_OK) {
                    url_decode(pwd);
                    // Password esplicitamente inviata (anche vuota)
                    if (strlen(pwd) > 0 || ssid_changed) {
                        if (strcmp(merged_creds[i].password, pwd) != 0) wifi_changed = true;
                        strcpy(merged_creds[i].password, pwd);
                    }
                    // Se password vuota e SSID invariato -> mantieni vecchia password
                } else if (ssid_changed) {
                    // SSID cambiato, password non inviata -> rete aperta
                    if (strlen(merged_creds[i].password) > 0) wifi_changed = true;
                    merged_creds[i].password[0] = 0;
                }
                if (i >= merged_cnt) merged_cnt = i + 1;
            }
        }
        if (wifi_changed) {
            // Doppio confronto: verifica che sia effettivamente cambiato
            bool diff = (merged_cnt != sta_cred_count);
            for (int i = 0; i < merged_cnt && !diff; i++)
                if (strcmp(merged_creds[i].ssid, sta_creds[i].ssid) != 0 ||
                    strcmp(merged_creds[i].password, sta_creds[i].password) != 0)
                    diff = true;
            if (diff) {
                memcpy(sta_creds, merged_creds, sizeof(merged_creds));
                sta_cred_count = merged_cnt;
            } else {
                wifi_changed = false;
            }
        }

        dot_length_us = (1200000 / cfg.wpm);
        settings_save();

        if (cfg.role != old_role) {
            ESP_LOGI(TAG, "Role changed, rebooting...");
            httpd_resp_set_type(req, "text/html");
            httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
            httpd_resp_sendstr(req,
                "<html><body style='background:#121212;color:#eee;text-align:center;padding:50px'>"
                "<h2>Role changed! Rebooting...</h2>"
                "<p>The device will restart to apply the new role.</p></body></html>");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }

        if (wifi_changed) {
            ESP_LOGI(TAG, "WiFi credentials changed, rebooting...");
            httpd_resp_set_type(req, "text/html");
            httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
            httpd_resp_sendstr(req,
                "<html><body style='background:#121212;color:#eee;text-align:center;padding:50px'>"
                "<h2>WiFi saved! Rebooting...</h2>"
                "<p>The device will restart to connect to the selected network.</p></body></html>");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }

        ESP_LOGI(TAG, "Web config saved OK");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
        httpd_resp_sendstr(req, "OK");
        return ESP_OK;
    }
    httpd_resp_sendstr(req, "ERROR: no data");
    return ESP_OK;
}

// ============================================================
// WIFI PAGE HTML
// ============================================================
static const char *wifi_html = R"rawliteral(
<!DOCTYPE html>
<html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>C6 Keyer - WiFi</title>
<style>
body{background:#121212;color:#eee;font-family:sans-serif;text-align:center;margin:0;padding:10px}
h3{color:#00adb5}.card{background:#1e1e1e;border-radius:12px;padding:15px;margin:10px 0}
.row{display:flex;justify-content:space-between;align-items:center;margin:6px 0}
label{font-size:14px;color:#aaa}.val{font-size:16px;font-weight:bold;color:#fff}
select,input{width:100%;padding:10px;border:none;border-radius:8px;font-size:14px;background:#333;color:#fff;margin:5px 0;box-sizing:border-box}
button{background:#00adb5;color:#fff;border:none;padding:10px 20px;border-radius:8px;font-size:16px;margin:6px;cursor:pointer}
#info{font-size:13px;color:#4caf50;min-height:18px}
</style></head><body>
<h3>C6 KEYER — WiFi Setup</h3>
<div class="card">
<button onclick="scan()">📡 Scan Networks</button>
<select id="ssid_sel" size="6" style="display:none;width:100%" onchange="pickSSID()"></select>
<div id="scan_status" style="font-size:13px;color:#aaa;margin:4px 0">Click Scan to see available networks</div>
<div class="row"><label>Selected SSID</label><span class="val" id="v_sel">-</span></div>
<input type="text" id="manual_ssid" placeholder="Or type SSID manually" oninput="manualSSID()">
<div class="row"><label>Password</label><span class="val" id="v_auth"></span></div>
<input type="password" id="pwd" placeholder="Leave empty for open networks">
<div id="portal_note" style="font-size:12px;color:#ff9800;display:none">⚠ Open network with captive portal? Connect your phone first, accept terms, then come back and save.</div>
<button onclick="saveWiFi()" style="background:#4caf50">💾 Save WiFi</button>
</div>
<div class="card">
<h4 style="margin:0 0 8px 0;color:#00adb5">Remote Keying</h4>
<div class="row"><label>Remote IP</label><span class="val" id="v_ip">...</span></div>
<input type="text" id="remote_ip" placeholder="192.168.1.100">
<div class="row"><label>Remote Port</label><span class="val" id="v_port">...</span></div>
<input type="number" id="remote_port" min="1" max="65535" value="7373">
<button onclick="saveRemote()" style="background:#4caf50">💾 Save Remote</button>
</div>
<div class="card">
<h4 style="margin:0 0 8px 0;color:#00adb5">Saved Networks</h4>
<div id="saved_list"><div class="row" style="color:#888;font-size:13px">No networks saved</div></div>
</div>
<div id="info"></div>
<p><a href="/" style="color:#00adb5;font-size:13px">← Back to Keyer controls</a></p>
<script>
var ap_list=[];
function scan(){
document.getElementById('scan_status').textContent='Scanning...';
document.getElementById('ssid_sel').style.display='none';
fetch('/scan?'+Date.now()).then(r=>r.json()).then(function(list){
ap_list=list;
var sel=document.getElementById('ssid_sel');
sel.innerHTML='';
list.sort(function(a,b){return b.r-a.r});
list.forEach(function(ap,i){
var opt=document.createElement('option');
opt.value=i;
var auth=['OPEN','WEP','WPA','WPA2','WPA1/2','ENT','WPA3','WPA2/3'][ap.o]||('?'+ap.o);
opt.textContent=ap.s+'  ['+auth+']  '+ap.r+'dBm';
sel.appendChild(opt);
});
sel.style.display='';
document.getElementById('scan_status').textContent=list.length+' networks found';
});
}
function pickSSID(){
var sel=document.getElementById('ssid_sel');
var idx=parseInt(sel.value);
if(isNaN(idx)||!ap_list[idx]) return;
var ap=ap_list[idx];
document.getElementById('manual_ssid').value=ap.s;
document.getElementById('v_sel').textContent=ap.s;
var authNames=['Open','WEP','WPA','WPA2','WPA1/2','Enterprise','WPA3','WPA2/3'];
document.getElementById('v_auth').textContent='('+authNames[ap.o]||'?'+ap.o+')';
document.getElementById('portal_note').style.display=(ap.o===0&&!document.getElementById('pwd').value)?'block':'none';
}
function manualSSID(){
document.getElementById('v_sel').textContent=this.value||'-';
document.getElementById('ssid_sel').value='';
}
document.getElementById('pwd').oninput=function(){
document.getElementById('portal_note').style.display=(this.value==''&&document.getElementById('v_sel').textContent!='-')?'block':'none';
};
function saveWiFi(){
var ssid=document.getElementById('manual_ssid').value.trim();
if(!ssid){alert('Enter or select an SSID');return;}
var pwd=document.getElementById('pwd').value;
fetch('/status?'+Date.now()).then(r=>r.json()).then(function(d){
var slot=-1;
for(var i=0;i<4;i++){
if(d['s'+i]==ssid){slot=i;break;}
if(!d['s'+i]&&slot<0) slot=i;
}
if(slot<0) slot=0;
var q='s'+slot+'='+encodeURIComponent(ssid)+'&p'+slot+'='+encodeURIComponent(pwd);
for(var i=0;i<4;i++){
if(i!=slot && d['s'+i]) q+='&s'+i+'='+encodeURIComponent(d['s'+i]);
}
document.getElementById('info').textContent='Saving and rebooting...';
fetch('/save?'+q).then(r=>r.text()).then(function(){});
});
}
function saveRemote(){
var ip=document.getElementById('remote_ip').value.trim();
var pt=document.getElementById('remote_port').value;
if(!ip){alert('Enter Remote IP');return;}
fetch('/save?i='+encodeURIComponent(ip)+'&p='+pt).then(r=>r.text()).then(function(m){
document.getElementById('info').textContent='Remote settings saved';
});
}
function selectNet(idx){
fetch('/selwifi?idx='+idx).then(function(r){return r.text()}).then(function(){
document.getElementById('info').textContent='Connecting to slot '+idx+'...';
});
}
function delNet(idx){
if(!confirm('Delete this network?')) return;
fetch('/delwifi?idx='+idx).then(function(r){return r.text()}).then(function(){
document.getElementById('info').textContent='Network deleted';
});
}
setInterval(function(){
fetch('/status?'+Date.now()).then(r=>r.json()).then(function(d){
var sl=document.getElementById('saved_list');
var html='';
for(var i=0;i<4;i++){
var key='s'+i;
if(d[key]&&d[key]!=''){
html+='<div class="row" style="cursor:pointer" onclick="selectNet('+i+')"><label>'+(i+1)+'.</label><span class="val">'+d[key]+'</span><button onclick="event.stopPropagation();delNet('+i+')" style="background:#c0392b;padding:2px 8px;font-size:12px">✕</button></div>';
}
}
sl.innerHTML=html||'<div class="row" style="color:#888;font-size:13px">No networks saved</div>';
if(d.stai) document.getElementById('scan_status').textContent='Connected to '+d.stai+' ('+d.staip+')';
if(document.getElementById('v_ip')) document.getElementById('v_ip').textContent=d.i||'-';
if(document.getElementById('v_port')) document.getElementById('v_port').textContent=d.p||'7373';
});
},3000);
// Pre-fill Remote IP/Port al caricamento (non in poll per non interferire con la digitazione)
fetch('/status?'+Date.now()).then(r=>r.json()).then(function(d){
if(document.getElementById('remote_ip')) document.getElementById('remote_ip').value=d.i||'';
if(document.getElementById('remote_port')) document.getElementById('remote_port').value=d.p||'7373';
});
</script>
</body></html>)rawliteral";

// ============================================================
// SCAN HANDLER
// ============================================================
static esp_err_t web_scan_handler(httpd_req_t *req) {
    wifi_scan_config_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 100;
    sc.scan_time.active.max = 300;
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    uint16_t cnt = 0;
    esp_wifi_scan_get_ap_num(&cnt);
    if (cnt > 30) cnt = 30;
    wifi_ap_record_t *aps = (wifi_ap_record_t*)malloc(cnt * sizeof(wifi_ap_record_t));
    if (!aps) { httpd_resp_sendstr(req, "[]"); return ESP_OK; }
    esp_wifi_scan_get_ap_records(&cnt, aps);

    char *json = (char*)malloc(4096);
    if (!json) { free(aps); httpd_resp_sendstr(req, "[]"); return ESP_OK; }
    int pos = 0;
    pos += snprintf(json + pos, 4096 - pos, "[");
    for (int i = 0; i < (int)cnt && pos < 4000; i++) {
        // Escape SSID per JSON
        char esc_ssid[128];
        int ep = 0;
        for (int si = 0; aps[i].ssid[si] && ep < 120; si++) {
            unsigned char c = aps[i].ssid[si];
            if (c == '"' || c == '\\') { if (ep < 126) esc_ssid[ep++] = '\\'; }
            if (c >= 0x20 && c < 0x7F) esc_ssid[ep++] = c;
            else if (c == 0) break;
            else if (ep < 127) esc_ssid[ep++] = '?';
        }
        esc_ssid[ep] = 0;
        pos += snprintf(json + pos, 4096 - pos, "%c{\"s\":\"%s\",\"r\":%d,\"o\":%d}",
                        i > 0 ? ',' : ' ', esc_ssid, aps[i].rssi, aps[i].authmode);
    }
    pos += snprintf(json + pos, 4096 - pos, "]");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, pos);
    free(json);
    free(aps);
    return ESP_OK;
}

static esp_err_t web_wifi_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_send(req, wifi_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Catch-all handler: redirect to "/" per captive portal
static esp_err_t web_catchall_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

// Tap su rete salvata: connetti immediatamente
static void sta_connect_to_index(int idx);
static bool sta_manual_selection = false;
static esp_err_t web_selwifi_handler(httpd_req_t *req) {
    char qbuf[64];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char tmp[8];
        if (httpd_query_key_value(qbuf, "idx", tmp, sizeof(tmp)) == ESP_OK) {
            int idx = atoi(tmp);
            if (idx >= 0 && idx < sta_cred_count) {
                sta_manual_selection = true;
                sta_connect_to_index(idx);
                httpd_resp_set_type(req, "text/plain");
                httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
                httpd_resp_sendstr(req, "OK");
                return ESP_OK;
            }
        }
    }
    httpd_resp_sendstr(req, "ERROR");
    return ESP_OK;
}

// Cancella rete WiFi salvata
static esp_err_t web_delwifi_handler(httpd_req_t *req) {
    char qbuf[64];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char tmp[8];
        if (httpd_query_key_value(qbuf, "idx", tmp, sizeof(tmp)) == ESP_OK) {
            int idx = atoi(tmp);
            if (idx >= 0 && idx < MAX_STA_NETS && strlen(sta_creds[idx].ssid) > 0) {
                bool was_current = sta_connected && strcmp(sta_ssid_connected, sta_creds[idx].ssid) == 0;
                for (int i = idx; i < MAX_STA_NETS - 1; i++) {
                    strcpy(sta_creds[i].ssid, sta_creds[i+1].ssid);
                    strcpy(sta_creds[i].password, sta_creds[i+1].password);
                }
                sta_creds[MAX_STA_NETS-1].ssid[0] = 0;
                sta_creds[MAX_STA_NETS-1].password[0] = 0;
                if (sta_cred_count > 0) sta_cred_count--;
                settings_save();
                if (was_current) {
                    esp_wifi_disconnect();
                }
                httpd_resp_set_type(req, "text/plain");
                httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
                httpd_resp_sendstr(req, "OK");
                return ESP_OK;
            }
        }
    }
    httpd_resp_sendstr(req, "ERROR");
    return ESP_OK;
}

// ============================================================
// DNS SERVER (captive portal: tutte le query → 192.168.4.1)
// ============================================================
static void dns_server_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(NULL);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed on port 53");
        close(sock);
        vTaskDelete(NULL);
    }

    uint8_t buf[512];
    uint8_t reply[512];
    struct sockaddr_in from;
    socklen_t fromlen;

    ESP_LOGI(TAG, "DNS server running on port 53");

    while (1) {
        fromlen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fromlen);
        if (n < 12) continue;

        // Only respond to standard queries (QR=0, OPCODE=0)
        if ((buf[2] & 0x78) != 0) continue;

        uint16_t qdcount = (buf[4] << 8) | buf[5];
        if (qdcount == 0) continue;

        // Build response header
        memcpy(reply, buf, 12);
        reply[2] = 0x81; // response, no error
        reply[3] = 0x80;
        reply[6] = 0;    // answer count = 1
        reply[7] = 1;
        reply[8] = 0;    // authority = 0
        reply[9] = 0;
        reply[10] = 0;   // additional = 0
        reply[11] = 0;

        // Copy question section verbatim
        int qlen = 12;
        while (qlen < n && buf[qlen] != 0) {
            int labellen = buf[qlen];
            if (labellen == 0 || qlen + labellen + 1 > n) break;
            qlen += labellen + 1;
        }
        if (qlen >= n) continue;
        qlen += 1; // trailing 0x00
        if (qlen + 4 > n) continue; // need at least QTYPE+QCLASS
        qlen += 4; // QTYPE + QCLASS

        memcpy(reply + 12, buf + 12, qlen - 12);
        int rlen = qlen;

        // Answer section: pointer to QNAME (offset 12 in the response)
        reply[rlen++] = 0xC0;
        reply[rlen++] = 0x0C;
        reply[rlen++] = 0x00; // TYPE A
        reply[rlen++] = 0x01;
        reply[rlen++] = 0x00; // CLASS IN
        reply[rlen++] = 0x01;
        reply[rlen++] = 0x00; // TTL 60s
        reply[rlen++] = 0x00;
        reply[rlen++] = 0x00;
        reply[rlen++] = 0x3C;
        reply[rlen++] = 0x00; // RDLENGTH = 4
        reply[rlen++] = 0x04;
        reply[rlen++] = 192;  // 192.168.4.1
        reply[rlen++] = 168;
        reply[rlen++] = 4;
        reply[rlen++] = 1;

        sendto(sock, reply, rlen, 0, (struct sockaddr*)&from, sizeof(from));
    }
}

// ============================================================
// UDP BEACON (annuncia IP keyer sulla LAN ogni 5 secondi)
// ============================================================
static void beacon_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "beacon socket failed"); vTaskDelete(NULL); return; }

    struct sockaddr_in bc_addr;
    memset(&bc_addr, 0, sizeof(bc_addr));
    bc_addr.sin_family = AF_INET;
    bc_addr.sin_port = htons(8888);
    bc_addr.sin_addr.s_addr = INADDR_BROADCAST;

    int broadcastEnable = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcastEnable, sizeof(broadcastEnable));

    const char *modes[] = {"ST","IA","IB","BG"};
    char msg[128];

    while (1) {
        msg[0] = 0;
        int pos = snprintf(msg, sizeof(msg), "C6_KEYER;name=esp32-keyer;staip=%s;role=%ld;mode=%s",
                           sta_got_ip ? sta_ip_str : "0.0.0.0",
                           (long)cfg.role, modes[cfg.mode >= 0 && cfg.mode <= 3 ? cfg.mode : 0]);
        (void)pos;
        sendto(sock, msg, strlen(msg), 0, (struct sockaddr*)&bc_addr, sizeof(bc_addr));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ============================================================
// WIFI AP+STA INIT
// ============================================================
static esp_netif_t *ap_netif = NULL;
static esp_netif_t *sta_netif = NULL;
int sta_ap_index = 0;

static void sta_connect_to_index(int idx) {
    sta_ap_index = idx;
    wifi_config_t sta_cfg;
    memset(&sta_cfg, 0, sizeof(sta_cfg));
    strcpy((char*)sta_cfg.sta.ssid, sta_creds[idx].ssid);
    strcpy((char*)sta_cfg.sta.password, sta_creds[idx].password);
    sta_cfg.sta.scan_method = WIFI_FAST_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_cfg.sta.threshold.rssi = -127;
    sta_cfg.sta.failure_retry_cnt = 3;
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (sta_connected) {
        sta_manual_selection = true;
        esp_wifi_disconnect();
    } else {
        esp_wifi_connect();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            if (sta_cred_count > 0) {
                sta_ap_index = 0;
                while (sta_ap_index < sta_cred_count && strlen(sta_creds[sta_ap_index].ssid) == 0)
                    sta_ap_index++;
                if (sta_ap_index < sta_cred_count) {
                    ESP_LOGI(TAG, "WiFi STA started, connecting to %s...", sta_creds[sta_ap_index].ssid);
                    sta_connect_to_index(sta_ap_index);
                } else {
                    ESP_LOGW(TAG, "WiFi STA started, all slots empty");
                }
            } else {
                ESP_LOGI(TAG, "WiFi STA started, no credentials configured");
            }
        } else if (id == WIFI_EVENT_STA_CONNECTED) {
            sta_connected = true;
            wifi_event_sta_connected_t *e = (wifi_event_sta_connected_t*)data;
            memcpy(sta_ssid_connected, e->ssid, e->ssid_len);
            sta_ssid_connected[e->ssid_len] = 0;
            ESP_LOGI(TAG, "WiFi STA connected to %s", sta_ssid_connected);
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            sta_connected = false;
            sta_got_ip = false;
            sta_ip_str[0] = 0;
            if (sta_manual_selection) {
                sta_manual_selection = false;
                ESP_LOGI(TAG, "WiFi STA disconnected (manual selection), connecting to %s...",
                         sta_creds[sta_ap_index].ssid);
                esp_wifi_connect();
            } else if (sta_cred_count > 1) {
                sta_ap_index = (sta_ap_index + 1) % sta_cred_count;
                ESP_LOGW(TAG, "WiFi STA disconnected, trying %s (idx=%d)...",
                         sta_creds[sta_ap_index].ssid, sta_ap_index);
                sta_connect_to_index(sta_ap_index);
            } else {
                ESP_LOGW(TAG, "WiFi STA disconnected, retrying...");
                esp_wifi_connect();
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t*)data;
        sta_got_ip = true;
        snprintf(sta_ip_str, sizeof(sta_ip_str), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "WiFi STA got IP: %s", sta_ip_str);
    }
}

static void wifi_apsta_init(void) {
    esp_err_t ret;

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %d", ret); return;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop failed: %d", ret); return;
    }

    // Crea netif per AP e STA
    ap_netif = esp_netif_create_default_wifi_ap();
    sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t w_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&w_cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "wifi init failed: %d", ret); return;
    }

    // Registra event handler
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    // Config AP (sempre attivo - nome include ruolo per distinguere TX/RX)
    wifi_config_t ap_cfg;
    memset(&ap_cfg, 0, sizeof(ap_cfg));
    if (cfg.role == 1)      strcpy((char*)ap_cfg.ap.ssid, "C6_KEYER-TX");
    else if (cfg.role == 2) strcpy((char*)ap_cfg.ap.ssid, "C6_KEYER-RX");
    else                    strcpy((char*)ap_cfg.ap.ssid, "C6_KEYER");
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.channel = 6;

    // Config STA (se ci sono credenziali)
    wifi_config_t sta_cfg;
    memset(&sta_cfg, 0, sizeof(sta_cfg));
    if (sta_cred_count > 0) {
        int first = 0;
        while (first < sta_cred_count && strlen(sta_creds[first].ssid) == 0) first++;
        if (first < sta_cred_count) {
            strcpy((char*)sta_cfg.sta.ssid, sta_creds[first].ssid);
            strcpy((char*)sta_cfg.sta.password, sta_creds[first].password);
            sta_cfg.sta.scan_method = WIFI_FAST_SCAN;
            sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
            sta_cfg.sta.threshold.rssi = -127;
            sta_cfg.sta.failure_retry_cnt = 3;
        }
    }

    ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "wifi set mode failed: %d", ret); return; }

    ret = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "wifi ap config failed: %d", ret); return; }

    if (sta_cred_count > 0) {
        ret = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        if (ret != ESP_OK) { ESP_LOGE(TAG, "wifi sta config failed: %d", ret); return; }
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "wifi start failed: %d", ret); return; }

    // Verifica AP: stampa l'IP dell'interfaccia AP
    esp_netif_ip_info_t ap_ip;
    if (esp_netif_get_ip_info(ap_netif, &ap_ip) == ESP_OK)
        ESP_LOGI(TAG, "AP IP: " IPSTR, IP2STR(&ap_ip.ip));
    else
        ESP_LOGW(TAG, "AP IP not available");

    ESP_LOGI(TAG, "WiFi APSTA: AP=%s (ch=6) STA=%s",
             ap_cfg.ap.ssid,
             sta_cred_count > 0 ? sta_creds[0].ssid : "(none)");
}

static esp_err_t ws_handler(httpd_req_t *req);

static void web_server_init(void) {
    httpd_config_t srv_cfg = HTTPD_DEFAULT_CONFIG();
    srv_cfg.max_uri_handlers = 12;
    srv_cfg.lru_purge_enable = true;
    srv_cfg.stack_size = 4096;

    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_start(&server, &srv_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %d", ret);
        return;
    }

    httpd_uri_t u_root = {};
    u_root.uri = "/";
    u_root.method = HTTP_GET;
    u_root.handler = web_root_handler;
    u_root.user_ctx = NULL;
    if (httpd_register_uri_handler(server, &u_root) != ESP_OK) {
        ESP_LOGE(TAG, "register / failed");
    }

    httpd_uri_t u_status = {};
    u_status.uri = "/status";
    u_status.method = HTTP_GET;
    u_status.handler = web_status_handler;
    u_status.user_ctx = NULL;
    if (httpd_register_uri_handler(server, &u_status) != ESP_OK) {
        ESP_LOGE(TAG, "register /status failed");
    }

    httpd_uri_t u_save = {};
    u_save.uri = "/save";
    u_save.method = HTTP_GET;
    u_save.handler = web_save_handler;
    u_save.user_ctx = NULL;
    if (httpd_register_uri_handler(server, &u_save) != ESP_OK) {
        ESP_LOGE(TAG, "register /save failed");
    }

    httpd_uri_t u_wifi = {};
    u_wifi.uri = "/wifi";
    u_wifi.method = HTTP_GET;
    u_wifi.handler = web_wifi_handler;
    u_wifi.user_ctx = NULL;
    if (httpd_register_uri_handler(server, &u_wifi) != ESP_OK) {
        ESP_LOGE(TAG, "register /wifi failed");
    }

    httpd_uri_t u_scan = {};
    u_scan.uri = "/scan";
    u_scan.method = HTTP_GET;
    u_scan.handler = web_scan_handler;
    u_scan.user_ctx = NULL;
    if (httpd_register_uri_handler(server, &u_scan) != ESP_OK) {
        ESP_LOGE(TAG, "register /scan failed");
    }

    httpd_uri_t u_rec = {};
    u_rec.uri = "/reconnect";
    u_rec.method = HTTP_GET;
    u_rec.handler = web_reconnect_handler;
    u_rec.user_ctx = NULL;
    if (httpd_register_uri_handler(server, &u_rec) != ESP_OK) {
        ESP_LOGE(TAG, "register /reconnect failed");
    }

    httpd_uri_t u_catch = {};
    u_catch.uri = "/*";
    u_catch.method = HTTP_GET;
    u_catch.handler = web_catchall_handler;
    u_catch.user_ctx = NULL;
    if (httpd_register_uri_handler(server, &u_catch) != ESP_OK) {
        ESP_LOGE(TAG, "register /* (catchall) failed");
    }

    httpd_uri_t u_sel = {};
    u_sel.uri = "/selwifi";
    u_sel.method = HTTP_GET;
    u_sel.handler = web_selwifi_handler;
    u_sel.user_ctx = NULL;
    if (httpd_register_uri_handler(server, &u_sel) != ESP_OK) {
        ESP_LOGE(TAG, "register /selwifi failed");
    }

    httpd_uri_t u_del = {};
    u_del.uri = "/delwifi";
    u_del.method = HTTP_GET;
    u_del.handler = web_delwifi_handler;
    u_del.user_ctx = NULL;
    if (httpd_register_uri_handler(server, &u_del) != ESP_OK) {
        ESP_LOGE(TAG, "register /delwifi failed");
    }

    httpd_uri_t u_ws = {};
    u_ws.uri = "/ws";
    u_ws.method = HTTP_GET;
    u_ws.handler = ws_handler;
    u_ws.user_ctx = NULL;
    if (httpd_register_uri_handler(server, &u_ws) != ESP_OK) {
        ESP_LOGE(TAG, "register /ws failed");
    }

    ESP_LOGI(TAG, "HTTP server ready on port %d", srv_cfg.server_port);
}


// ============================================================
// WebSocket handler lato RX (blocca finché client connesso)
// ============================================================
static esp_err_t ws_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "WS client connected");
    ws_client_connected = true;
    uint8_t buf[sizeof(cw_packet_t)];
    while (1) {
        httpd_ws_frame_t ws = {};
        ws.type = HTTPD_WS_TYPE_BINARY;
        ws.payload = buf;
        ws.len = sizeof(buf);
        esp_err_t ret = httpd_ws_recv_frame(req, &ws, sizeof(buf));
        if (ret != ESP_OK) { break; }
        if (ws.type == HTTPD_WS_TYPE_BINARY && ws.len == sizeof(cw_packet_t) && rx_pkt_queue) {
            cw_packet_t pkt;
            memcpy(&pkt, buf, sizeof(pkt));
            if (xQueueSend(rx_pkt_queue, &pkt, 0) != pdTRUE) {
                ESP_LOGW(TAG, "WS rx queue full");
            }
        } else if (ws.type == HTTPD_WS_TYPE_CLOSE) { break; }
        else if (ws.type == HTTPD_WS_TYPE_PING) {
            httpd_ws_frame_t pong = {}; pong.type = HTTPD_WS_TYPE_PONG;
            httpd_ws_send_frame(req, &pong);
        }
    }
    ws_client_connected = false;
    ESP_LOGI(TAG, "WS client disconnected");
    return ESP_OK;
}

// ============================================================
// JITTER BUFFER RX TASK (legge da coda WS)
// ============================================================
#define JB_SIZE 128

typedef struct {
    uint8_t type;     // 1=ON, 0=OFF
    int64_t abs_us;   // tempo assoluto (esp_timer) in cui eseguire
    uint8_t seq;
    bool used;
} jb_entry_t;

static void net_rx_task(void *arg) {
    jb_entry_t jb[JB_SIZE] = {};
    int64_t jb_base_time = 0;
    int64_t jb_base_ts = -1;
    bool jb_key_on = false;
    int64_t jb_last_pkt_us = 0;

    esp_task_wdt_add(NULL);

    while (1) {
        esp_task_wdt_reset();
        int64_t now = esp_timer_get_time();

        // Coda non bloccante
        cw_packet_t pkt;
        if (rx_pkt_queue && xQueueReceive(rx_pkt_queue, &pkt, 0) == pdTRUE) {
            jb_last_pkt_us = now;
            DBG_INC(dbg_rx_total);

            if (jb_base_ts < 0) { jb_base_time = now; jb_base_ts = pkt.ts_ms; }
            int64_t ts_diff = (int64_t)(pkt.ts_ms - (uint32_t)jb_base_ts);
            int64_t abs_us = jb_base_time + ts_diff * 1000 + (int64_t)cfg.jb_latency * 1000;

            int slot = -1;
            for (int i = 0; i < JB_SIZE; i++) { if (!jb[i].used) { slot = i; break; } }
            if (slot < 0) {
                int64_t oldest = INT64_MAX;
                for (int i = 0; i < JB_SIZE; i++) {
                    if (jb[i].used && jb[i].abs_us < oldest) { oldest = jb[i].abs_us; slot = i; }
                }
            }
            if (slot >= 0) {
                jb[slot].type = pkt.type; jb[slot].abs_us = abs_us;
                jb[slot].seq = pkt.seq; jb[slot].used = true;
                DBG_INC(dbg_jb_in);
            }
        }

        // Processa JB
        while (1) {
            int best = -1;
            int64_t best_us = INT64_MAX;
            for (int i = 0; i < JB_SIZE; i++) {
                if (jb[i].used && jb[i].abs_us <= now && jb[i].abs_us < best_us) {
                    best = i; best_us = jb[i].abs_us;
                }
            }
            if (best < 0) break;
            if (jb[best].type == 1) {
                if (jb_key_on) send_key(false); // micro-gap
                jb_key_on = true;
                send_key(true);
                DBG_INC(dbg_jb_out);
                EVT_LOG(1, jb[best].seq, now);
            } else {
                if (jb_key_on) {
                    jb_key_on = false;
                    send_key(false);
                    DBG_INC(dbg_jb_out);
                    EVT_LOG(0, jb[best].seq, now);
                }
            }
            jb[best].used = false;
        }

        // Inattivita 3s: reset
        if (jb_last_pkt_us > 0 && now - jb_last_pkt_us > 3000000) {
            if (jb_key_on) { jb_key_on = false; send_key(false); }
            jb_base_time = 0; jb_base_ts = -1;
            memset(jb, 0, sizeof(jb));
            jb_last_pkt_us = 0;
        }

        // Stuck key recovery
        int64_t key_timeout = dot_length_us * 4 + 500000;
        if (key_timeout < 500000) key_timeout = 500000;
        if (jb_key_on && jb_last_pkt_us > 0 && now - jb_last_pkt_us > key_timeout) {
            jb_key_on = false; send_key(false);
        }

        vTaskDelay(1);
    }
}

static void net_init(void) {
    if (cfg.role == 1) {
        // TX: WS reconnect gestito da keyer_task
    } else if (cfg.role == 2) {
        rx_pkt_queue = xQueueCreate(64, sizeof(cw_packet_t));
        if (!rx_pkt_queue) ESP_LOGE(TAG, "rx_pkt_queue create failed");
        else xTaskCreate(net_rx_task, "net_rx", 8192, NULL, 5, NULL);
    }
    net_ready = true;
}

// ============================================================
// APP MAIN
// ============================================================
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "ESP32-C6 Remote Keyer v1.0");

    // 1. NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS flash needs erase, doing it");
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_LOGI(TAG, "NVS initialized");

    // 2. GPIO (appena possibile per debug seriale)
    gpio_init();
    ESP_LOGI(TAG, "GPIO initialized");

    // 3. Buzzer
    buzzer_init();
    buzzer_off();
    ESP_LOGI(TAG, "LEDC initialized (buzzer on pin %d)", PIN_BUZZER_NUM);

    // 4. Carica impostazioni
    settings_load();
    ESP_LOGI(TAG, "Settings loaded from NVS");

    // 5. Self-test (verifica HW)
    startup_test();
    ESP_LOGI(TAG, "Self-test completed");

    // 6. I2C + OLED display (auto-detect)
    i2c_init();
    if (bus_handle && disp_init()) {
        disp_redraw();
        ESP_LOGI(TAG, "SSD1306 display ready");
    } else {
        ESP_LOGW(TAG, "SSD1306 not detected on I2C bus");
    }

    // 7. Keyer task - priorità media
    xTaskCreate(keyer_task, "keyer", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Keyer task created (prio=5)");

    // 8. Menu task
    xTaskCreate(menu_task, "menu", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "Menu task created (prio=2)");

    // 9. WiFi AP+STA + Web Server
    wifi_apsta_init();
    web_server_init();
    xTaskCreate(dns_server_task, "dns", 2048, NULL, 3, NULL);
    xTaskCreate(beacon_task, "beacon", 2048, NULL, 1, NULL);
    mdns_init();
    mdns_hostname_set("esp32-keyer");
    mdns_instance_name_set("C6 Keyer");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "WiFi AP+STA + Web Server + DNS + mDNS ready");

    // 10. TCP (TX connect o RX listen in base al ruolo)
    net_init();

    ESP_LOGI(TAG, "Ready. MODE=%s WPM=%ld VOL=%ld BYP=%ld",
             cfg.mode == 0 ? "STRAIGHT" : cfg.mode == 1 ? "IAMBIC A" :
             cfg.mode == 2 ? "IAMBIC B" : "BUG",
             (long)cfg.wpm, (long)cfg.volume, (long)cfg.bypass);
    ESP_LOGI(TAG, "Config: +/WPM=up, MODE=cycle, BYPASS short=toggle long=save");
}
