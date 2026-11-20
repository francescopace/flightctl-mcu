"""Encode the provisioning tool's JSON input as an ESP-IDF NVS partition."""
import json
import os
import sys

try:
    from esp_idf_nvs_partition_gen import nvs_partition_gen as nvs
except ModuleNotFoundError:
    # ESP-IDF images that ship the generator as a script.
    sys.path.insert(0, os.path.join(os.environ["IDF_PATH"], "components", "nvs_flash", "nvs_partition_generator"))
    import nvs_partition_gen as nvs


if __name__ == "__main__":
    size, port, values = json.load(sys.stdin)
    # fc_creds is writable. Espressif reserves the last page for compaction.
    page_size = nvs.Page.PAGE_PARAMS["max_size"]
    if size < 3 * page_size or size % page_size:
        sys.exit("invalid writable NVS partition size")
    with nvs.nvs_open(sys.stdout.buffer, size - page_size, nvs.Page.VERSION2) as partition:
        partition.write_namespace("fc")
        for key, value in values.items():
            partition.write_entry(key, value, "string")
        partition.write_entry("server_port", port, "i32")
