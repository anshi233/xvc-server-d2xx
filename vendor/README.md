# Vendor Libraries

This directory contains all external libraries required by the XVC server.

## Philosophy

All external dependencies are **bundled locally** in this `vendor/` directory. This ensures:

- **Reproducible builds**: No dependency on system library versions
- **Version control**: Specific library versions are tracked in repository
- **Cross-compilation**: Works consistently across different platforms
- **Self-contained**: No system-wide installations required

## Only System Dependencies

The only dependencies taken from the system are:
- **C standard library** (libc, libm, libpthread)
- **Standard system headers**

All other libraries are built locally from source.

## Directory Structure

```
vendor/
├── README.md               # This file
├── d2xx/                   # FTDI D2XX driver for HS2 communication
│   ├── x86_64/             # x86_64 architecture
│   │   ├── build/          # Static library
│   │   └── libusb/         # libusb bundled with D2XX
│   └── arm64/              # ARM64 architecture
│       ├── build/          # Static library
│       └── libusb/         # libusb bundled with D2XX
└── (libusb is bundled with D2XX)
```

## D2XX Driver

**Purpose**: FTDI's proprietary driver for communicating with FTDI USB devices (Digilent HS2)

**Version**: See `vendor/d2xx/x86_64/ftd2xx.h` or `vendor/d2xx/arm64/ftd2xx.h` for version

**Source**: https://ftdichip.com/drivers/d2xx-drivers/

**Used by**: `xvc-server` (for HS2 communication)

**Note**: The D2XX driver requires exclusive access to the FTDI device. The `ftdi_sio` kernel module must be removed before using:
```bash
sudo rmmod ftdi_sio
sudo rmmod usbserial
```

**Key Features**:
- Direct hardware access without kernel driver
- Better performance for high-speed JTAG
- Works in userspace
- Bundled with compatible libusb

## Building Vendor Libraries

The D2XX driver is provided as pre-built static libraries. No compilation needed:

```bash
# The D2XX libraries are already included in vendor/d2xx/
# Just build the main project:
make
```

## Cross-Compilation

D2XX libraries are available for both x86_64 and ARM64:

```bash
# Build for ARM64
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

# The correct D2XX library will be selected automatically
```

## Version Information

D2XX driver versions are documented in the release notes:
- **x86_64**: `vendor/d2xx/x86_64/release-notes.txt`
- **ARM64**: `vendor/d2xx/arm64/release-notes.txt`

## Troubleshooting

### Device Not Detected

**Check if ftdi_sio is blocking D2XX access:**
```bash
# Check if kernel module is loaded
lsmod | grep ftdi_sio

# Remove if present
sudo rmmod ftdi_sio
sudo rmmod usbserial

# Verify device is accessible
sudo xvc-discover
```

**Make persistent across reboots:**
```bash
echo "blacklist ftdi_sio" | sudo tee /etc/modprobe.d/blacklist-ftdi.conf
echo "blacklist usbserial" | sudo tee -a /etc/modprobe.d/blacklist-ftdi.conf
```

### Architecture Mismatch

**Check library architecture:**
```bash
# Should show: ELF 64-bit LSB shared object
file vendor/d2xx/x86_64/build/libftd2xx.a
file vendor/d2xx/arm64/build/libftd2xx.a
```

### Cross-Compilation Issues

**ARM64 cross-compiler not found:**
```bash
# Install cross-compiler
sudo apt install gcc-aarch64-linux-gnu

# Verify installation
aarch64-linux-gnu-gcc --version
```

## Security Considerations

### Trust
Vendor libraries are trusted because:
- They are from official FTDI source
- Version-controlled in repository
- No binary downloads from untrusted sources

## References

- **D2XX Programmer's Guide**: https://ftdichip.com/document/programming-guides/
- **FTDI Chip Documentation**: https://www.ftdichip.com/
- **Digilent HS2**: https://digilent.com/reference/test-and-measurement/jtag-hs2/start

## License

- **D2XX Driver**: Proprietary (FTDI), free to use with FTDI hardware
- **Bundled libusb**: LGPL v2.1

Both are compatible with our project's requirements.
