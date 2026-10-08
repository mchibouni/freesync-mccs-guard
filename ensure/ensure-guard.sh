#!/bin/bash
# Make sure the amdgpu FreeSync MCCS guard is built for the RUNNING kernel.
#
# Why this exists: SteamOS 3.x is A/B atomic. An OS update writes a whole new
# rootfs (/usr, and a fresh per-slot /var) into the other partition set, so BOTH
# the DKMS source tree in /usr/src AND the DKMS state in /var/lib/dkms are gone
# on the first boot after an update. Without the guard, the faulty
# dm_freesync_mccs_ddc_worker fires ~26s in and the machine bootloops on HDMI.
#
# /home is shared across both slots, so the source is stashed there.
#
# NOTE: this cannot rescue the boot it runs on -- amdgpu has already loaded by
# then. It is the recovery path: boot UNDOCKED (which comes up clean), let this
# rebuild the guard, then re-dock.
set -uo pipefail

PKG=freesync-mccs-guard
VER=1.0.0
STASH_DIR=/home/deck/.local/share/freesync-mccs-guard
# canonical source is the kprobe working tree; the stash is the fallback copy
if [[ -f /home/deck/kprobe/dkms.conf ]]; then STASH=/home/deck/kprobe; else STASH="$STASH_DIR/src"; fi
SRC="/usr/src/$PKG-$VER"
KVER="$(uname -r)"
MOD=freesync_mccs_guard

log() { echo "[freesync-guard] $*"; }

if [[ $EUID -ne 0 ]]; then log "must run as root"; exit 1; fi

# --- REFUSE to auto-build for a kernel whose struct layout nobody verified.
# The guard calls kfree(work), valid ONLY while work_struct is the FIRST member
# of struct dm_freesync_mccs_ddc_work. If Valve reorders it, an auto-rebuilt
# guard loads cleanly and CORRUPTS THE HEAP. An unguarded bootloop is recoverable
# (boot undocked); heap corruption is not. So we fail loud instead of guessing.
VERIFIED=/home/deck/kprobe/verified-kernels
if ! grep -qxF "$KVER" "$VERIFIED" 2>/dev/null; then
    log "REFUSING to auto-build: kernel $KVER is not in $VERIFIED"
    log "  The struct layout for this kernel has not been human-verified."
    log "  Check 'struct dm_freesync_mccs_ddc_work' in amdgpu_dm.c, then:"
    log "    echo $KVER >> $VERIFIED  &&  sudo ~/kprobe/guard rebuild"
    log "  Until then this machine is UNGUARDED -- do not boot it docked."
    exit 0
fi
log "kernel $KVER is in the verified list"

if dkms status -m "$PKG" -v "$VER" -k "$KVER" 2>/dev/null | grep -q installed; then
    log "already installed for $KVER"
    modprobe "$MOD" 2>/dev/null
    log "armed=$(cat /sys/module/$MOD/parameters/armed 2>/dev/null || echo '?')"
    exit 0
fi

log "guard NOT installed for $KVER -- rebuilding"

RO_WAS="$(steamos-readonly status 2>/dev/null || echo unknown)"
if [[ "$RO_WAS" == enabled ]]; then
    log "disabling rootfs read-only"
    steamos-readonly disable || { log "FATAL: cannot make / writable"; exit 1; }
fi
restore_ro() { [[ "$RO_WAS" == enabled ]] && steamos-readonly enable && log "rootfs read-only restored"; }
trap restore_ro EXIT

# 1. source tree (wiped by the update)
if [[ ! -f "$SRC/dkms.conf" ]]; then
    if [[ ! -f "$STASH/dkms.conf" ]]; then log "FATAL: no stashed source at $STASH"; exit 1; fi
    log "restoring source from $STASH"
    mkdir -p "$SRC" && install -m 0644 "$STASH/freesync_mccs_guard.c" "$STASH/Makefile" "$STASH/dkms.conf" "$SRC/"
fi

# 2. kernel headers for the new kernel (dkms cannot build without them)
if [[ ! -f "/usr/lib/modules/$KVER/build/Makefile" ]]; then
    KPKG="$(pacman -Qqo "/usr/lib/modules/$KVER/modules.order" 2>/dev/null | head -1)"
    if [[ -n "$KPKG" ]]; then
        log "headers missing -- installing ${KPKG}-headers"
        pacman -Sy --noconfirm "${KPKG}-headers" || log "WARN: header install failed (network?)"
    else
        log "WARN: cannot determine kernel package for $KVER"
    fi
fi
[[ -f "/usr/lib/modules/$KVER/build/Makefile" ]] || { log "FATAL: no kernel headers for $KVER"; exit 1; }

# 3. build + install
dkms add -m "$PKG" -v "$VER" 2>/dev/null
dkms build  --force -m "$PKG" -v "$VER" -k "$KVER" || { log "FATAL: build failed"; exit 1; }
dkms install --force -m "$PKG" -v "$VER" -k "$KVER" || { log "FATAL: install failed"; exit 1; }

depmod -a "$KVER"
modprobe "$MOD" && log "loaded; armed=$(cat /sys/module/$MOD/parameters/armed 2>/dev/null || echo '?')"
dkms status -m "$PKG"
