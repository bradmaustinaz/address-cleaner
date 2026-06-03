#!/bin/bash
set -euo pipefail

# Only needed in the Claude Code on the web remote environment; on a developer's
# own machine the MinGW toolchain is already set up.
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

# Address Cleaner is a Win32 GUI app. On Linux we cross-compile it with the
# MinGW-w64 toolchain (the Makefile auto-selects the x86_64-w64-mingw32-* tools
# when not running on Windows). Install it plus make if it isn't present yet.
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
  export DEBIAN_FRONTEND=noninteractive
  # Unrelated third-party PPAs baked into the base image can make `apt-get
  # update` exit non-zero, but the main Ubuntu repos (which provide mingw-w64)
  # still refresh, so don't let that abort the hook.
  apt-get update -qq || true
  apt-get install -y -qq mingw-w64 make
fi
