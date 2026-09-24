#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# piBrick CM5 Drivers - Installer
#

set -euo pipefail

PACKAGE_NAME="pibrick-drivers"
PACKAGE_VERSION="1.0.0"
DKMS_NAME="${PACKAGE_NAME}"
DKMS_VERSION="${PACKAGE_VERSION}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

KERNEL_VERSION="$(uname -r)"
KERNEL_BUILD_DIR="/lib/modules/${KERNEL_VERSION}/build"

DKMS_SOURCE_DIR="/usr/src/${DKMS_NAME}-${DKMS_VERSION}"

BOOT_FIRMWARE_DIR="/boot/firmware"
OVERLAY_DIR="${BOOT_FIRMWARE_DIR}/overlays"
OVERLAY_FILE="${OVERLAY_DIR}/pibrick.dtbo"

BUTTON_APP_DIR="${ROOT_DIR}/app/pibrick-button-service"
KEYBOARD_APP_DIR="${ROOT_DIR}/app/pibrick-kbd"

die()
{
    echo
    echo "Error: $*" >&2
    exit 1
}

info()
{
    echo
    echo "==> $*"
}

require_root()
{
    if [[ "${EUID}" -ne 0 ]]; then
        echo "This installer requires root privileges."
        echo "Requesting sudo..."
        exec sudo -E bash "$0" "$@"
    fi
}

check_source_tree()
{
    local required

    for required in \
        "${ROOT_DIR}/Makefile" \
        "${ROOT_DIR}/dkms/dkms.conf" \
        "${ROOT_DIR}/panel" \
        "${ROOT_DIR}/power" \
        "${ROOT_DIR}/touch/hyn_ts" \
        "${ROOT_DIR}/dts/pibrick.dts"
    do
        [[ -e "${required}" ]] || \
            die "Required source is missing: ${required}"
    done
}

install_dependencies()
{
    info "Installing build dependencies"

    export DEBIAN_FRONTEND=noninteractive

    apt-get update

    local header_package

    case "${KERNEL_VERSION}" in
        *rpt-rpi-v8)
            header_package="linux-headers-rpi-v8"
            ;;
        *rpt-rpi-v7)
            header_package="linux-headers-rpi-v7"
            ;;
        *rpt-rpi-v7l)
            header_package="linux-headers-rpi-v7l"
            ;;
        *rpt-rpi-v6)
            header_package="linux-headers-rpi-v6"
            ;;
        *)
            die "Unsupported Raspberry Pi kernel: ${KERNEL_VERSION}"
            ;;
    esac

    apt-get install -y \
        dkms \
        build-essential \
        device-tree-compiler \
        "${header_package}"
}

check_kernel_build_tree()
{
    [[ -d "${KERNEL_BUILD_DIR}" ]] || \
        die "Kernel build directory not found: ${KERNEL_BUILD_DIR}"
}

build_dtbo()
{
    info "Building device-tree overlay"

    make \
        -C "${ROOT_DIR}" \
        dtbo \
        DTC=dtc

    [[ -f "${ROOT_DIR}/dts/pibrick.dtbo" ]] || \
        die "Device-tree overlay was not generated."
}

prepare_dkms_source()
{
    info "Preparing DKMS source"

    rm -rf "${DKMS_SOURCE_DIR}"

    mkdir -p "${DKMS_SOURCE_DIR}"

    install -m 0644 \
        "${ROOT_DIR}/Makefile" \
        "${DKMS_SOURCE_DIR}/Makefile"

    if [[ -f "${ROOT_DIR}/Kconfig" ]]; then
        install -m 0644 \
            "${ROOT_DIR}/Kconfig" \
            "${DKMS_SOURCE_DIR}/Kconfig"
    fi

    install -m 0644 \
        "${ROOT_DIR}/dkms/dkms.conf" \
        "${DKMS_SOURCE_DIR}/dkms.conf"

    cp -a \
        "${ROOT_DIR}/panel" \
        "${DKMS_SOURCE_DIR}/panel"

    cp -a \
        "${ROOT_DIR}/power" \
        "${DKMS_SOURCE_DIR}/power"

    cp -a \
        "${ROOT_DIR}/accel" \
        "${DKMS_SOURCE_DIR}/accel"

    mkdir -p "${DKMS_SOURCE_DIR}/touch"

    cp -a \
        "${ROOT_DIR}/touch/hyn_ts" \
        "${DKMS_SOURCE_DIR}/touch/hyn_ts"

    echo "DKMS source:"
    echo "  ${DKMS_SOURCE_DIR}"
}

remove_existing_dkms()
{
    if ! command -v dkms >/dev/null 2>&1; then
        return
    fi

    if dkms status "${DKMS_NAME}/${DKMS_VERSION}" \
        2>/dev/null | grep -q "${DKMS_NAME}/${DKMS_VERSION}"
    then
        info "Removing existing DKMS registration"

        dkms remove \
            "${DKMS_NAME}/${DKMS_VERSION}" \
            --all || true
    fi
}

install_dkms()
{
    info "Installing kernel modules with DKMS"

    remove_existing_dkms

    prepare_dkms_source

    dkms add \
        "${DKMS_NAME}/${DKMS_VERSION}"

    dkms build \
        "${DKMS_NAME}/${DKMS_VERSION}" \
        -k "${KERNEL_VERSION}"

    dkms install \
        "${DKMS_NAME}/${DKMS_VERSION}" \
        -k "${KERNEL_VERSION}"

    depmod -a "${KERNEL_VERSION}"
}

install_dtbo()
{
    info "Installing device-tree overlay"

    mkdir -p "${OVERLAY_DIR}"

    install -m 0644 \
        "${ROOT_DIR}/dts/pibrick.dtbo" \
        "${OVERLAY_FILE}"
}

build_application()
{
    local app_dir="$1"

    [[ -d "${app_dir}" ]] || return 0

    [[ -f "${app_dir}/Makefile" ]] || \
        die "Makefile not found: ${app_dir}/Makefile"

    info "Building $(basename "${app_dir}")"

    make -C "${app_dir}"
}

install_button_service()
{
    [[ -d "${BUTTON_APP_DIR}" ]] || return 0

    info "Installing pibrick-button-service"

    make -C "${BUTTON_APP_DIR}" install
}

install_keyboard_application()
{
    [[ -d "${KEYBOARD_APP_DIR}" ]] || return 0

    info "Installing pibrick-kbd"

    make -C "${KEYBOARD_APP_DIR}" install
}

show_status()
{
    info "Installation complete"

    echo
    echo "DKMS:"
    dkms status "${DKMS_NAME}/${DKMS_VERSION}" || true

    echo
    echo "Kernel modules:"

    for module in \
        panel-pibrick \
        pibrick-battery \
        pibrick-charger \
        pibrick-mma8451 \
        hyn_ts
    do
        if modinfo "${module}" >/dev/null 2>&1; then
            echo "  ${module}: installed"
        else
            echo "  ${module}: NOT FOUND"
        fi
    done

    echo
    echo "Device-tree overlay:"

    if [[ -f "${OVERLAY_FILE}" ]]; then
        echo "  ${OVERLAY_FILE}"
    else
        echo "  NOT FOUND"
    fi

    echo
    echo "Userspace:"
    echo "  pibrick-button-service:"
    if systemctl list-unit-files --type=service 2>/dev/null |
        grep -q '^pibrick-button-service\.service'; then

        if systemctl is-active --quiet pibrick-button-service.service; then
            echo "    installed, running"
        else
            echo "    installed, not running"
        fi
    else
        echo "    not installed"
    fi

    echo "  pibrick-kbd:"
    if [[ -x /usr/local/bin/pibrick-kbd ]]; then
        echo "    installed"
    else
        echo "    not installed"
    fi

    echo
    echo "Reboot is recommended."
}

install_config()
{
    local config="/boot/firmware/config.txt"

    if ! grep -qE '^[[:space:]]*dtoverlay=pibrick([[:space:]]|$)' "$config"; then
        info "Enabling piBrick device-tree overlay"
        printf '\n# piBrick CM5 drivers\ndtoverlay=pibrick\n' >> "$config"
    else
        info "piBrick device-tree overlay already enabled"
    fi
}

main()
{
    require_root "$@"

    echo "========================================"
    echo " piBrick CM5 Driver Installer"
    echo "========================================"
    echo
    echo "Version : ${PACKAGE_VERSION}"
    echo "Kernel  : ${KERNEL_VERSION}"
    echo "Source  : ${ROOT_DIR}"

    check_source_tree
    install_dependencies
    check_kernel_build_tree

    # The root Makefile builds the DTBO.
    # Kernel modules are built only once by DKMS.
    build_dtbo

    # DKMS owns kernel module compilation and installation.
    install_dkms

    # The DTBO is installed separately because it is not a kernel module.
    install_dtbo
    install_config

    # Userspace applications are built independently.
    install_button_service
    install_keyboard_application

    

    show_status
}

main "$@"
