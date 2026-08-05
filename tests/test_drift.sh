#!/usr/bin/env bash
# Verifies codegen output fails to compile when the C header drifts
# (field renamed) — proving the spec's compile-time drift safety.
set -euo pipefail
cd "$(dirname "$0")/.."

# Generate the registry code from the schema.
python3 tools/gen_registry.py tests/drift_schema.json tests/drift_gen.cpp

# A main that includes the drift header + the generated registry, and calls it.
cat > tests/drift_main.cpp <<'EOF'
#include "drift_header.h"
#include <struct_view/NameRegistry.hpp>
#include "drift_gen.cpp"
int main() { sv::NameRegistry r; registerStructViewNames(r); return r.lookup("time") ? 0 : 1; }
EOF

echo "=== Step A: baseline compiles ==="
if g++ -std=c++17 -Iinclude -Ithird_party tests/drift_main.cpp src/*.cpp -o /tmp/drift_ok 2>/dev/null; then
  echo "baseline OK"
else
  echo "ERROR: baseline should compile but did not"
  g++ -std=c++17 -Iinclude -Ithird_party tests/drift_main.cpp src/*.cpp -o /tmp/drift_ok 2>&1 | tail -15
  exit 1
fi

echo "=== Step B: rename field -> compile MUST fail ==="
cat > tests/drift_header.h <<'EOF'
#pragma once
#include <stdint.h>
typedef struct { uint64_t renamed; } DriftEvent;
EOF
if g++ -std=c++17 -Iinclude -Ithird_party tests/drift_main.cpp src/*.cpp -o /tmp/drift_bad 2>/dev/null; then
  echo "ERROR: rename should have failed the build (drift not caught)"
  exit 1
else
  echo "drift caught at compile (expected)"
fi

# Restore the baseline header so the repo is left clean.
cat > tests/drift_header.h <<'EOF'
#pragma once
#include <stdint.h>
typedef struct { uint64_t timestamp; } DriftEvent;
EOF
echo "ALL OK"
