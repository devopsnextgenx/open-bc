#!/usr/bin/env bash

# Setup arrays for iterated path creation
levels=("level1_A/level2_1/level3" "level1_A/level2_2" "level1_B/level2_1")

# --- 1. Identical Content Files (Both Sides) ---
for path in "${levels[@]}"; do
  # Standard JSON
  cat <<'EOF' > "source/${path}/config.json"
{"app": "test", "version": "1.0.0", "enabled": true}
EOF
  cp "source/${path}/config.json" "target/${path}/config.json"

  # Standard YAML
  cat <<'EOF' > "source/${path}/settings.yml"
server:
  host: 127.0.0.1
  port: 8080
EOF
  cp "source/${path}/settings.yml" "target/${path}/settings.yml"

  # Executable Shell Script
  cat <<'EOF' > "source/${path}/run.sh"
#!/usr/bin/env bash
echo "Executing test script"
EOF
  chmod +x "source/${path}/run.sh"
  cp -p "source/${path}/run.sh" "target/${path}/run.sh"
done

# --- 2. Files with Content Differences ---
# JSON content difference
cat <<'EOF' > source/level1_A/level2_1/data.json
{"status": "pending", "retries": 3}
EOF
cat <<'EOF' > target/level1_A/level2_1/data.json
{"status": "active", "retries": 5}
EOF

# YAML content difference
cat <<'EOF' > source/level1_B/level2_2/deploy.yml
environment: staging
replicas: 2
EOF
mkdir -p target/level1_B/level2_2
cat <<'EOF' > target/level1_B/level2_2/deploy.yml
environment: production
replicas: 5
EOF

# Shell script content difference
cat <<'EOF' > source/level1_A/level2_1/level3/build.sh
#!/usr/bin/env bash
BUILD_ENV="local"
EOF
cat <<'EOF' > target/level1_A/level2_1/level3/build.sh
#!/usr/bin/env bash
BUILD_ENV="ci"
EOF

# --- 3. Unique Files (Only in Source or Target) ---
# Unique to Source
cat <<'EOF' > source/level1_A/source_only.json
{"description": "This file exists only in source"}
EOF

cat <<'EOF' > source/level1_B/level2_1/level3/source_script.sh
#!/usr/bin/env bash
echo "Source only script"
EOF

# Unique to Target
cat <<'EOF' > target/level1_B/target_only.yml
meta:
  side: target
EOF

cat <<'EOF' > target/level1_C/level2_1/extra_config.json
{"description": "Inside extra target folder"}
EOF

echo "Test directory structure and files generated successfully."