#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin logs

if ! command -v gcc >/dev/null 2>&1 || ! command -v make >/dev/null 2>&1; then
  echo "gcc and make are required. Attempting to install Ubuntu build dependencies..."
  if command -v apt-get >/dev/null 2>&1 && command -v sudo >/dev/null 2>&1; then
    sudo apt-get update
    sudo apt-get install -y build-essential coreutils
  else
    echo "Install build-essential and coreutils, then run this script again."
    exit 1
  fi
fi

make all
exec ./bin/supervisor -c config/services.conf "$@"
