#!/usr/bin/env bash
set -euo pipefail

# Web parity smoke for ESP32 HTTP API.
#
# Default mode is READ-ONLY. To run mutating checks, set RUN_MUTATING=1.
# Example:
#   BASE_URL=http://192.168.4.1 TOKEN=<admin-token> RUN_MUTATING=1 \
#     Firmware/ESP32/tools/web_parity_smoke.sh

BASE_URL="${BASE_URL:-http://192.168.4.1}"
TOKEN="${TOKEN:-}"
RUN_MUTATING="${RUN_MUTATING:-0}"
CH="${CH:-0}"
BRIDGE_ID="${BRIDGE_ID:-0}"

if [[ -z "$TOKEN" ]]; then
  echo "ERROR: TOKEN is required (X-BugBuster-Admin-Token)"
  exit 2
fi

if ! command -v curl >/dev/null 2>&1; then
  echo "ERROR: curl not found"
  exit 2
fi
if ! command -v jq >/dev/null 2>&1; then
  echo "ERROR: jq not found"
  exit 2
fi

HDR=(-H "Content-Type: application/json" -H "X-BugBuster-Admin-Token: $TOKEN")

log() {
  printf "\n[%s] %s\n" "$(date +%H:%M:%S)" "$*"
}

get_json() {
  local path="$1"
  curl -fsS "$BASE_URL$path" | jq . >/dev/null
}

post_json() {
  local path="$1"
  local body="$2"
  curl -fsS -X POST "$BASE_URL$path" "${HDR[@]}" -d "$body" | jq . >/dev/null
}

log "Read-only baseline checks"
get_json "/api/device/info"
get_json "/api/status"
get_json "/api/diagnostics"
get_json "/api/faults"
get_json "/api/uart/config"
get_json "/api/uart/pins"
get_json "/api/usbpd"
get_json "/api/ioexp"
get_json "/api/ioexp/faults"
get_json "/api/hat"
get_json "/api/hat/la/status"
get_json "/api/board"
get_json "/api/debug"
get_json "/api/channel/$CH"

echo "Read-only API checks: OK"

if [[ "$RUN_MUTATING" != "1" ]]; then
  echo "RUN_MUTATING!=1, mutating checks skipped."
  exit 0
fi

log "Mutating parity checks (reversible where possible)"

# Overview parity
post_json "/api/channel/$CH/function" '{"function":3}'

# Analog parity
post_json "/api/channel/$CH/adc/config" '{"mux":0,"range":0,"rate":8}'
post_json "/api/channel/$CH/vout/range" '{"bipolar":false}'
post_json "/api/channel/$CH/dac" '{"voltage":1.0,"bipolar":false}'

# Digital parity
post_json "/api/channel/$CH/din/config" '{"thresh":64,"debounce":1,"ocDet":false,"scDet":false}'
post_json "/api/channel/$CH/do/config" '{"mode":1,"srcSelGpio":false,"t1":0,"t2":0}'
post_json "/api/channel/$CH/do/set" '{"on":false}'

# Scope/Wavegen parity
post_json "/api/wavegen/start" "{\"channel\":$CH,\"waveform\":0,\"mode\":0,\"freq_hz\":10,\"amplitude\":1.0,\"offset\":0.0}"
post_json "/api/wavegen/stop" '{}'

# System parity
post_json "/api/usbpd/caps" '{}'
post_json "/api/usbpd/select" '{"voltage":5}'
post_json "/api/uart/$BRIDGE_ID/config" '{"uartNum":1,"txPin":17,"rxPin":18,"baudrate":115200,"dataBits":8,"parity":0,"stopBits":0,"enabled":true}'
post_json "/api/ioexp/control" '{"control":"vadj1","on":false}'
post_json "/api/ioexp/control" '{"control":"vadj1","on":true}'
post_json "/api/ioexp/fault_config" '{"auto_disable":true,"log_events":true}'
post_json "/api/faults/clear" '{}'
post_json "/api/faults/clear/0" '{}'
post_json "/api/faults/mask" '{"alertMask":65535,"supplyMask":65535}'
post_json "/api/faults/mask/0" '{"mask":65535}'
post_json "/api/hat/detect" '{}'
post_json "/api/hat/reset" '{}'
post_json "/api/hat/config" '{"pins":[0,0,0,0]}'

# Board profile select only if at least one profile exists
if curl -fsS "$BASE_URL/api/board" | jq -e '.available | length > 0' >/dev/null; then
  FIRST_BOARD_ID="$(curl -fsS "$BASE_URL/api/board" | jq -r '.available[0].id')"
  post_json "/api/board/select" "{\"boardId\":\"$FIRST_BOARD_ID\"}"
fi

echo "Mutating parity checks: OK"
