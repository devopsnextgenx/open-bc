#!/bin/bash
set -euo pipefail
cargo xtask setup && cargo xtask build && cargo xtask run