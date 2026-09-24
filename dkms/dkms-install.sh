#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

#
# piBrick DKMS installer
#

if [[ "${EUID}" -ne 0 ]]; then
	if ! command -v sudo >/dev/null 2>&1; then
		echo "ERROR: sudo is required to run this installer." >&2
		exit 1
	fi

	echo "[piBrick DKMS] Re-running installer with sudo..."
	exec sudo -E bash "$0" "$@"
fi


SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

PACKAGE_NAME="pibrick-drivers"
PACKAGE_VERSION="${PIBRICK_DKMS_VERSION:-1.0.0}"

DKMS_SOURCE_DIR="/usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}"
DKMS_CONF="${SCRIPT_DIR}/dkms.conf"

KERNEL_VERSION="$(uname -r)"
KERNEL_BUILD_DIR="/lib/modules/${KERNEL_VERSION}/build"


echo "[piBrick DKMS] Package      : ${PACKAGE_NAME}"
echo "[piBrick DKMS] Version      : ${PACKAGE_VERSION}"
echo "[piBrick DKMS] Kernel       : ${KERNEL_VERSION}"
echo "[piBrick DKMS] Source       : ${REPO_ROOT}"
echo "[piBrick DKMS] DKMS source  : ${DKMS_SOURCE_DIR}"
echo


#
# Check / install DKMS
#

if ! command -v dkms >/dev/null 2>&1; then
	echo "[piBrick DKMS] DKMS is not installed."

	if ! command -v apt-get >/dev/null 2>&1; then
		echo "ERROR: apt-get is not available." >&2
		exit 1
	fi

	echo "[piBrick DKMS] Installing DKMS..."
	apt-get update
	apt-get install -y dkms
fi


#
# Check / install make
#

if ! command -v make >/dev/null 2>&1; then
	echo "[piBrick DKMS] Installing make..."

	apt-get update
	apt-get install -y make
fi


#
# Check kernel headers
#

if [[ ! -f "${KERNEL_BUILD_DIR}/Makefile" ]]; then
	echo "[piBrick DKMS] Kernel headers are missing."

	if ! command -v apt-get >/dev/null 2>&1; then
		echo "ERROR: apt-get is not available." >&2
		exit 1
	fi

	apt-get update

	if apt-cache show raspberrypi-kernel-headers >/dev/null 2>&1; then
		apt-get install -y raspberrypi-kernel-headers
	else
		apt-get install -y "linux-headers-${KERNEL_VERSION}"
	fi
fi


#
# Validate required files
#

if [[ ! -f "${DKMS_CONF}" ]]; then
	echo "ERROR: DKMS configuration not found:" >&2
	echo "       ${DKMS_CONF}" >&2
	exit 1
fi

if [[ ! -f "${REPO_ROOT}/Makefile" ]]; then
	echo "ERROR: Root Makefile not found:" >&2
	echo "       ${REPO_ROOT}/Makefile" >&2
	exit 1
fi

if [[ ! -f "${REPO_ROOT}/touch/hyn_ts/Makefile" ]]; then
	echo "ERROR: Hynitron Makefile not found:" >&2
	echo "       ${REPO_ROOT}/touch/hyn_ts/Makefile" >&2
	exit 1
fi

if [[ ! -f "${KERNEL_BUILD_DIR}/Makefile" ]]; then
	echo "ERROR: Kernel build directory is unavailable:" >&2
	echo "       ${KERNEL_BUILD_DIR}" >&2
	exit 1
fi


#
# Remove previous DKMS registration
#

echo "[piBrick DKMS] Removing previous registration..."

dkms remove \
	-m "${PACKAGE_NAME}" \
	-v "${PACKAGE_VERSION}" \
	--all || true


#
# Prepare /usr/src package
#

echo "[piBrick DKMS] Preparing source tree..."

rm -rf "${DKMS_SOURCE_DIR}"
mkdir -p "${DKMS_SOURCE_DIR}"

#
# Root build system
#

cp "${REPO_ROOT}/Makefile" \
	"${DKMS_SOURCE_DIR}/Makefile"

if [[ -f "${REPO_ROOT}/Kconfig" ]]; then
	cp "${REPO_ROOT}/Kconfig" \
		"${DKMS_SOURCE_DIR}/Kconfig"
fi


#
# Kernel driver sources
#

cp -a "${REPO_ROOT}/panel" \
	"${DKMS_SOURCE_DIR}/"

cp -a "${REPO_ROOT}/power" \
	"${DKMS_SOURCE_DIR}/"

cp -a "${REPO_ROOT}/touch" \
	"${DKMS_SOURCE_DIR}/"


#
# DKMS configuration must be at package root
#

cp "${DKMS_CONF}" \
	"${DKMS_SOURCE_DIR}/dkms.conf"


#
# Register with DKMS
#

echo "[piBrick DKMS] Adding DKMS package..."

dkms add \
	-m "${PACKAGE_NAME}" \
	-v "${PACKAGE_VERSION}"


#
# Build
#

echo "[piBrick DKMS] Building kernel modules..."

dkms build \
	-m "${PACKAGE_NAME}" \
	-v "${PACKAGE_VERSION}" \
	-k "${KERNEL_VERSION}"


#
# Install
#

echo "[piBrick DKMS] Installing kernel modules..."

dkms install \
	-m "${PACKAGE_NAME}" \
	-v "${PACKAGE_VERSION}" \
	-k "${KERNEL_VERSION}"


#
# Refresh module dependency database
#

depmod -a "${KERNEL_VERSION}"


#
# Show result
#

echo
echo "[piBrick DKMS] DKMS status:"
dkms status \
	-m "${PACKAGE_NAME}" \
	-v "${PACKAGE_VERSION}" || true


echo
echo "[piBrick DKMS] Installed modules:"

for module in \
	panel-pibrick \
	pibrick-battery \
	pibrick-charger \
	hyn_ts
do
	echo
	echo "----- ${module} -----"
	modinfo "${module}" 2>/dev/null || \
		echo "Module information not available."
done


echo
echo "[piBrick DKMS] Installation complete."
