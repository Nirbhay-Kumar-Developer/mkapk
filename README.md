# mkapk: Android Build System for Termux

`mkapk` is a lightweight, native Android build orchestrator designed specifically for high-speed, local compilation directly inside Termux (`aarch64`). It bypasses the overhead of heavy cross-compilation environments, bringing intelligent incremental builds, code optimization, and multi-architecture targeting straight to your terminal.

---

## Features

* **Incremental Builds:** Automatically detects source changes and rebuilds only what is necessary, drastically cutting down iteration times.
* **Obfuscation Engine:** Built-in support for Java, Kotlin, and resource obfuscation to protect your source code.
* **Binary Stripping:** Automatically strips library binaries to minimize your final APK footprint.
* **Project Initialization:** Bootstrap new Android projects instantly with built-in template directory structure initialization.
* **Targeted Architecture Generation:** Compile universal APKs or generate split, architecture-specific binaries smoothly.

---

## Prerequisites

Before compiling or running `mkapk`, you need to ensure the required native build utilities, Java development kit, and Android platform tools are installed in your Termux environment.

### Required Packages
* `openjdk-21`
* `openssl`
* `ndk-multilib`
* `aapt2`
* `apksigner`
* `clang`
* `binutils`
* `libzip`
* `wget`
* `kotlin`

### One-Line Installation
Run the following command inside Termux to install all dependencies at once:

```bash
apt update && apt install openjdk-21 ndk-multilib aapt2 clang binutils libzip openssl apksigner -y
```

## Debian package download
<a href="https://github.com/Nirbhay-Kumar-Developer/mkapk/releases/latest/download/mkapk-aarch64.deb">Download Latest Release</a>


---

## Project template Repository
<a href="https://github.com/Nirbhay-Kumar-Developer/mkapk-template">Template</a>


---

## Usage Instructions

### Project Setup

Initialise template directory structure in the current directory.

```bash
mkapk init
```

### Build

Start build using:

```bash
mkapk build
```

Default build is debug.

#### Build Flags

* `-release`: Do a release build
* `-all`: Force a full rebuild
* `-ndk-all`: Build apk for all supported architectures along with a universal apk
* `-arch <architecture>`: Build apk only for the specified architecture

### Clean Up

To clean up previous build artifacts, run:

```bash
mkapk clean
```

**Note:** This will disable incremental build for the next build.

### Language Plugin Management (experimental)

To install a plugin, run:

```bash
mkapk install <path/to/plugin.pl>
```

To uninstall a plugin, run:

```bash
mkapk uninstall <path/to/plugin.pl>
```