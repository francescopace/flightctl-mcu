# PoC flightctl-mcu: risultati

Microcontrollori Espressif gestiti da **Flight Control 1.3.1** (upstream di Red Hat Edge Manager) senza Linux sul device e **senza modifiche al server**. Il server è in esecuzione su kind sul Mac. La PoC è stata eseguita dal 5 al 6 novembre 2026.

## Cosa è stato dimostrato

| Capacità | Come | Esito |
|---|---|---|
| Enrollment e identità | Chiave P-256 generata sul device, CSR, approvazione con `flightctl approve`, mTLS con TLS 1.3 | 5 device MCU registrati e un device finto su host |
| Monitoraggio | Status completo della API agent: risorse, `customInfo` (heap, RSSI, temperatura, traffico) e `Unknown` dopo 5 minuti offline | Funziona |
| Config senza riavvio | `spec.config` inline, validata dall'applicazione prima di essere salvata | Applicata in 1–6 s; una config non valida viene rifiutata e segnalata con OutOfDate e il messaggio d'errore |
| Firmware OTA | `spec.os.image` punta a un indice OCI con un firmware per chip; il download va direttamente nello slot A/B e viene verificato con sha256 | Rilascio graduale (prima il canary, poi il resto) su 3 device in 50–80 s |
| Rollback automatico | Il nuovo firmware viene confermato solo dopo uno status accettato dal server; altrimenti torna al precedente | Un firmware rotto torna indietro da solo in circa 3,5 minuti; il rollout si ferma al canary e gli altri device non lo ricevono |
| App WASM senza riavvio | `spec.applications` di tipo `container` con un'immagine OCI WASM, eseguita con WAMR | Aggiornamento da v1 a v2 su Xtensa e RISC-V con lo stesso file `.wasm`, nessun riavvio |
| Flotta eterogenea | Una sola fleet, un solo riferimento firmware, impostazioni per device tramite label e template | S3, C3, C5 ed ESP32 classico nella stessa fleet |

## Flotta provata

| Chip | Arch | Board | Sensore temperatura | LED | Fasi provate |
|---|---|---|---|---|---|
| ESP32-S3 (×2) | Xtensa LX7 | DevKitC N8R8 / N4R2 | interno | WS2812 su GPIO 38, ordine RGB (label) | F2–F5 |
| ESP32-C3 | RISC-V | SuperMini | interno | blu, attivo basso, GPIO 8 (label) | F2–F5 |
| ESP32-C5 | RISC-V | DevKitC | interno (circa 15 °C sotto gli altri) | WS2812 su GPIO 27 | F2–F5 |
| ESP32 classico | Xtensa LX6 | DevKit | **simulato** (nessun sensore supportato) | GPIO 2 | F2–F5 |
| ESP32-C6 | RISC-V | DevKitC | interno | WS2812 su GPIO 8 | solo compilato: la flash di questa board fallisce (checksum dello stub) |
| ESP32-S2 | Xtensa LX7 | — | — | — | non provato (board non collegata) |
| Host macOS | arm64 | device finto | simulato | stampato sul terminale | F1, F3, F5 (test senza hardware) |

## Misure

### Firmware (v2.6, ESP-IDF 5.5.6, build `-Og` di default)

| Chip | Immagine | Spazio libero nello slot OTA (1,6 MB) |
|---|---|---|
| ESP32 | 1.081 KB | 34% |
| ESP32-S3 | 1.077 KB | 35% |
| ESP32-C3 | 1.148 KB | 30% |
| ESP32-C5 | 1.247 KB | 25% |
| ESP32-C6 | 1.186 KB | 28% |

Ripartizione su C3 (codice e dati in flash):

| Componente | Dimensione |
|---|---|
| WiFi e lwIP | circa 280 KB |
| mbedTLS (crypto, TLS, X.509) | circa 222 KB |
| WAMR (fast interpreter) | 71 KB |
| **Agent flightctl-mcu** (`fc_core`: enrollment, spec, status, OTA, OCI, app) | **21 KB** |

L'agent occupa poco: quasi tutto lo spazio va allo stack di rete e alla crittografia, che servono comunque a qualsiasi device connesso in modo sicuro.

### RAM (heap interno, PSRAM spenta, con WAMR e l'app `alarm` in esecuzione)

| Chip | Libera a regime | Minimo dall'avvio |
|---|---|---|
| ESP32-S3 | circa 150 KB | 26 KB (durante il download OTA) |
| ESP32-C3 | circa 87 KB | 39 KB |
| ESP32-C5 | circa 68 KB | 29 KB |
| ESP32 classico | circa 88 KB | 60 KB |

Due handshake TLS 1.3 in parallelo (thread status e thread spec) più il download OTA sono il momento di picco. Sul C5 il margine è il più basso.

### Tempi

| Operazione | Tempo |
|---|---|
| Dall'avvio alla richiesta di enrollment (WiFi, SNTP, TLS) | circa 10 s |
| Cambio di config, dall'apply alla conferma del device | 1–6 s |
| Rollout firmware su 3 device (canary, poi gli altri) | 50–80 s, di cui circa 20 s per download e riavvio di ciascun device |
| Rollback di un firmware rotto | circa 3,5 min (150 s per la conferma più 2 riavvii) |
| Aggiornamento app WASM sulla fleet (canary, poi gli altri) | circa 50 s, quasi tutto attesa del rollout; sul device lo scambio è immediato |

### Traffico a riposo

Misurato dal device con i contatori `netTxBytes` e `netRxBytes` (byte in rete, TLS compreso) su 869 s, con status ogni 60 s e long-poll del server di circa 4 minuti:

| Device | Inviati | Ricevuti | Totale |
|---|---|---|---|
| ESP32 classico (con app WASM) | 236 KB/h | 493 KB/h | **circa 730 KB/h, circa 17 MB al giorno** |
| Device finto su host (senza app) | 183 KB/h | 380 KB/h | circa 560 KB/h |

Ogni richiesta costa circa 10 KB, e la maggior parte è l'handshake TLS 1.3 con mTLS, perché la PoC apre una connessione nuova per ogni richiesta. Lo status JSON vale 2–3 KB. Il traffico si riduce soprattutto con connessioni keep-alive o con la ripresa della sessione TLS e con status più rari o differenziali: va bene sul WiFi, ma va ottimizzato per NB-IoT o LTE-M. Nei 25 minuti di log seriale dell'ESP32 non ci sono stati errori né blocchi.

## Problemi trovati e corretti durante la PoC

1. **Il rollout andava avanti mentre il canary si riavviava.** Il device riportava la nuova `renderedVersion` subito dopo il download, quindi Flight Control contava il canary come aggiornato e passava agli altri prima che il nuovo firmware fosse confermato. Ora la versione resta quella precedente finché il firmware nuovo non è confermato; con questa correzione il rollout si ferma al canary.
2. **Memoria lineare WASM.** Ogni modulo WASM riserva almeno una pagina da 64 KB, che non trova spazio sui C3/C5. WAMR la riduce alla dimensione davvero usata solo se il modulo esporta sia `__heap_base` sia `__data_end`.
3. **Config non valida segnalata come aggiornata.** La versione veniva avanzata anche quando la config era rifiutata. Ora resta la precedente e l'errore compare in `updated.info`.
4. **ESP32 classico: status fermo per circa 6 minuti** dopo un OTA, causa non individuata; in seguito un handshake TLS è fallito una volta con "generic error". Ho aggiunto un watchdog: se per 10 minuti nessuno status viene accettato, il device si riavvia.

## Limiti di Flight Control emersi (proposte upstream)

1. **`matchExpressions` nel selettore delle fleet viene accettato ma ignorato.** La fleet resta vuota senza alcun errore. Il controller usa solo `matchLabels` (`internal/tasks/fleet_selector.go`). Proposta: supportarlo, oppure rifiutarlo in fase di validazione.
2. **Il successo di un batch si basa solo su `renderedVersion`.** Un agent che segnala la versione prima di aver confermato il nuovo OS fa avanzare il rollout. L'agent Linux lo evita in modo implicito. Proposta: scrivere questo requisito nel contratto della API agent, oppure basare il successo anche su `os.image` e sulla condition `Updating`.
3. **Timeout di disconnessione fisso a 5 minuti** (`DeviceDisconnectedTimeout`). I device a batteria in deep sleep risulterebbero `Unknown` per la maggior parte del tempo. Proposta: renderlo configurabile per fleet o per device, oppure far dichiarare al device il proprio intervallo di contatto atteso.
4. **Nessun modo di dichiarare la classe del device o i runtime supportati.** Il server accetta per un MCU anche app `compose`, `quadlet` o `helm`, e campi Podman (porte, volumi, `runAs`) che lì non hanno significato; l'errore emerge solo sul device. Proposta: estendere `status.capabilities` (oggi c'è `osMode`) con qualcosa come `applicationRuntimes: [wasm]` e validare il rendering in base a quello. Eventualmente aggiungere un `appType: wasm` esplicito, oppure riconoscere come WASM le immagini con piattaforma `wasip1/wasm`.
5. **Nessun campo di stato libero per singola app.** `DeviceApplicationStatus` non ha campi come `message` o `info`; lo stato dell'app è finito in `customInfo` (`app.<name>`).
6. **Costo di ogni status.** Ogni PUT invia lo status completo, 2–3 KB di JSON. Proposta: un PATCH documentato come percorso consigliato per i device con risorse limitate, oppure una variante solo heartbeat.
7. **Il server confronta `os.image` solo come stringa.** Ripubblicare lo stesso tag non viene visto come aggiornamento. Proposta: consigliare il riferimento per digest (`@sha256:`) per firmware e app, o confrontare `imageDigest`.

## Limiti della PoC (non pronta per produzione)

- **Registry in HTTP senza autenticazione**; firmware e app **non sono firmati**: mancano cosign o firme verificate dal bootloader (Secure Boot v2) e la flash encryption. La chiave del device sta in NVS in chiaro.
- **WiFi configurato in fase di build.** Manca il provisioning (BLE o SoftAP), così come il trasferimento della config di enrollment.
- **Rinnovo del certificato di gestione non implementato.** La API (`certificatesigningrequests`) esiste.
- **Una connessione TLS per ogni richiesta**, senza keep-alive: è semplice ma aumenta traffico e consumo.
- **Formati delle app**: solo l'artifact OCI WASM. Le immagini podman "compat" (con `.wasm` in un layer tar.gz) non sono supportate.
- **Build di debug** (`-Og`): con `-Os` le immagini sarebbero più piccole.
- **ESP32-S2 non provato.** Sul C6 la PoC è solo compilata.

## Prossimi passi suggeriti

1. Aprire una discussione upstream sui punti 1–4 dei limiti di Flight Control (quelli che bloccano l'adozione per gli MCU).
2. Sicurezza: firma delle immagini con verifica sul device, Secure Boot v2, flash encryption, chiave del device nell'HMAC/DS peripheral, dove il chip lo supporta.
3. Provisioning WiFi e dell'enrollment in fabbrica o sul campo.
4. Port Zephyr dello strato `port/`: il core è C POSIX e WAMR supporta Zephyr.
5. Gestione dei device a batteria: long-poll opzionale e status solo al risveglio, legata al punto 3 dei limiti.
