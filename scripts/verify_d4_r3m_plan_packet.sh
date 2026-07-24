#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

D4_R3M_PLAN_PACKET_STRICT=1 \
  exec "$ROOT/scripts/verify_v1_contract.sh"
