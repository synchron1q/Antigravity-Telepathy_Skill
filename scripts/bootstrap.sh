#!/usr/bin/env bash
set -euo pipefail

# Zero-touch one-line bootstrap installer for ag-mesh on Linux & macOS
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/synchron1q/Antigravity-Telepathy_Skill/main/scripts/bootstrap.sh | bash
#   curl -fsSL ... | bash -s -- --join "agmesh://join/v1/..."
#   ./scripts/bootstrap.sh --local-binary ./build/linux-release/ag-mesh --join 10.8.0.1 --pin 482-915

REPO="${AGMESH_REPO:-synchron1q/Antigravity-Telepathy_Skill}"
VERSION="latest"
INSTALL_DIR="${HOME}/.local/bin"
LOCAL_BINARY=""
JOIN_TARGET=""
JOIN_PIN=""
NODE_ID=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --repo)         REPO="$2"; shift 2 ;;
        --version)      VERSION="$2"; shift 2 ;;
        --install-dir)  INSTALL_DIR="$2"; shift 2 ;;
        --local-binary) LOCAL_BINARY="$2"; shift 2 ;;
        --join)         JOIN_TARGET="$2"; shift 2 ;;
        --pin)          JOIN_PIN="$2"; shift 2 ;;
        --node-id)      NODE_ID="$2"; shift 2 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

OS_RAW="$(uname -s)"
ARCH_RAW="$(uname -m)"

case "${OS_RAW}" in
    Linux)  OS_NAME="linux" ;;
    Darwin) OS_NAME="macos" ;;
    *) echo "Unsupported OS: ${OS_RAW}" >&2; exit 1 ;;
esac

case "${ARCH_RAW}" in
    x86_64|amd64)   ARCH_NAME="x86_64" ;;
    arm64|aarch64)  ARCH_NAME="arm64" ;;
    *) echo "Unsupported architecture: ${ARCH_RAW}" >&2; exit 1 ;;
esac

TARGET="${OS_NAME}-${ARCH_NAME}"
mkdir -p "${INSTALL_DIR}"
BIN_PATH="${INSTALL_DIR}/ag-mesh"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

SKILL_SRC=""
if [[ -n "${LOCAL_BINARY}" ]]; then
    if [[ ! -f "${LOCAL_BINARY}" ]]; then
        echo "Local binary not found: ${LOCAL_BINARY}" >&2
        exit 1
    fi
    echo "[1/4] Installing offline binary from ${LOCAL_BINARY} -> ${BIN_PATH}"
    cp "${LOCAL_BINARY}" "${BIN_PATH}"
    chmod 0755 "${BIN_PATH}"
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    if [[ -d "${SCRIPT_DIR}/../skill/antigravity-mesh" ]]; then
        SKILL_SRC="${SCRIPT_DIR}/../skill/antigravity-mesh"
    fi
else
    echo "[1/4] Resolving GitHub Release (${REPO}, version=${VERSION})..."
    if [[ "${VERSION}" == "latest" ]]; then
        TAG="$(curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest" \
            | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -n 1)"
    else
        TAG="${VERSION}"
    fi
    if [[ -z "${TAG}" ]]; then
        echo "Failed to resolve release tag for ${REPO}" >&2
        exit 1
    fi

    ASSET="ag-mesh-${TAG}-${TARGET}.tar.gz"
    BASE_URL="https://github.com/${REPO}/releases/download/${TAG}"
    curl -fsSL "${BASE_URL}/${ASSET}" -o "${TMP_DIR}/${ASSET}"
    curl -fsSL "${BASE_URL}/${ASSET}.sha256" -o "${TMP_DIR}/${ASSET}.sha256"

    EXPECTED_SHA="$(awk '{print $1}' "${TMP_DIR}/${ASSET}.sha256" | tr '[:upper:]' '[:lower:]')"
    if command -v sha256sum >/dev/null 2>&1; then
        ACTUAL_SHA="$(sha256sum "${TMP_DIR}/${ASSET}" | awk '{print $1}' | tr '[:upper:]' '[:lower:]')"
    else
        ACTUAL_SHA="$(shasum -a 256 "${TMP_DIR}/${ASSET}" | awk '{print $1}' | tr '[:upper:]' '[:lower:]')"
    fi
    if [[ "${EXPECTED_SHA}" != "${ACTUAL_SHA}" ]]; then
        echo "SHA-256 verification failed! Expected=${EXPECTED_SHA} Actual=${ACTUAL_SHA}" >&2
        exit 1
    fi
    echo "SHA-256 verified: ${ACTUAL_SHA}"

    tar -xzf "${TMP_DIR}/${ASSET}" -C "${TMP_DIR}"
    PKG_DIR="${TMP_DIR}/ag-mesh-${TAG}-${TARGET}"
    cp "${PKG_DIR}/ag-mesh" "${BIN_PATH}"
    chmod 0755 "${BIN_PATH}"
    if [[ -d "${PKG_DIR}/skill/antigravity-mesh" ]]; then
        SKILL_SRC="${PKG_DIR}/skill/antigravity-mesh"
    fi
fi

echo "[2/4] Installing Antigravity skill..."
AG_DIR="${ANTIGRAVITY_HOME:-${HOME}/.gemini/antigravity}"
SKILL_DST="${AG_DIR}/skills/antigravity-mesh"
mkdir -p "${SKILL_DST}"
if [[ -n "${SKILL_SRC}" && -d "${SKILL_SRC}" ]]; then
    cp -R "${SKILL_SRC}/." "${SKILL_DST}/"
fi

echo "[3/4] Running ag-mesh init --auto..."
INIT_CMD=("${BIN_PATH}" init --auto)
if [[ -n "${NODE_ID}" ]]; then
    INIT_CMD+=(--node-id "${NODE_ID}")
fi
"${INIT_CMD[@]}"

if [[ -n "${JOIN_TARGET}" ]]; then
    echo "[4/4] Joining cluster..."
    JOIN_CMD=("${BIN_PATH}" join "${JOIN_TARGET}")
    if [[ -n "${JOIN_PIN}" ]]; then
        JOIN_CMD+=(--pin "${JOIN_PIN}")
    fi
    "${JOIN_CMD[@]}"
else
    echo "[4/4] Done! Run '${BIN_PATH} pair' to invite peers or '${BIN_PATH} join <token>' to join a cluster."
fi
