#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEMPLATE_FILE="${ROOT_DIR}/CMakeUserPresetsTemplate.json"
OUTPUT_FILE="${ROOT_DIR}/CMakeUserPresets.json"

# Determine VCPKG_ROOT
if [ -n "${VCPKG_ROOT:-}" ]; then
    VCPKG_PATH="${VCPKG_ROOT}"
elif [ -d "${HOME}/vcpkg" ]; then
    VCPKG_PATH="${HOME}/vcpkg"
elif [ -d "${ROOT_DIR}/vcpkg" ]; then
    VCPKG_PATH="${ROOT_DIR}/vcpkg"
else
    echo "VCPKG_ROOT not found. Please set VCPKG_ROOT environment variable or install vcpkg in ~/vcpkg"
    read -r -p "Enter vcpkg path (or press Enter to use ~/vcpkg): " VCPKG_PATH
    VCPKG_PATH="${VCPKG_PATH:-${HOME}/vcpkg}"
fi

# Expand ~ to home directory
VCPKG_PATH="${VCPKG_PATH/#\~/${HOME}}"

# Check if template exists
if [ ! -f "${TEMPLATE_FILE}" ]; then
    echo "Error: Template file not found: ${TEMPLATE_FILE}" >&2
    exit 1
fi

# Generate CMakeUserPresets.json from template
sed "s|\${VCPKG_ROOT}|${VCPKG_PATH}|g" "${TEMPLATE_FILE}" > "${OUTPUT_FILE}"

echo "Generated ${OUTPUT_FILE}"
echo "VCPKG_ROOT: ${VCPKG_PATH}"
