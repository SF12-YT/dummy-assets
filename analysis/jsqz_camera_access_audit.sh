#!/system/bin/sh
#
# Read-only JSQZ camera-access audit for Exynos 9820 Samsung devices.
# This script does NOT open /dev/jsqz, submit an ioctl, change permissions,
# edit SELinux policy, or restart any service.
#
# Run as root for complete /proc and log visibility:
#   su -c 'sh /path/to/jsqz_camera_access_audit.sh'
#

set +e

section() {
    echo
    echo "===== $1 ====="
}

section "Device build"
for key in \
    ro.product.device \
    ro.product.model \
    ro.build.fingerprint \
    ro.vendor.build.fingerprint \
    ro.build.version.release \
    ro.build.version.sdk \
    ro.build.version.incremental \
    ro.build.version.security_patch
do
    printf '%s=' "$key"
    getprop "$key" 2>/dev/null
done

section "Root and SELinux state"
id 2>/dev/null
if command -v getenforce >/dev/null 2>&1; then
    getenforce
else
    echo "getenforce unavailable"
fi

section "JSQZ device node"
if [ -e /dev/jsqz ]; then
    ls -lnZ /dev/jsqz 2>&1
    ls -lZ /dev/jsqz 2>&1
    if command -v stat >/dev/null 2>&1; then
        stat -c 'mode=%a (%A) uid=%u gid=%g owner=%U group=%G type=%F' /dev/jsqz 2>&1
    fi
else
    echo "/dev/jsqz does not exist"
fi

section "Active ueventd rules mentioning JSQZ"
found=0
for file in \
    /ueventd.rc \
    /vendor/ueventd.rc \
    /vendor/etc/ueventd.rc \
    /odm/ueventd.rc \
    /odm/etc/ueventd.rc \
    /system/etc/ueventd.rc \
    /system/vendor/ueventd.rc
do
    if [ -f "$file" ]; then
        matches=$(grep -n '/dev/jsqz' "$file" 2>/dev/null)
        if [ -n "$matches" ]; then
            echo "--- $file"
            echo "$matches"
            found=1
        fi
    fi
done
if [ "$found" -eq 0 ]; then
    echo "No /dev/jsqz rule found in the standard ueventd.rc locations checked."
fi

section "Camera-related processes and security identities"
if command -v ps >/dev/null 2>&1; then
    ps -AZ 2>/dev/null | grep -i -E 'camera|provider' | grep -v grep
    ps -A -o PID,NAME 2>/dev/null | awk 'tolower($0) ~ /camera|provider/ {print $1}' | while read pid
    do
        case "$pid" in
            ''|*[!0-9]*) continue ;;
        esac
        [ -r "/proc/$pid/status" ] || continue
        echo "--- PID $pid"
        grep -E '^(Name|Uid|Gid|Groups):' "/proc/$pid/status" 2>/dev/null
        if [ -r "/proc/$pid/attr/current" ]; then
            printf 'SELinux domain: '
            cat "/proc/$pid/attr/current" 2>/dev/null
        fi
        if [ -r "/proc/$pid/cmdline" ]; then
            printf 'Command: '
            tr '\000' ' ' < "/proc/$pid/cmdline" 2>/dev/null
            echo
        fi
    done
else
    echo "ps unavailable"
fi

section "Relevant SELinux AVC records from logcat"
if command -v logcat >/dev/null 2>&1; then
    avc_log=$(logcat -b all -d 2>/dev/null | grep -i -E 'avc: *denied.*(jsqz|camera|chr_file)|avc: *denied.*tclass=chr_file.*(jsqz|camera)' | tail -n 80)
    if [ -n "$avc_log" ]; then
        echo "$avc_log"
    else
        echo "No matching logcat AVC line found in the available buffers."
    fi
else
    echo "logcat unavailable"
fi

section "Relevant kernel audit / JSQZ messages"
if command -v dmesg >/dev/null 2>&1; then
    kernel_log=$(dmesg 2>/dev/null | grep -i -E 'avc: *denied.*(jsqz|camera)|jsqz' | tail -n 80)
    if [ -n "$kernel_log" ]; then
        echo "$kernel_log"
    else
        echo "No matching dmesg line found or access was denied."
    fi
else
    echo "dmesg unavailable"
fi

section "Audit complete"
echo "Read-only only: no JSQZ open, ioctl, chmod, chown, policy change, or service restart was performed."
