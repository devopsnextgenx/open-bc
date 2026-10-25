#!/bin/bash
# Registers the runner, runs it, and deregisters on container stop.
set -euo pipefail

: "${REPO_URL:?REPO_URL must be provided}"
: "${REG_TOKEN:?REG_TOKEN must be provided}"

RUNNER_NAME="${RUNNER_NAME:-$(hostname)}"
RUNNER_LABELS="${RUNNER_LABELS:-}"

cd "$(dirname "$0")"

# A restarted container keeps its old config; clear it so config.sh does not refuse.
rm -f .runner .credentials .credentials_rsaparams

echo "Configuring runner '$RUNNER_NAME' for $REPO_URL (labels: ${RUNNER_LABELS:-none})..."
./config.sh --url "$REPO_URL" --token "$REG_TOKEN" \
    --name "$RUNNER_NAME" ${RUNNER_LABELS:+--labels "$RUNNER_LABELS"} \
    --unattended --replace

CLEANED=0
cleanup() {
    [ "$CLEANED" -eq 1 ] && return
    CLEANED=1
    echo "Removing runner from GitHub..."
    ./config.sh remove --token "$REG_TOKEN" \
        || echo "Warning: could not deregister (token expired?). Delete it under Settings > Actions > Runners."
}

# Forward stop signals to the runner so it can finish cleanly, then deregister.
on_signal() {
    kill -TERM "$RUN_PID" 2>/dev/null || true
    wait "$RUN_PID" 2>/dev/null || true
    cleanup
    exit 0
}
trap on_signal INT TERM

echo "Starting the GitHub Actions runner..."
./run.sh &
RUN_PID=$!
status=0
wait "$RUN_PID" || status=$?
cleanup
exit "$status"