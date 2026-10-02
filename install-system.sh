#!/bin/bash
# install-system.sh
# Installs our custom libfprint build to the system and reloads fprintd.
# This will override the system libfprint so Ubuntu uses our CanvasBio driver.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
LIB_SRC="$BUILD_DIR/libfprint/libfprint-2.so.2.0.0"
SYS_LIB_DIR="/usr/lib/x86_64-linux-gnu"
# Store backup OUTSIDE the lib dir — ldconfig scans everything in SYS_LIB_DIR
# and always picks the lexicographically latest SONAME match (any suffix beats .0.0)
BACKUP_DIR="/var/lib/libfprint-backup"
BACKUP_FILE="$BACKUP_DIR/libfprint-2.so.2.0.0.original"

# Check if the build exists
if [ ! -f "$LIB_SRC" ]; then
    echo "❌ Build not found. Run 'ninja -C build' first."
    exit 1
fi

# Check for sudo
if [ "$EUID" -ne 0 ]; then
    echo "❌ Please run as root: sudo ./install-system.sh"
    exit 1
fi

echo "🛑 Stopping fprintd..."
systemctl stop fprintd || true

echo "📦 Backing up original system library..."
mkdir -p "$BACKUP_DIR"
if [ -f "$SYS_LIB_DIR/libfprint-2.so.2.0.0" ] && [ ! -f "$BACKUP_FILE" ]; then
    cp "$SYS_LIB_DIR/libfprint-2.so.2.0.0" "$BACKUP_FILE"
    echo "   Backup saved to $BACKUP_FILE"
else
    echo "   Backup already exists, skipping."
fi

echo "📂 Installing custom libfprint..."
cp "$LIB_SRC" "$SYS_LIB_DIR/libfprint-2.so.2.0.0"
# ldconfig must run BEFORE setting the symlink — it scans all .so* files in the
# lib dir and overwrites the symlink to the lexicographically latest SONAME match.
# With no competing file in the dir, it will set the symlink correctly; we then
# force it once more for safety.
ldconfig
ln -sf libfprint-2.so.2.0.0 "$SYS_LIB_DIR/libfprint-2.so.2"

echo "🔌 Adding udev rules for CanvasBio CB2000..."
cat > /etc/udev/rules.d/60-canvasbio.rules << 'EOF'
# CanvasBio CB2000 Fingerprint Reader
SUBSYSTEM=="usb", ATTRS{idVendor}=="2df0", ATTRS{idProduct}=="0003", \
    GROUP="plugdev", MODE="0664", TAG+="uaccess"
EOF
udevadm control --reload-rules
udevadm trigger

echo "🚀 Restarting fprintd..."
systemctl start fprintd
sleep 1

echo ""
echo "✅ Installation complete!"
echo ""
echo "Next steps:"
echo "   1. Plug in your CanvasBio scanner (or reconnect it)"
echo "   2. Run:  fprintd-enroll"
echo "   3. Or open Settings → Users → Fingerprint Login"
echo ""
echo "To restore the original library:"
echo "   sudo cp $BACKUP_FILE $SYS_LIB_DIR/libfprint-2.so.2.0.0 && sudo ldconfig && sudo systemctl restart fprintd"
