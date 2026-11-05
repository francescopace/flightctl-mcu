# PoC: device ESP32 gestiti da Flight Control (approccio POSIX, base ESP-IDF)

## Context

Obiettivo: dimostrare che microcontrollori Espressif possono essere gestiti, monitorati e aggiornati da un control plane Flight Control (upstream di Red Hat Edge Manager, integrabile in OpenShift/ACM), senza Linux sul device e **senza modifiche al server**. Tre livelli di aggiornamento: config (senza riavvio), firmware completo (OTA A/B con rollback), logica applicativa WASM distribuita come immagine OCI (senza riavvio).

Il codice core è C POSIX (pthread, socket BSD, file, mbedTLS, WAMR) e gira sia su host Linux/macOS (sviluppo e test senza hardware) sia su ESP-IDF; Zephyr resta un target futuro dietro lo stesso strato `port/`.

## Assunzioni

**Verificate nel codice di Flight Control (main, 4 nov 2026):**
- API agent `api/agent/v1beta1/openapi.yaml`: `POST/GET enrollmentrequests`, `POST/GET certificatesigningrequests`, `GET devices/{name}/rendered?knownRenderedVersion=` (long-poll fino a ~4 min, `204` se invariato), `PUT/PATCH devices/{name}/status`.
- mTLS, **TLS 1.3**, ECDSA P-256/SHA-256 (`docs/user/references/certificate-architecture.md`).
- Nome device = `lower(base32hex_nopad(sha256(trimLeft0(X) || trimLeft0(Y))))` della chiave pubblica (`pkg/crypto/key.go:89`, `internal/agent/identity/identity.go:132`).
- Status: campi obbligatori `conditions, systemInfo{architecture,bootID,operatingSystem,agentVersion}, applications, applicationsSummary, resources{cpu,memory,disk}, integrity, config{renderedVersion}, os{image,imageDigest}, updated, summary, lifecycle`.
- Device "aggiornato" se `status.config.renderedVersion` = renderedVersion servito; OS out-of-date se `status.os.image != spec.os.image` (confronto stringa, `internal/service/common/device.go:262`). Usare `osMode: image`.
- App `container`: il server valida solo riferimento OCI, limiti formato Podman, porte/env/volumi (`api/core/v1beta1/validation.go:1231`); i delta hint falliscono in modo silenzioso se l'immagine non è ispezionabile.
- Device `Unknown` dopo 5 min senza contatto (`DeviceDisconnectedTimeout`, non configurabile).
- Fleet: template con placeholder Go (`{{ .metadata.labels.x }}`), rollout `BatchSequence` con `successThreshold`.
- Deploy locale: `make deploy` su kind, porte host 3443 (API), 7443 (agent), 9001 (UI). Enrollment config: `flightctl certificate request --signer=enrollment --expiration=365d --output=embedded`. Approvazione: `flightctl approve -l k=v enrollmentrequest/<name>`.

**Da verificare durante la PoC:**
- mbedTLS di ESP-IDF v5.x come **client TLS 1.3** con mTLS ECDSA, e RAM del handshake su C3/S2.
- WAMR (fast-interp) su ESP-IDF per Xtensa (S3) e RISC-V (C3/C5/C6).
- Kind su Podman machine (macOS): porta 7443 raggiungibile dalla LAN; SAN del certificato server con IP o hostname nip.io della LAN.
- Il server accetta i valori di status "non Linux" (es. `operatingSystem: esp-idf`, `architecture: xtensa-esp32s3`, `disk` da LittleFS).

**Semplificazioni PoC (fuori scope):** niente TPM/attestation, chiave in NVS senza flash encryption, niente firma cosign, registry locale senza auth via HTTP, WiFi configurato a build time, nessuna modifica al server.

## Hardware e ruoli

| Chip | Qtà | Ruolo PoC |
|---|---|---|
| ESP32-S3 | 2 | Target principale: config + firmware + WASM. Uno `stage=canary` |
| ESP32-C3 | 3 | RISC-V: config + firmware + WASM (stesso `.wasm` dell'S3) |
| ESP32-C5 / C6 | 2 / 1 | RISC-V: come C3; in seguito chiave ECDSA in hardware |
| ESP32-S2 / ESP32 | 3 / 1 | Solo config + firmware (RAM limitata), flotta eterogenea |

## Caso d'uso PoC: "allarme temperatura di linea"

Ogni device legge il sensore di temperatura interno (fallback simulato sull'ESP32 classico), lo riporta allo stato e accende il LED di bordo sopra una soglia.
- **Config**: `threshold_c`, `sample_ms` in un file inline della fleet → cambio senza riavvio.
- **Firmware**: v1 → v2 (nuova info in status) con rollout canary; v3 volutamente rotta → rollback automatico.
- **WASM** (`alarm` v1 → v2): v1 LED fisso, v2 lampeggio con isteresi; sostituito senza riavvio, stesso digest su Xtensa e RISC-V.
- **Monitoraggio**: heap, RSSI, uptime, temperatura in `status`/`customInfo`; device spento → `Unknown` dopo 5 min.

## Architettura (nuovo repo, nome provvisorio `flightctl-mcu`)

```
core/            C POSIX, nessun include ESP-IDF
  fc_client.c    HTTP/TLS (mbedTLS) + JSON (cJSON) verso agent API
  fc_identity.c  chiave P-256, CSR PKCS#10, calcolo nome device
  fc_enroll.c    enrollment request → polling approvazione → cert gestione
  fc_spec.c      loop long-poll rendered, knownRenderedVersion
  fc_status.c    costruzione status completo, PUT/PATCH periodico
  fc_reconcile.c applica config / os / applications, calcola stato
  oci_client.c   manifest/index/blob, verifica sha256, streaming
  wasm_rt.c      WAMR: load/instantiate/start/stop, API host
port/port.h      ~12 funzioni: storage chiavi/kv, file, OTA, sysinfo, tempo, reboot
port/linux/      host: OTA = file, kv = directory (per sviluppo e CI)
port/espidf/     NVS, LittleFS, esp_ota_* + rollback, esp_chip_info, SNTP
esp-idf/         componente ESP-IDF + examples/poc-sensor
wasm-apps/alarm  sorgente (wasi-sdk) + Containerfile (platform wasip1/wasm)
deploy/          fleet.yaml, script registry/enrollment, Makefile demo
```

API host WASM (modellate sui nomi Ocre dove possibile): `log`, `temp_read`, `led_set`, `config_get`, `sleep_ms`, `now_ms`.

Formati OCI:
- **App**: immagine "compat" (`FROM scratch`, `COPY alarm.wasm /`, `ENTRYPOINT ["/alarm.wasm"]`, platform `wasip1/wasm`) e layer raw `application/vnd.wasm.content.layer.v1+wasm`; build con `podman build`, push sul registry locale.
- **Firmware**: artifact `oras push` con `artifactType` dedicato e un layer `.bin` per chip; tag per chip (`fw:esp32s3-v2`) referenziato dal template fleet tramite label `chip`.

## Fasi

**F0 – Ambiente locale (Mac)**
- Podman machine con risorse sufficienti, clone di flightctl, `make deploy` (kind), CLI `flightctl login`.
- Registry locale `registry:2` su porta 5000 esposto in LAN.
- Generare enrollment config embedded; estrarre CA, cert e chiave di enrollment e server URL.
- Uscita: UI su :9001, `curl --cert/--key` di enrollment su `:7443` dalla LAN risponde.

**F1 – Core su host (port/linux)**
- Enrollment → `flightctl approve` → cert di gestione → long-poll → status completo.
- Uscita: un "device finto" sul Mac risulta `Online` in UI, status accettato senza 400.

**F2 – Port ESP-IDF su S3**
- WiFi, SNTP (validità dei cert), NVS per chiavi/cert, TLS 1.3 mbedTLS, LittleFS.
- Uscita: S3 enrolled e `Online`; misurare RAM/flash di agent + TLS.

**F3 – Config**
- Inline config → file in LittleFS → l'app rilegge `threshold_c`; reporting `renderedVersion`.
- Uscita: modifica nella fleet applicata entro un ciclo di long-poll.

**F4 – Firmware OTA (`spec.os.image`)**
- Pull manifest → blob in streaming su `esp_ota_write` con verifica sha256 → reboot → conferma app valida solo dopo riconnessione riuscita a Flight Control, altrimenti rollback; report `os.image`/`imageDigest`.
- Uscita: rollout canary (S3 `stage=canary`) poi resto della fleet; v3 rotta torna a v2 e il device risulta Degraded/OutOfDate.

**F5 – App WASM (`spec.applications`, tipo `container`)**
- Pull immagine app, estrazione `.wasm`, WAMR, lifecycle e `status.applications[]` (Running/Error, restarts, imageDigests).
- Uscita: `alarm` v1 → v2 senza riavvio su S3 e C3/C5/C6 con lo stesso digest.

**F6 – Flotta eterogenea e risultati**
- Template con `{{ .metadata.labels.chip }}`; S2/ESP32 solo config+firmware.
- Misure: RAM, flash, traffico idle/ora, tempi di enrollment e update.
- Documento con limiti emersi per proposte upstream (timeout disconnessione per device in sleep, dichiarazione esplicita runtime WASM / classe MCU).

## Verifica end-to-end (in locale)

1. `make deploy` su kind; `flightctl get devices` funziona; registry locale risponde.
2. Device finto host (F1): `flightctl get enrollmentrequests` → approve → `flightctl get device/<name> -o yaml` mostra status completo e `Online`.
3. Flash S3/C3 con enrollment config embedded; approvazione con `-l chip=esp32s3 -l stage=canary -l site=lab`.
4. `flightctl apply -f deploy/fleet.yaml` (config + `os.image` v1 + app `alarm:v1`) → tutti `UpToDate`.
5. Cambio soglia → LED reagisce senza reboot; `status.config.renderedVersion` allineato.
6. Template `os.image` → v2: canary prima, poi batch; v3 rotta → rollback osservabile nello status.
7. App `alarm:v2` → cambio comportamento LED senza reboot su Xtensa e RISC-V.
8. Spegnere un device → `Unknown` dopo 5 min; riaccenderlo → torna `Online` e riconcilia.
9. Test host automatici (port/linux) in CI contro un Flight Control su kind.

## Rischi principali

- TLS 1.3/mTLS su mbedTLS ESP-IDF (supporto o RAM) → fallback: misurare su S3 prima dei chip piccoli.
- Raggiungibilità di kind dalla LAN su macOS → fallback: port-forward (`socat`/ssh) o macchina Linux.
- Long-poll di 4 min tramite NAT/AP domestico → timeout e riconnessione gestiti dal loop.
- Validazione status per valori non Linux → verificare in F1 sull'host prima dell'hardware.
