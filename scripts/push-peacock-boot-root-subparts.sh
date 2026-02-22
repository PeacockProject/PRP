#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMG="${1:-$HOME/.local/var/peacock/samsung-jflte.img}"

"$SCRIPT_DIR/push-peacock-boot-subpart.sh" "$IMG"
"$SCRIPT_DIR/push-peacock-root-subpart.sh" "$IMG"
