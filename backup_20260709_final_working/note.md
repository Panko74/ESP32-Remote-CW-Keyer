# ESP32 Remote Keyer - Backup 09/07/2026 ✅ FUNZIONANTE

## Stato: COMPLETO E FUNZIONANTE

Comunicazione TX→RX via **TCP raw** sulla **porta 7373**. Task server separato dall'HTTP server.

## Architettura

- **TX** (role=1): TCP client, connette a `remote_ip:7373`, invia `cw_packet_t` (8 byte)
- **RX** (role=2): `tcp_rx_task` (task separato, prio=4) ascolta su **7373**
- **HTTP server** su **porta 80** solo per web UI, mai bloccato
- **Router Pankonet**: port forwarding **7373 → 192.168.0.26:7373**

## Display OLED (SSD1306)

**Pagina IP** (al boot, mostra IP WiFi):
```
MOD:IAMBIC B
[ONLINE] O
SPD:30WPM
VOL:5/10
IP:192.168.0.26
```

**Pagina 1** (dopo pressione BYPASS):
```
MOD: IAMBIC B
TX:[ONLINE]             ███
SPEED: 30 WPM
VOL: 5 / 10
```

In bypass: `TX:[PRACTICE]`. Indicatore `███` a destra se connesso.

## Pulsanti

- **BYPASS** breve: nasconde IP / toglie bypass, seconda pressione alterna bypass
- **BYPASS** lungo (>1.5s): salva impostazioni (mostra "SAVED!")
- **MODE**: ciclo modi (STRAIGHT → IAMBIC A → IAMBIC B → BUG)
- **WPM +/-**: varia velocità (5-70 WPM), menu_task priorità 2
- **Menu + UP/DOWN**: combinato per volume

## Ottimizzazioni rete

1. **TCP raw** invece di WebSocket — nessun handshake, nessun handler bloccante
2. **`tcp_rx_task` separato** dall'HTTP server (prio=4, stack=4096)
3. **Keepalive** (idle=1s, intvl=1s, cnt=2) su TX e RX — rileva caduta in ~3s
4. **`SO_RCVTIMEO=1s`** sul task server RX — non resta bloccato per sempre
5. **TCP_NODELAY** sulla socket TX
6. **WIFI_PS_NONE** — disabilita risparmio energetico WiFi
7. **Poll skip** quando `tcp_state == 3` — non spreca cicli
8. **Health check** ogni 100ms in `tcp_state == 3` — rileva disconnessioni
9. **`tcp_retry_us = 0`** su ogni errore — riconnessione immediata
10. **WiFi disconnect → `ws_sock` chiusa** — niente attesa 3s
11. **`sta_got_ip → tcp_retry_us = 0`** — reconnect subito dopo WiFi
12. **Heartbeat log** solo su TX
13. **Drain completo** coda RX (while loop)
14. **JB_SIZE** = 32
15. **Priorità `net_rx_task`** = 6

## Variabili rinominate

- `ws_sock` (era `udp_tx_sock`)
- `pkt_seq` (era `udp_tx_seq`)
- `key_start_us` (era `udp_tx_on_us`)
- `remote_port` rimosso da struct/NVS/web UI

## Note

- `ws_client_connected` settato da `tcp_rx_task`
- Connessione via TCP raw su 7373, niente WebSocket
- `cfg.remote_ip` usato per l'indirizzo del server remoto
- `disp_show_ip` mostra l'IP al primo boot

## Build/Flash

- Board: esp32-c6-devkitc-1, ESP-IDF 6.0.1
- TX=COM9, RX=COM8
- `build_flags = -DCONFIG_HTTPD_WS_SUPPORT=1` (non più necessario ma lasciato)
- `CONFIG_FREERTOS_HZ=1000`
