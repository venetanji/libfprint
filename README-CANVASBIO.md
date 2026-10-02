# CanvasBio CB2000 - Manual Installation Guide

This guide describes how to manually install the custom `libfprint` driver for the CanvasBio CB2000 fingerprint sensor (`2df0:0003`) on Ubuntu, without using the automatic install script.

## 1. Prerequisites

Ensure you have built the driver first:
```bash
cd ~/workspace/fingerprint/libfprint
meson setup build -Ddrivers=canvasbio
ninja -C build
```

## 2. Manual Installation Steps

### Step A: Stop the fingerprint daemon
```bash
sudo systemctl stop fprintd
```

### Step B: Backup the original system library
It is highly recommended to keep a backup of the original `libfprint` in case you need to revert.
```bash
sudo mkdir -p /var/lib/libfprint-backup
sudo cp /usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0 /var/lib/libfprint-backup/libfprint-2.so.2.0.0.original
```

### Step C: Install the custom library
Copy the newly built library to the system library directory:
```bash
sudo cp build/libfprint/libfprint-2.so.2.0.0 /usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0
```

### Step D: Update library links
Update the system's shared library cache and ensure the symlink points to our new version:
```bash
sudo ldconfig
sudo ln -sf libfprint-2.so.2.0.0 /usr/lib/x86_64-linux-gnu/libfprint-2.so.2
```

### Step E: Configure USB Permissions (udev)
Create a new udev rule so the system can access the scanner without root privileges:
```bash
echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="2df0", ATTRS{idProduct}=="0003", GROUP="plugdev", MODE="0664", TAG+="uaccess"' | sudo tee /etc/udev/rules.d/60-canvasbio.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

### Step F: Restart the daemon
```bash
sudo systemctl start fprintd
```

## 3. Verification

To verify that the system is using the new driver, you can check the logs while enrolling:
```bash
G_MESSAGES_DEBUG=all fprintd-enroll
```

And verify the enrollment:
```bash
G_MESSAGES_DEBUG=all fprintd-verify
```

## 4. Reverting Changes

If you need to restore the original Ubuntu driver:
```bash
sudo cp /var/lib/libfprint-backup/libfprint-2.so.2.0.0.original /usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0
sudo ldconfig
sudo systemctl restart fprintd
```
