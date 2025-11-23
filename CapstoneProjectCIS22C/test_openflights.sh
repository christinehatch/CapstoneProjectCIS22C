#!/usr/bin/env bash
set -euo pipefail

BASE="http://localhost:8080"

echo "=================================================="
echo " OpenFlights Server Test Harness"
echo "=================================================="
echo

fail() {
  echo "❌ $1"
  exit 1
}

pass() {
  echo "✅ $1"
}

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    fail "Required command '$1' not found. Please install it."
  fi
}

require_cmd curl
require_cmd jq

# ------------------------
# 1. /json sanity check
# ------------------------
echo "[1] Testing /json ..."
JSON_RESP=$(curl -s -f "$BASE/json") || fail "/json did not respond with 2xx"

STATUS=$(echo "$JSON_RESP" | jq -r '.status // empty')
if [[ "$STATUS" != "ok" ]]; then
  echo "$JSON_RESP" | jq .
  fail "/json did not return {\"status\":\"ok\"}"
fi
pass "/json looks good"

# ------------------------
# 2. /onehop SFO -> JFK
# ------------------------
echo
echo "[2] Testing /onehop?src=SFO&dst=JFK&limit=5&recommended=true ..."
ONEHOP_RESP=$(curl -s -f "$BASE/onehop?src=SFO&dst=JFK&limit=5&recommended=true") \
  || fail "/onehop did not respond with 2xx"

SRC=$(echo "$ONEHOP_RESP" | jq -r '.source.iata // empty')
DST=$(echo "$ONEHOP_RESP" | jq -r '.destination.iata // empty')
COUNT=$(echo "$ONEHOP_RESP" | jq '.one_hop_routes | length')

if [[ "$SRC" != "SFO" || "$DST" != "JFK" ]]; then
  echo "$ONEHOP_RESP" | jq .
  fail "/onehop source/destination mismatch (expected SFO->JFK, got $SRC->$DST)"
fi

if (( COUNT < 1 )); then
  echo "$ONEHOP_RESP" | jq .
  fail "/onehop returned zero routes"
fi

pass "/onehop SFO->JFK returned $COUNT routes and correct src/dst"

# ------------------------
# 3. /direct SFO -> JFK
# ------------------------
echo
echo "[3] Testing /direct?src=SFO&dst=JFK ..."
DIRECT_RESP=$(curl -s -f "$BASE/direct?src=SFO&dst=JFK") \
  || fail "/direct did not respond with 2xx"

SRC=$(echo "$DIRECT_RESP" | jq -r '.source.iata // empty')
DST=$(echo "$DIRECT_RESP" | jq -r '.destination.iata // empty')
COUNT=$(echo "$DIRECT_RESP" | jq '.direct_flights | length')

if [[ "$SRC" != "SFO" || "$DST" != "JFK" ]]; then
  echo "$DIRECT_RESP" | jq .
  fail "/direct source/destination mismatch (expected SFO->JFK, got $SRC->$DST)"
fi

echo "  direct_flights count: $COUNT"
# it's okay if there are 0 nonstop flights, so we don't fail here
pass "/direct SFO->JFK responded with correct src/dst"

# ------------------------
# 4. /airline/routes?code=AA
# ------------------------
echo
echo "[4] Testing /airline/routes?code=AA ..."
AIRLINE_RESP=$(curl -s -f "$BASE/airline/routes?code=AA") \
  || fail "/airline/routes?code=AA did not respond with 2xx"

AIR_IATA=$(echo "$AIRLINE_RESP" | jq -r '.airline.iata // empty')
AP_COUNT=$(echo "$AIRLINE_RESP" | jq '.airports | length')

if [[ "$AIR_IATA" != "AA" ]]; then
  echo "$AIRLINE_RESP" | jq .
  fail "/airline/routes returned wrong airline (expected AA, got $AIR_IATA)"
fi

if (( AP_COUNT < 1 )); then
  echo "$AIRLINE_RESP" | jq .
  fail "/airline/routes?code=AA returned zero airports"
fi

pass "/airline/routes?code=AA returned $AP_COUNT airports and correct airline"

# ------------------------
# 5. /airport/routes?code=LGA
# ------------------------
echo
echo "[5] Testing /airport/routes?code=LGA ..."
AIRPORT_RESP=$(curl -s -f "$BASE/airport/routes?code=LGA") \
  || fail "/airport/routes?code=LGA did not respond with 2xx"

AP_IATA=$(echo "$AIRPORT_RESP" | jq -r '.airport.iata // empty')
AL_COUNT=$(echo "$AIRPORT_RESP" | jq '.airlines | length')

if [[ "$AP_IATA" != "LGA" ]]; then
  echo "$AIRPORT_RESP" | jq .
  fail "/airport/routes returned wrong airport (expected LGA, got $AP_IATA)"
fi

if (( AL_COUNT < 1 )); then
  echo "$AIRPORT_RESP" | jq .
  fail "/airport/routes?code=LGA returned zero airlines"
fi

pass "/airport/routes?code=LGA returned $AL_COUNT airlines and correct airport"

# ------------------------
# 6. /airports/search?q=San+Jose&limit=5
# ------------------------
echo
echo "[6] Testing /airports/search?q=San+Jose&limit=5 ..."
SEARCH_RESP=$(curl -s -f "$BASE/airports/search?q=San+Jose&limit=5") \
  || fail "/airports/search did not respond with 2xx"

SEARCH_LEN=$(echo "$SEARCH_RESP" | jq 'length')
if (( SEARCH_LEN < 1 )); then
  echo "$SEARCH_RESP" | jq .
  fail "/airports/search returned no results for 'San Jose'"
fi

pass "/airports/search returned $SEARCH_LEN results for 'San Jose'"

# ------------------------
# 7. /airlines?limit=5&offset=0
# ------------------------
echo
echo "[7] Testing /airlines?limit=5&offset=0 ..."
AIRLINES_RESP=$(curl -s -f "$BASE/airlines?limit=5&offset=0") \
  || fail "/airlines did not respond with 2xx"

LEN=$(echo "$AIRLINES_RESP" | jq 'length')
if (( LEN < 1 )); then
  echo "$AIRLINES_RESP" | jq .
  fail "/airlines?limit=5&offset=0 returned no airlines"
fi

pass "/airlines?limit=5&offset=0 returned $LEN airlines"

echo
echo "=================================================="
echo "✅ All tests completed"
echo "=================================================="