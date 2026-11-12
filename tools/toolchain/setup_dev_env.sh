#!/usr/bin/env bash
# ZeroCC development environment setup
# Works on Ubuntu/Debian, Fedora, Arch, macOS (with Homebrew)
# Options:
#   USE_BOOTLIN=1  - use Bootlin gcc-15.3 cross toolchains
#   USE_DOCKER=1   - use Docker-based cross toolchains (no root, works everywhere except Docker-in-Docker)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# --- Detect OS ---
detect_os() {
    if [[ -f /etc/os-release ]]; then
        . /etc/os-release
        echo "$ID"
    elif [[ "$(uname)" == "Darwin" ]]; then
        echo "macos"
    else
        echo "unknown"
    fi
}

OS="$(detect_os)"
echo "Detected OS: $OS"

# --- Host C compiler (GCC >= 13 or Clang >= 14) ---
check_host_compiler() {
    local cc="${CC:-gcc}"
    if ! command -v "$cc" >/dev/null 2>&1; then
        echo "ERROR: No C compiler found. Set CC=gcc or CC=clang." >&2
        return 1
    fi
    local ver=$("$cc" -dumpversion 2>/dev/null | cut -d. -f1)
    if [[ "$cc" == *clang* ]]; then
        [[ $ver -ge 14 ]] || { echo "WARN: Clang < 14 may have issues" >&2; }
    else
        [[ $ver -ge 13 ]] || { echo "WARN: GCC < 13 may have issues" >&2; }
    fi
    echo "Host compiler: $cc $($cc --version | head -1)"
}

# --- Reference GCC 15 (mandatory for C23 gate + c-torture) ---
install_gcc15() {
    case "$OS" in
        ubuntu|debian)
            if ! command -v gcc-15 >/dev/null 2>&1; then
                echo "Installing gcc-15..."
                sudo apt update && sudo apt install -y gcc-15
            fi
            ;;
        fedora)
            sudo dnf install -y gcc15
            ;;
        arch)
            sudo pacman -S --needed gcc15
            ;;
        macos)
            if ! command -v gcc-15 >/dev/null 2>&1; then
                brew install gcc@15
            fi
            ;;
        *)
            echo "WARN: Manual gcc-15 install needed for $OS" >&2
            ;;
    esac
}

# --- Cross toolchains (for ISA gates) ---
install_cross_toolchains() {
    local use_bootlin="${USE_BOOTLIN:-0}"
    local use_docker="${USE_DOCKER:-0}"
    case "$OS" in
        ubuntu|debian)
            if [[ "$use_docker" == "1" ]]; then
                echo "Using Docker for cross toolchains..."
                install_docker_toolchains
            elif [[ "$use_bootlin" == "1" ]]; then
                echo "Using Bootlin toolchains..."
                install_bootlin_toolchains
            else
                sudo apt update && sudo apt install -y \
                    gcc-aarch64-linux-gnu gcc-riscv64-linux-gnu \
                    qemu-user-static
            fi
            ;;
        fedora)
            sudo dnf install -y \
                gcc-aarch64-linux-gnu gcc-riscv64-linux-gnu \
                qemu-user-static
            ;;
        arch)
            sudo pacman -S --needed \
                aarch64-linux-gnu-gcc riscv64-linux-gnu-gcc \
                qemu-user-static
            ;;
        macos)
            if [[ "$use_docker" == "1" ]]; then
                echo "Using Docker for cross toolchains..."
                install_docker_toolchains
            elif [[ "$use_bootlin" == "1" ]]; then
                echo "Using Bootlin toolchains (manual install required on macOS)..."
                install_bootlin_toolchains
            else
                echo "Cross toolchains on macOS: set USE_DOCKER=1 or USE_BOOTLIN=1" >&2
            fi
            ;;
        *)
            if [[ "$use_docker" == "1" ]]; then
                echo "Using Docker for cross toolchains..."
                install_docker_toolchains
            else
                echo "WARN: Manual cross toolchain install needed for $OS" >&2
            fi
            ;;
    esac
}

# --- Bootlin cross toolchains (gcc-15.3 for all targets) ---
install_bootlin_toolchains() {
    local version="2026.08-1"
    local base="https://toolchains.bootlin.com/downloads/releases/toolchains"
    local dir="$HOME/.local/bootlin"
    mkdir -p "$dir"
    cd "$dir"

    for target in aarch64--glibc riscv64-lp64d--glibc riscv32-ilp32d--glibc; do
        local tarball="${target}--stable-${version}.tar.xz"
        local url="${base}/${target}/tarballs/${tarball}"
        if [[ ! -d "${target}" ]]; then
            echo "Downloading $tarball..."
            curl -L -o "$tarball" "$url"
            tar -xf "$tarball"
            rm "$tarball"
        fi
    done

    # Add to PATH
    export PATH="$dir/aarch64--glibc/bin:$dir/riscv64-lp64d--glibc/bin:$dir/riscv32-ilp32d--glibc/bin:$PATH"
    echo "export PATH=\"$dir/aarch64--glibc/bin:$dir/riscv64-lp64d--glibc/bin:$dir/riscv32-ilp32d--glibc/bin:\$PATH\"" >> ~/.bashrc
    echo "Bootlin toolchains installed to $dir and added to PATH"
}

# --- Docker-based cross toolchains (no root, works everywhere except Docker-in-Docker) ---
install_docker_toolchains() {
    if ! command -v docker >/dev/null 2>&1; then
        echo "Docker not found." >&2
        return 1
    fi
    # Check if we're already inside a container (Docker-in-Docker often fails)
    if [[ -f /.dockerenv ]] || grep -q 'docker\|lxc' /proc/1/cgroup 2>/dev/null; then
        echo "Detected container environment - Docker-in-Docker may not work." >&2
        echo "Falling back to Bootlin toolchains..." >&2
        install_bootlin_toolchains
        return
    fi
    local img="ghcr.io/wxcdart/zerocc-cross:latest"
    echo "Pulling cross-toolchain Docker image: $img"
    docker pull "$img"

    # Create wrapper scripts
    local bindir="$HOME/.local/bin"
    mkdir -p "$bindir"
    for cc in aarch64-linux-gnu-gcc riscv64-linux-gnu-gcc; do
        cat > "$bindir/$cc" <<'EOF'
#!/usr/bin/env bash
docker run --rm -v "$(pwd):/work" -w /work ghcr.io/wxcdart/zerocc-cross:latest $cc "$@"
EOF
        chmod +x "$bindir/$cc"
    done
    # QEMU via docker too
    cat > "$bindir/qemu-aarch64-static" <<'EOF'
#!/usr/bin/env bash
docker run --rm -v "$(pwd):/work" -w /work ghcr.io/wxcdart/zerocc-cross:latest qemu-aarch64-static "$@"
EOF
    cat > "$bindir/qemu-riscv64-static" <<'EOF'
#!/usr/bin/env bash
docker run --rm -v "$(pwd):/work" -w /work ghcr.io/wxcdart/zerocc-cross:latest qemu-riscv64-static "$@"
EOF
    cat > "$bindir/qemu-riscv32-static" <<'EOF'
#!/usr/bin/env bash
docker run --rm -v "$(pwd):/work" -w /work ghcr.io/wxcdart/zerocc-cross:latest qemu-riscv32-static "$@"
EOF
    chmod +x "$bindir/qemu-aarch64-static" "$bindir/qemu-riscv64-static" "$bindir/qemu-riscv32-static"
    export PATH="$bindir:$PATH"
    echo "Docker-based cross toolchains installed to $bindir and added to PATH"
    echo "(runs via docker - needs ghcr.io/wxcdart/zerocc-cross image)"
}

# --- Build musl cross-wrappers ---
build_musl_wrappers() {
    echo "Building static musl cross-wrappers..."
    bash tools/setup_musl_cross.sh --arch x86_64 --ccbin gcc-15 --host x86_64-linux-musl
    bash tools/setup_musl_cross.sh --arch aarch64 --ccbin aarch64-linux-gnu-gcc
    bash tools/setup_musl_cross.sh --arch riscv64 --ccbin riscv64-linux-gnu-gcc
    bash tools/setup_musl_cross.sh --arch riscv32 --ccbin riscv64-linux-gnu-gcc \
        --extra "-march=rv32gc -mabi=ilp32" --host riscv32-linux-musl
}

# --- Main ---
main() {
    echo "=== ZeroCC Development Environment Setup ==="
    check_host_compiler || exit 1
    install_gcc15
    install_cross_toolchains
    build_musl_wrappers
    echo "=== Building zerocc ==="
    make -j"$(nproc)" build/bin/zerocc
    echo "=== Running quick smoke test ==="
    ./build/bin/zerocc --run -O0 test/jit/basic.c
    echo "=== Setup complete ==="
    echo "Run 'make test' for full regression suite"
    echo "Run 'make test-c23' for C23 gate (needs gcc-15)"
    echo "Run 'make test-isa-qemu' for ISA gates (needs cross toolchains + QEMU)"
}

main "$@"
