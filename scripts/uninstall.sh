#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# piBrick CM5 Drivers - Uninstaller
#

set -euo pipefail

PACKAGE_NAME="pibrick-drivers"
PACKAGE_VERSION="1.0.0"

DKMS_NAME="${PACKAGE_NAME}"
DKMS_VERSION="${PACKAGE_VERSION}"
DKMS_SOURCE_DIR="/usr/src/${DKMS_NAME}-${DKMS_VERSION}"

BOOT_FIRMWARE_DIR="/boot/firmware"
OVERLAY_FILE="${BOOT_FIRMWARE_DIR}/overlays/pibrick.dtbo"

require_root()
{
    if [[ "${EUID}" -ne 0 ]]; then
        echo "This uninstaller requires root privileges."
        echo "Requesting sudo..."
        exec sudo -E bash "$0" "$@"
    fi
}

info()
{
    echo
    echo "==> $*"
}

stop_button_service()
{
    info "Stopping pibrick-button-service"

    systemctl disable \
        --now \
        pibrick-button-service.service \
        2>/dev/null || true

    rm -f \
        /etc/systemd/system/pibrick-button-service.service

    rm -rf \
        /etc/pibrick

    systemctl daemon-reload
}

remove_userspace()
{
    info "Removing piBrick userspace applications"

    rm -f \
        /usr/local/bin/pibrick-button-service \
        /usr/local/bin/pibrick-kbd
}

remove_dkms()
{
    info "Removing piBrick DKMS modules"

    if command -v dkms >/dev/null 2>&1; then
        if dkms status "${DKMS_NAME}/${DKMS_VERSION}" \
            2>/dev/null | grep -q "${DKMS_NAME}/${DKMS_VERSION}"
        then
            dkms remove \
                "${DKMS_NAME}/${DKMS_VERSION}" \
                --all
        else
            echo "No piBrick DKMS registration found."
        fi
    else
        echo "DKMS is not installed."
    fi

    rm -rf "${DKMS_SOURCE_DIR}"

    depmod -a
}

remove_dtbo()
{
    info "Removing piBrick device-tree overlay"

    rm -f "${OVERLAY_FILE}"
}

show_status()
{
    echo
    echo "========================================"
    echo " piBrick CM5 Driver Uninstall Complete"
    echo "========================================"
    echo
    echo "Removed:"
    echo "  - DKMS kernel modules"
    echo "  - /usr/src/${DKMS_NAME}-${DKMS_VERSION}"
    echo "  - pibrick.dtbo"
    echo "  - pibrick-button-service"
    echo "  - pibrick-kbd"
    echo
    echo "The source tree was not modified."
    echo
    echo "Reboot is recommended."
}

remove_config()
{
    local config="/boot/firmware/config.txt"

    if [[ ! -f "${config}" ]]; then
        info "config.txt not found, skipping overlay configuration removal"
        return
    fi

    if grep -qE '^[[:space:]]*dtoverlay=pibrick([[:space:]]|$)' "${config}"; then
        info "Removing piBrick device-tree overlay from config.txt"

        sed -i \
            '/^[[:space:]]*dtoverlay=pibrick[[:space:]]*$/d' \
            "${config}"

        # Remove the comment added by install_config(), but only
        # when it is immediately associated with the piBrick overlay.
        sed -i \
            '/^[[:space:]]*#[[:space:]]*piBrick CM5 drivers[[:space:]]*$/d' \
            "${config}"
    else
        info "piBrick device-tree overlay is not enabled in config.txt"
    fi
}


main()
{
    require_root "$@"

    echo "========================================"
    echo " piBrick CM5 Driver Uninstaller"
    echo "========================================"
    echo
    echo "Version : ${PACKAGE_VERSION}"

    stop_button_service
    remove_userspace
    remove_dkms
    remove_dtbo
    remove_config

    rm -rf "${DKMS_SOURCE_DIR}"
    depmod -a

    show_status
}

main "$@"
