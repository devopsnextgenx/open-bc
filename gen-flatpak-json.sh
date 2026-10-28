#!/bin/sh

python  flatpak-cargo-generator.py Cargo.lock -o cargo-sources.json
python  flatpak-cargo-generator.py Cargo.lock -o ./scripts/packaging/cargo-sources.json