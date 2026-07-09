# ESP32 Remote Keyer - Backup 08/07/2026

## Stato attuale

Progetto funzionante. Comunicazione TX→RX via **WebSocket** su TCP (non più UDP).

### Architettura

- TX (role=1): WS client, si connette a `remote_ip:80`
- RX (role=2): WS server, HTTP server su porta 80, handler `/ws`
- Port forwarding sul router Pankonet: **80 → 192.168.0.26:80**
- L'IP remoto (`cfg.remote_ip`) è configurato su `185.55.209.71` (IP pubblico Pankonet)

### Ottimizzazioni applicate

1. **TCP_NODELAY** sulla socket TX — disabilita Nagle, latenza ridotta
2. **WIFI_PS_NONE** — disabilita risparmio energetico WiFi
3. **Poll skip** quando `tcp_state == 3` (connesso) — non spreca cicli
4. **Health check** ogni 100ms in `tcp_state == 3` — rileva disconnessioni silenti via poll + `SO_ERROR`
5. **`tcp_retry_us = 0`** su ogni errore/chiusura — riconnessione immediata (non 3s)
6. **`sta_got_ip` check** — non tenta connessione prima che WiFi abbia IP
7. **Heartbeat log** spostato dopo il check RX — non logga in modalità RX
8. **Drain completo** coda RX (`while` loop su `xQueueReceive`)
9. **JB_SIZE ridotto** da 128 a 32
10. **Priorità `net_rx_task`** elevata a 6 (sopra HTTP server)

### Variabili rinominate

- `udp_tx_sock` → `ws_sock`
- `udp_tx_seq` → `pkt_seq`
- `udp_tx_on_us` → `key_start_us`
- `remote_port` rimosso da struct/NVS/web UI (non usato, WS va sulla 80)

### Bug risolti

- **Disconnessione 15s**: il poll con `events=0` quando `tcp_state==3` triggerava il timeout "connect timeout" dopo 15s, chiudendo la connessione. Risolto saltando il poll quando connesso.
- **Handler RX bloccato**: l'HTTP server ESP-IDF è single-thread. Se il `ws_handler` è in `recv()`, non accetta nuove connessioni. L'health check TX rileva la caduta, chiude la vecchia socket (FIN → recv restituisce 0 → handler esce), e riconnette.
- **Riconnessione lenta**: `tcp_retry_us` non veniva azzerato sugli errori, causando attese fino a 3s prima di riprovare.

### Problemi aperti

- **wpm_mode cambia**: Non più bloccante.
- **Health check TX**: Implementato con poll + `SO_ERROR` e `tcp_retry_us = 0`. **DA VERIFICARE** — non ancora testato con reset RX.
- **TCP keepalive** (`SO_KEEPALIVE`) sulla socket TX dopo connessione WS (idle=5s, intvl=3s, cnt=3). Rileva cadute di rete/reset RX entro ~14s senza traffico.
- **RX 5s timeout**: Era stato testato ma rimosso — causava disconnessione durante pause CW normali. Soluzione attuale: nessun timeout, handle RX si libera quando TX riconnette.

### Build/Flash

- Board: esp32-c6-devkitc-1
- Framework: ESP-IDF 6.0.1
- Porte: TX=COM9, RX=COM8
- `build_flags = -DCONFIG_HTTPD_WS_SUPPORT=1`
- `CONFIG_FREERTOS_HZ=1000`
