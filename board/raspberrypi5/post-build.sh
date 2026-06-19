#!/bin/sh
set -eu

# $TARGET_DIR is passed by Buildroot
T="${TARGET_DIR}"

# Buildroot exports BR2_CONFIG, but we set a default just in case
BR2_CONFIG="${BR2_CONFIG:-.config}"

# ------------------------------------------------------------------------------
# Board Detection
# ------------------------------------------------------------------------------
# We check the actual configuration symbols to determine the board variant.

if grep -q "^BR2_PACKAGE_RPI_FIRMWARE_VARIANT_PI4=y" "$BR2_CONFIG"; then
    COMPAT_STR="Synthesizer-RPi4"
    BOARD_NAME="Raspberry Pi 4 B"
elif grep -q "^BR2_PACKAGE_RPI_FIRMWARE_VARIANT_PI5=y" "$BR2_CONFIG"; then
    COMPAT_STR="Synthesizer-RPi5"
    BOARD_NAME="Raspberry Pi 5"
else
    # Fallback: Check if the legacy RPi4 64-bit string exists in the config header
    # or default to a safe generic name.
    if grep -q "raspberrypi4-64" "$BR2_CONFIG"; then
        COMPAT_STR="Synthesizer-RPi4"
        BOARD_NAME="Raspberry Pi 4 B"
    else
        COMPAT_STR="Synthesizer-Generic"
        BOARD_NAME="Raspberry Pi (Generic)"
        echo "WARNING: Could not detect specific RPi firmware variant. Defaulting to Generic."
    fi
fi

echo "POST-BUILD: Detected Board: $BOARD_NAME ($COMPAT_STR)"

# ------------------------------------------------------------------------------
# Update RAUC Configuration
# ------------------------------------------------------------------------------
if [ -f "$T/etc/rauc/system.conf" ]; then
    sed -i "s/@BOARD_COMPATIBLE@/$COMPAT_STR/g" "$T/etc/rauc/system.conf"
    echo "POST-BUILD: Configured RAUC system.conf for $COMPAT_STR"
fi

# ------------------------------------------------------------------------------
# Generate /etc/os-release
# ------------------------------------------------------------------------------
cat <<EOF > "$T/etc/os-release"
NAME="Synthesizer-OS"
ID=instrument-cluster
PRETTY_NAME="Instrument Cluster OS ($BOARD_NAME)"
BUILD_ID=$(date +%Y%m%d%H%M)
VARIANT_ID=$COMPAT_STR
EOF

echo "POST-BUILD: Generated /etc/os-release"

# ------------------------------------------------------------------------------
# Create Directory Structure & Permissions
# ------------------------------------------------------------------------------
mkdir -p \
  "$T/etc/systemd/system/multi-user.target.wants" \
  "$T/etc/systemd/system/getty.target.wants"

# Make installers executable if they exist
for script in "install-wifi-config.sh" "mount-overlay.sh"; do
    if [ -e "$T/usr/local/bin/$script" ]; then
        chmod 0755 "$T/usr/local/bin/$script"
    fi
done

# ------------------------------------------------------------------------------
# Systemd Tweaks
# ------------------------------------------------------------------------------
# We check for the directory existence instead of a specific optional service file.
UNITDIR=""
for d in /usr/lib/systemd/system /lib/systemd/system; do
  if [ -d "$T$d" ]; then
      UNITDIR="$d"
      break
  fi
done

if [ -n "$UNITDIR" ]; then
    echo "POST-BUILD: Found systemd unit dir at $UNITDIR"

    # Force a login prompt on tty1
    # if [ -e "$T$UNITDIR/getty@.service" ]; then
    #   ln -snf "$UNITDIR/getty@.service" \
    #     "$T/etc/systemd/system/getty.target.wants/getty@tty1.service"
    # fi
    
    # Mask the generic wpa_supplicant unit so only the @wlan0 instance runs
    # This prevents wpa_supplicant from fighting with NetworkManager or running without config
    ln -snf /dev/null "$T/etc/systemd/system/wpa_supplicant.service"
    rm -f "$T/etc/systemd/system/multi-user.target.wants/wpa_supplicant.service" 2>/dev/null || true

    # DEBUG: Mask synthesizer service
    # if [ -e "${T}/usr/lib/systemd/system/synthesizer.service" ]; then
    #   ln -sf /dev/null "${T}/etc/systemd/system/synthesizer.service"
    # fi

    # --------------------------------------------------------------------------
    # PipeWire / WirePlumber / D-Bus Hotfixes
    # --------------------------------------------------------------------------
    # patch WirePlumber's alsa.lua to avoid crash when node_name is nil
    ALSA_LUA="$T/usr/share/wireplumber/scripts/monitors/alsa.lua"
    if [ -f "$ALSA_LUA" ]; then
        sed -i 's/node_names_table\[node_name\] = nil/if node_name then node_names_table[node_name] = nil end/g' "$ALSA_LUA"
        echo "POST-BUILD: Patched wireplumber alsa.lua"
    fi

    # update WirePlumber service to use dbus-run-session for proper device discovery
    WP_SERVICE="$T$UNITDIR/wireplumber.service"
    if [ -f "$WP_SERVICE" ]; then
        sed -i 's|ExecStart=/usr/bin/wireplumber|ExecStart=/usr/bin/dbus-run-session /usr/bin/wireplumber|g' "$WP_SERVICE"
        echo "POST-BUILD: Patched wireplumber.service to use dbus-run-session"
    fi

    # redirect WirePlumber state to /data/wireplumber (writable partition)
    mkdir -p "$T/etc/systemd/system/wireplumber.service.d"
    cat <<OV > "$T/etc/systemd/system/wireplumber.service.d/state.conf"
[Service]
Environment=WIREPLUMBER_STATE_DIR=/data/wireplumber
OV
    echo "POST-BUILD: Configured WirePlumber state redirect to /data/wireplumber"

    # fix bluez.lua to ignore seat state (fixes device discovery in headless)
    BLUEZ_LUA="$T/usr/share/wireplumber/scripts/monitors/bluez.lua"
    if [ -f "$BLUEZ_LUA" ]; then
        sed -i 's/if seat_state == "active" then/if true then/g' "$BLUEZ_LUA"
        echo "POST-BUILD: Patched wireplumber bluez.lua"
    fi
else
    echo "POST-BUILD: WARNING - Systemd unit directory not found. Skipping systemd tweaks."
fi

exit 0