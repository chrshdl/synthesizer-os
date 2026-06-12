# ==========================================================
# RAUC A/B Boot Script for Raspberry Pi 4 (Waveshare DSI)
# ==========================================================

test -n "${BOOT_ORDER}" || setenv BOOT_ORDER "A B"
test -n "${BOOT_A_LEFT}" || setenv BOOT_A_LEFT 3
test -n "${BOOT_B_LEFT}" || setenv BOOT_B_LEFT 3

setenv raucslot
for BOOT_SLOT in "${BOOT_ORDER}"; do
  if test "x${raucslot}" != "x"; then
    # Skip if we already found a slot
  elif test "x${BOOT_SLOT}" = "xA"; then
    if test ${BOOT_A_LEFT} -gt 0; then
      setenv raucslot "A"
      setenv raucpart 2
      setexpr BOOT_A_LEFT ${BOOT_A_LEFT} - 1
    fi
  elif test "x${BOOT_SLOT}" = "xB"; then
    if test ${BOOT_B_LEFT} -gt 0; then
      setenv raucslot "B"
      setenv raucpart 3
      setexpr BOOT_B_LEFT ${BOOT_B_LEFT} - 1
    fi
  fi
done

if test -n "${raucslot}"; then
  setenv bootargs "root=/dev/mmcblk0p${raucpart} rauc.slot=${raucslot} rootwait console=tty3 console=ttyAMA0,115200 quiet loglevel=3 video=HDMI-A-1:d video=HDMI-A-2:d logo.nologo vt.global_cursor_default=0 systemd.show_status=0 cma=256M"
  
  # Silence U-Boot's internal echoes by using 'setenv silent 1' 
  # (Requires U-Boot to be compiled with CONFIG_SILENT_CONSOLE=y)
  setenv silent 1
  setenv bootdelay 0
  
  fatload mmc 0:1 ${kernel_addr_r} Image
  booti ${kernel_addr_r} - ${fdt_addr}
else
  setenv BOOT_A_LEFT 3
  setenv BOOT_B_LEFT 3
  saveenv
  reset
fi
