# ESP32-C6 Remote Keyer v1.0.0

**Trasmettitore/Ricevitore CW remoto via TCP su ESP32-C6**

Coppia di dispositivi ESP32-C6 che permettono di azionare un tasto telegrafico CW in remoto via Internet. Il TX (collegato al paddle/bug/tasto verticale) invia i punti e le linee via TCP all'RX (collegato alla radio), che li riproduce su radio tramite l'ingresso Key (in modalità tasto verticale), fedelmente, con lo stesso timing e "firma" della trasmissione.
Latenza impostabile da 0 a 1000ms (default 100ms).
Il sidetone del TX è in locale su buzzer, per una perfetta sincronia dell'ascolto con la manipolazione.

> Due dispositivi identici. Il ruolo (TX/RX/Standalone) si assegna via web UI.

---

## Architettura

```
┌──────────────┐    TCP raw      ┌─────────────┐
│  TX (paddle) │───porta 7373──▶│  RX (radio) │
│   hotspot    │                 │  IP fisso   │
│   telefono   │                 │  router     │
└──────────────┘                 └─────────────┘
```

- **TX**: si connette via TCP all'IP pubblico dell'RX, porta **7373**
- **RX**: server TCP su porta **7373**, task separato dall'HTTP server
- **Trasporto**: TCP raw, nessuna dipendenza WebSocket
- **Server HTTP** su porta **80**: web UI sempre reattiva (non bloccata dal traffico CW)

---

## Requisiti hardware

- 2x ESP32-C6-DevKitC-1 (o ESP32-C6 compatibile)
- Paddle/Bug/Tasto verticale telegrafico (per il TX)
- Buzzer o radio (per l'RX)
- Display SSD1306 I2C 128×64 (per entrambi)
- Resistenze pull-up I2C 4.7kΩ (se il display non le ha integrate)
- Alimentazione USB (power bank, caricatore, porta PC) o con BMS interno per batteria Li 3,7V.
- Router con IP pubblico statico (rete dell'RX)
- Telefono cellulare con hotspot (per il TX)

---

## Collegamenti (pinout)

```
ESP32-C6-DevKitC-1
┌────────────────────────┐
│ GPIO 2  ── Paddle DOT  │
│ GPIO 3  ── Paddle DASH │
│ GPIO 4  ── WPM +       │
│ GPIO 5  ── WPM −       │
│ GPIO 6  ── MODE        │
│ GPIO 7  ── OUT RADIO   │
│ GPIO 10 ── BYPASS      │
│ GPIO 15 ── BUZZER      │
│ GPIO 18 ── SDA (I2C)   │
│ GPIO 19 ── SCL (I2C)   │
└────────────────────────┘
```

| Pin | Funzione | Note |
|-----|----------|------|
| GPIO 2 | Paddle DOT | Input pull-up interno, GND = premuto |
| GPIO 3 | Paddle DASH | Input pull-up interno, GND = premuto |
| GPIO 4 | WPM UP | Input pull-up, GND = premuto |
| GPIO 5 | WPM DOWN | Input pull-up, GND = premuto |
| GPIO 6 | MODE | Input pull-up, GND = premuto |
| GPIO 7 | OUT RADIO | Uscita, collegata al relè/keyer radio settata in modalità tasto verticale |
| GPIO 10 | BYPASS | Input pull-up, GND = premuto |
| GPIO 15 | BUZZER | Uscita PWM 600Hz, pilotare con transistor se necessario |
| GPIO 18 | I2C SDA | Display SSD1306 |
| GPIO 19 | I2C SCL | Display SSD1306 |

> Il display è opzionale. Se non rilevato all'avvio, il dispositivo funziona comunque (solo web UI).

---

## Prima configurazione

### 1. Flash del firmware

Scarica l'ultima release da GitHub. Con PlatformIO:

```bash
# RX
pio run -t upload --upload-port COM8 (o la COM del RX rilevata dal PC)
# TX
pio run -t upload --upload-port COM9 (o la COM del TX rilevata dal PC)
```

Per la prima installazione su ESP vergine: può essere necessario tenere premuto il boot button mentre si collega l'USB.

### 2. Connessione al WiFi

Alla prima accensione (o dopo erase NVS), il ruolo è **Standalone** e l'AP ha nome generico:

| Stato | Nome AP | IP AP |
|-------|---------|-------|
| Prima configurazione | `C6_KEYER` | 192.168.4.1 |
| Dopo aver impostato Role=TX | `C6_KEYER-TX` | 192.168.4.1 |
| Dopo aver impostato Role=RX | `C6_KEYER-RX` | 192.168.4.1 |

1. Connettiti all'AP del dispositivo (telefono/PC)
2. Apri browser a `http://192.168.4.1`
3. Vai su **WiFi Network Settings**
4. Clicca **Scan Networks**
5. Seleziona la tua rete WiFi
6. Inserisci password
7. **Save WiFi**

Il dispositivo si riavvia e si connette alla rete. L'AP rimane attivo per future riconfigurazioni.

> **RX**: il dispositivo che starà presso la stazione radio va connesso alla **rete WiFi fissa** di quella postazione (la stessa su cui farai il port forwarding). Il TX invece può essere su qualsiasi rete (hotspot telefono, WiFi viaggio, ecc.).

### 3. IP fisso per l'RX

Accedi al router della rete dell'RX e trova il MAC del dispositivo tra i **dispositivi connessi** (cerca il nome `ESP32Keyer` o l'IP dell'RX). Imposta un **lease DHCP statico** associando il MAC all'IP desiderato. In questo modo, dopo ogni reboot, l'RX mantiene lo stesso IP.

> Senza IP fisso, dopo un reboot l'RX potrebbe ricevere un IP diverso e il port forwarding smetterebbe di funzionare.

### 4. Port forwarding sul router

Sul router dell'RX aggiungi una regola di port forwarding:

```
Porta esterna: 7373 → IP_RX:7373 (TCP)
```

### 5. Assegnazione ruoli

Dalla web UI di ciascun dispositivo (`http://192.168.4.1`):

**RX**: imposta **Role = RX**, salva. Il dispositivo si riavvia in modalità RX e il server TCP su 7373 parte.

**TX**: imposta **Role = TX**, **Remote IP** = IP pubblico del router dell'RX, salva. Il dispositivo si riavvia in modalità TX e si connette automaticamente all'RX.

> Dopo il primo save, il dispositivo riavvia. L'AP potrebbe impiegare alcuni secondi a riapparire.

### 6. Hotspot telefono per il TX

Connetti il TX all'hotspot del telefono. Le reti mobili (TIM, Vodafone, Iliad, WINDTRE, ecc.) hanno:

- **Porte in uscita**: generalmente aperte (il TX può connettersi all'RX)
- **Porte in entrata**: generalmente chiuse (non serve, il TX non deve ricevere connessioni)

> Se usi una saponetta 4G/5G al posto dell'hotspot, verifica che non blocchi le connessioni in uscita sulla porta 7373 e sulla 80.

---

## Primo test

1. Alimenta entrambi i dispositivi
2. Attendi che il TX si connetta (display mostra `TX:[ONLINE] █`)
3. Premi il paddle sul TX
4. L'RX dovrebbe emettere il tono dal buzzer

Se non senti nulla:
- Controlla che il **volume** non sia a 0 (MODE + WPM +/-)
- Controlla che il **bypass** non sia attivo (premi BYPASS)
- Controlla la connessione TCP (il rettangolo `█` deve essere visibile)

---

## Uso

### Pulsanti

| Pulsante | Azione breve | Azione lunga (>1.5s) |
|----------|-------------|---------------------|
| **MODE** | Ciclo modi (STRAIGHT → IAMBIC A → IAMBIC B → BUG) | — |
| **BYPASS** | Nasconde IP / attiva/disattiva bypass | **Salva impostazioni** (mostra "SAVED!") |
| **WPM +** | Aumenta velocità (5-70 WPM) | — |
| **WPM −** | Diminuisce velocità (5-70 WPM) | — |

> **MODE + WPM +/-** tenuti insieme dopo aver premuto MODE: regolano il **volume** invece del modo.

### Display OLED

**Pagina IP** (al primo avvio o dopo reset NVS, mostra IP unica volta):
```
MOD:IAMBIC B
[ONLINE] O
SPD:30WPM
VOL:5/10
IP:192.168.0.50
```

**Pagina 1** (dopo prima pressione BYPASS):
```
MOD: IAMBIC B
TX:[ONLINE]  █
SPEED: 30 WPM
VOL: 5 / 10
```

- `O` / `█`: indicatore di connessione attiva
- `TX:[ONLINE]`: TX connesso all'RX
- `TX:[PRACTICE]`: bypass attivo, non trasmette via radio
- `RX:[ONLINE]`: RX in ascolto

> Il display va in sleep dopo 15s di inattività. Qualsiasi pulsante lo riaccende.

### Modi operativi

| Modo | Descrizione |
|------|-------------|
| **STRAIGHT** | Tasto verticale.|
| **IAMBIC A** | Auto punti e linee, squeeze = alternanza continua |
| **IAMBIC B** | IAMBIC A con memoria |
| **BUG** | Punti automatici (premi DOT), linee manuali (premi DASH) |

---

## Web UI

Ogni dispositivo ha una web UI completa sulla porta **80**:

| URL | Funzione |
|-----|----------|
| `http://IP_DISPOSITIVO` | Controlli principali (WPM, volume, modo, bypass, ruolo) |
| `http://IP_DISPOSITIVO/wifi` | Configurazione WiFi e IP remoto |
| `http://IP_DISPOSITIVO/status` | JSON stato (per diagnostica) |

Per accedere via AP: `http://192.168.4.1`

---

## Diagnostica via seriale

Collega via USB e apri un monitor seriale (115200 baud):

```bash
pio device monitor -p COM8 (o la COM rilevata dal PC) -b 115200
```

Il dispositivo stampa:
- `TCP connect(...:7373) ret=... EINPROGRESS` → tentativo di connessione
- `TCP connected to ...:7373` → connessione stabilita
- `send_key errno=...` → errore di invio (seguito da riconnessione)
- `WiFi STA disconnected` → WiFi perso, il dispositivo si riconnette
- `TCP server listening on port 7373` → RX in ascolto
- `TCP client connected on fd=...` → RX ha accettato una connessione

Per il debug avanzato, abilitare `CONFIG_LOG_DEFAULT_LEVEL_INFO` nel menuconfig.

---

## Aggiornamento firmware

Le impostazioni (WiFi, ruolo, IP remoto, ecc.) sono salvate in **NVS** e **non vengono perse** durante un normale flash. Per un aggiornamento:

```bash
# Aggiorna entrambi
pio run -t upload --upload-port COM8 (o la COM del RX rilevata dal PC)
pio run -t upload --upload-port COM9 (o la COM del TX rilevata dal PC)
```

Per resettare le impostazioni ai default (raro):

```bash
pio run -t erase --upload-port COM8 (o la COM rilevata dal PC)  # cancella TUTTO, NVS incluso
pio run -t upload --upload-port COM8 (o la COM rilevata dal PC) # riflasha
```

---

## Troubleshooting

### "Il TX non si connette all'RX"
- L'RX ha un IP diverso? Verifica sul router. Aggiorna il port forwarding
- La porta 7373 è forwardata sul router? Usa [canIP](https://www.canyouseeme.org) per testarla
- Il TX è su hotspot? Alcuni operatori bloccano la 7373 in uscita
- Controlla il log seriale del TX: cerca `TCP connect timeout 15s`

### "Il display non si accende"
- Display non saldato/collegato? Il dispositivo funziona anche senza
- I2C non rilevato all'avvio: logga `SSD1306 not detected on I2C bus`
- Pull-up I2C mancanti? Aggiungi resistenze 4.7kΩ su SDA/SCL a 3.3V

### "Il CW si sente ma poi si ferma"
- Connessione TCP caduta? Controlla il pallino `█` sul display

### "Il volume è zero"
- Volume regolabile via web UI o MODE + WPM +/- dopo aver premuto MODE
- Controlla che `VOL: OFF` non sia visualizzato sul display

### "Il WiFi si disconnette continuamente"
- Segnale debole? Avvicina il dispositivo al router
- Rete 5GHz? Il ESP32-C6 supporta solo 2.4GHz
- Troppe reti sullo stesso canale? Prova a cambiare canale sul router

### "Dopo un erase, il dispositivo non si connette più al WiFi"
- Le credenziali WiFi erano in NVS che è stato cancellato
- Riconnettiti all'AP `C6_KEYER` e riconfigura

---

## Specifiche tecniche

| Parametro | Valore |
|-----------|--------|
| MCU | ESP32-C6 (RISC-V, 160 MHz) |
| WiFi | 2.4 GHz 802.11 b/g/n/ax (WiFi 6) |
| Display | SSD1306 I2C 128×64 (opzionale) |
| Buzzer | PWM 600 Hz, volume 0-10 |
| Range WPM | 5-70 |
| Latenza buffer | 50-1000 ms (default 200 ms) |
| Keepalive TCP | 1s idle, 1s interval, 2 probe (rileva caduta in ~3s) |
| Porte | 80 (web UI), 7373 (CW TCP) |
| Alimentazione | 5V USB / pin VBUS, BMS step-up 3.7V→5V su batteria Li, ~200 mA |

## Build da sorgente

```bash
git clone https://github.com/.../ESP32RemoteKeyer.git
cd ESP32RemoteKeyer
pio run -t upload
```

## Future implementazioni
- Pubblicazione dello schema dei collegamenti
- DNS dinamico per provider internet senza IP pubblico statico 

## Licenza

MIT
