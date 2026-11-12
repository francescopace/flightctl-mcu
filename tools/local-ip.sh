# Sourced by the deploy scripts: prints this machine's LAN IP.
local_ip() {
  if command -v ipconfig >/dev/null 2>&1 && ipconfig getifaddr en0 >/dev/null 2>&1; then
    ipconfig getifaddr en0          # macOS
  else
    hostname -I | awk '{print $1}'  # Linux
  fi
}
