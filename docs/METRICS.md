# Metrics

Configure the Telemetry Gateway to export device and app metrics to Prometheus. The gateway adds `device_id` and `org_id` to each series.

## Turning it on

The fleet writes the gateway's address in `/etc/fc/metrics.json`, an inline config file:

```yaml
spec:
  template:
    spec:
      config:
        - name: metrics
          inline:
            - path: /etc/fc/metrics.json
              content: '{"endpoint": "telemetry.flightctl.example.com:443", "intervalSeconds": 60}'
```

| Setting | Meaning |
|---|---|
| `endpoint` | the gateway, `host:port` (443 if no port). On OpenShift Flight Control's chart exposes it as `telemetry.<base domain>`; elsewhere it needs a route of its own (on kind, [tools/kind/telemetry-route.yaml](../tools/kind/telemetry-route.yaml)). |
| `intervalSeconds` | how often the device sends, 60 by default, at least 30: each export is a TLS connection |

Remove the file or its `endpoint` to stop exports. The device requests a separate gateway certificate using its device key, signer `flightctl.io/device-svc-client` and CN `otel-<device name>`. Flight Control approves it automatically. It is valid for a year and renewed after one quarter of its lifetime. The device verifies the gateway against its Flight Control CA.

## What is sent

Every interval, one `ExportMetricsServiceRequest` (OTLP over gRPC), with the resource `service.name="flightctl-mcu"` and `service.version` (the firmware's tag). In Prometheus the gateway adds `device_id` and `org_id`, and the names take their unit (`_bytes`, `_total`):

| Metric | Prometheus | Where |
|---|---|---|
| `device.memory.free` (By) | `device_memory_free_bytes` | every port |
| `device.memory.min_free` (By) | `device_memory_min_free_bytes` | ESP-IDF, Zephyr |
| `device.memory.internal.free`, `.largest_block` (By) | `device_memory_internal_free_bytes`... | ESP-IDF reports both. Zephyr with the agent’s C heap also reports the largest block. |
| `device.memory.kernel.free`, `.min_free` (By) | `device_memory_kernel_free_bytes`... | Zephyr |
| `device.network.io` (By, counter), `direction` transmit or receive | `device_network_io_bytes_total` | every port: the agent's TLS traffic |
| `device.tls.handshakes` (counter) | `device_tls_handshakes_total` | every port |
| `device.cpu.utilization` (%) | `device_cpu_utilization_percent` | where the port can tell |
| `device.uptime` (s) | `device_uptime_seconds` | ESP-IDF |
| `device.wifi.rssi` (dBm) | `device_wifi_rssi…` (the unit as the exporter writes it) | ESP-IDF on WiFi |
| `device.temperature` (Cel) | `device_temperature_celsius` | ESP-IDF, chips with a sensor |
| `device.metrics.skipped` (counter) | `device_metrics_skipped_total` | every port: exports not made (below) |
| `device.metrics.export_min_free` (By) | `device_metrics_export_min_free_bytes` | ESP-IDF: the internal heap at its lowest during the last export |
| `app.<app>.<name>` | `app_<app>_<name>` (`_total` for a counter) | an app's `fc_metric`, with `metrics` in its `FC_ALLOW` |

Device counters start at boot. App counters start at their first update. Exports begin after the device clock synchronizes.

![Four ESP-IDF temperature series in Prometheus, one per device_id](img/metrics-prometheus.png)

![Prometheus, an app's metric next to the device's](img/metrics-app.png)
On the S3, compare `app_trend_temp_c` (smoothed) with `device_temperature_celsius` (chip reading).

## Memory

The export reserves one point per active app series, plus 15 slots for device and exporter metrics. On ESP32-C6 the point array takes 2,160 B with no app series and 4,464 B with all 16. A series first published while the snapshot is being collected can appear in the next export.

Each export briefly opens a second TLS connection. It needs a contiguous block of 24 KiB on ESP-IDF and Zephyr builds with the agent's C heap, plus 20 KiB free above the agent's reserve (60 KiB total by default). It waits for downloads, status, spec changes, firmware verification, MQTT handshakes and decommissioning.

A skipped export increments `device.metrics.skipped` and retries after 10 s. The agent logs every tenth memory skip. A gateway request times out after 15 s. Repeated failures back off to one hour. Updating `metrics.json` retries immediately. [Board measurements with apps and MQTT](RESULTS.md#ram)

## Limits

- The first metric an app publishes allocates room for its eight metrics. An installed app that publishes none uses no heap for metrics. A board-map restart preserves the app’s counters. Removal, replacement and parking for primary-app memory release its series.
- The exported name includes the full app name (up to 47 bytes) and metric name (up to 32 bytes). Two publishing apps can export up to 16 metrics per interval.
- A gauge or a counter, as a number (a double); no histograms.
- The device's own metrics are fixed by the firmware.
- Gateway enrollment with a hardware-backed device key is unverified.
