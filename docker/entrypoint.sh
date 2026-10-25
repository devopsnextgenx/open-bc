#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e

# Check if required environment variables are passed
if [ -z "$REPO_URL" ] || [ -z "$REG_TOKEN" ]; then
  echo "Error: REPO_URL and REG_TOKEN must be provided."
  exit 1
fi

echo "Configuring runner for repository: $REPO_URL..."

# Configure the runner
./config.sh --url "$REPO_URL" --token "$REG_TOKEN" --unattended --replace

# Cleanup configuration on container exit
cleanup() {
    echo "Removing runner from GitHub..."
    ./config.sh remove --token "$REG_TOKEN"
}
trap cleanup EXIT

echo "Starting the GitHub Actions runner..."
./run.sh