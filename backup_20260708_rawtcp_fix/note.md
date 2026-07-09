# ESP32 Remote Keyer - Backup 09/07/2026 (RAW TCP)

## Stato: FUNZIONANTE

Comunicazione TX→RX via **TCP raw** sulla **porta 7373**. WebSocket eliminato.

## Architettura

- **TX** (role=1): TCP client raw, connette a `remote_ip:7373`, invia `cw_packet_t` (8 byte)
- **RX** (role=2): `tcp_rx_task` (task separato) ascolta su **7373**, riceve pacchetti raw
- **HTTP server** su **porta 80** solo per web UI, mai bloccato
- **Router Pankonet**: port forwarding **7373 → 192.168.0.26:7373**

## Ottimizzazioni applicate

1. **TCP raw** invece di WebSocket — nessun handshake, nessun handler bloccante
2. **`tcp_rx_task` separato** dall'HTTP server — non lo blocca mai
3. **TCP_NODELAY** sulla socket TX
4. **WIFI_PS_NONE** — disabilita risparmio energetico WiFi
5. **Poll skip/health check** quando `tcp_state == 3`
6. **`tcp_retry_us = 0`** su ogni errore — riconnessione immediata
7. **`sta_got_ip`** azzera `ws_sock` se WiFi cade
8. **`tcp_retry_us = 0`** quando WiFi ottiene IP
9. **SO_KEEPALIVE** (idle=1s, intvl=1s, cnt=2) su TX e RX — rileva caduta in ~3s
10. **SO_RCVTIMEO=1s** sul task server RX
11. **Priorità `net_rx_task`** = 6
12. **JB_SIZE** = 32
13. **Drain completo** coda RX
14. **Heartbeat log** solo su TX

## Bug risolti

- **Disconnessione 15s** dal poll con events=0
- **Handler RX bloccato** — task separato, non blocca server HTTP
- **Riconnessione lenta** — `tcp_retry_us = 0` ovunque
- **TX non riconnette dopo power-cycle** — keepalive su entrambi i lati
- **Stallo dopo treno CW** — risolto rimuovendo timeout 5s sul WS handler (poi eliminato del tutto con TCP raw)

## Note

- `ws_sock` ora è la socket TCP raw
- `tcp_state` ora è lo stato della connessione TCP (1=connecting, 3=connected)
- `ws_client_connected` settato da `tcp_rx_task`
- `cfg.remote_port` rimosso (hardcoded 7373)
- `ws_send_bin` rimosso (send raw direttamente)
- `ws_read_frame` rimosso (non più necessario)
- `httpd_ws_recv_frame` testato ma non funzionante con i frame custom

## Build/Flash

- Board: esp32-c6-devkitc-1
- ESP-IDF 6.0.1
- TX=COM9, RX=COM8
- `CONFIG_HTTPD_WS_SUPPORT=1` (non più necessario ma lasciato)
- `CONFIG_FREERTOS_HZ=1000`
