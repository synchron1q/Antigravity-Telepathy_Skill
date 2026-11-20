#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY_PATH="${1:-$(command -v ag-mesh || echo ag-mesh)}"
CONFIG_PATH="${2:-}"

if command -v python3 >/dev/null 2>&1; then
    exec python3 "${SCRIPT_DIR}/install_mcp.py" "${BINARY_PATH}" "${CONFIG_PATH}"
fi

AG_DIR="${ANTIGRAVITY_HOME:-${HOME}/.gemini/antigravity}"
SKILL_SRC="$(cd "${SCRIPT_DIR}/.." && pwd)"
SKILL_DST="${AG_DIR}/skills/antigravity-mesh"
mkdir -p "${SKILL_DST}"
cp -R "${SKILL_SRC}/." "${SKILL_DST}/"

MCP_FILE="${AG_DIR}/mcp_config.json"
if [[ ! -f "${MCP_FILE}" ]]; then
    cat > "${MCP_FILE}" <<EOF
{
  "mcpServers": {
    "ag-mesh": {
      "command": "${BINARY_PATH}",
      "args": ["mcp"]
    }
  }
}
EOF
fi
echo "Installed skill to ${SKILL_DST} and checked ${MCP_FILE}"
