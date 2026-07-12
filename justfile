# UFB — Qt 6.11 + Rust + CMake build helpers.
# Install with: cargo install just

set windows-shell := ["powershell.exe", "-NoLogo", "-NonInteractive", "-Command"]

# Hardcoded for now; matches the dev machine. CI overrides via env.
vcvars := 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'

# Qt toolchain location — needed both by CMake (find_package) and by
# cxx-qt-build (QMAKE env var). Override with `just QT=... build`.
qt_root := 'C:\Qt\6.11.1\msvc2022_64'

default: build

# Configure + build dev (RelWithDebInfo on Windows because Rust+MSVC needs release CRT)
[windows]
build:
    @powershell -NoLogo -NoProfile -Command "$env:QMAKE='{{qt_root}}\bin\qmake.exe'; cmd /c '\"{{vcvars}}\" && cmake --preset=debug && cmake --build --preset=debug --parallel'"

[unix]
build:
    cmake --preset=debug
    cmake --build --preset=debug --parallel

# Configure + build release
[windows]
release:
    @powershell -NoLogo -NoProfile -Command "$env:QMAKE='{{qt_root}}\bin\qmake.exe'; cmd /c '\"{{vcvars}}\" && cmake --preset=release && cmake --build --preset=release --parallel'"

[unix]
release:
    cmake --preset=release
    cmake --build --preset=release --parallel

# Run the dev binary (Windows). Adds Qt's bin/ to PATH so the Qt DLLs resolve.
[windows]
run: build
    @powershell -NoLogo -NoProfile -Command "$env:PATH='C:\Qt\6.11.1\msvc2022_64\bin;'+$env:PATH; & 'build\debug\ufb.exe'"

# Run the dev binary (macOS/Linux)
[unix]
run: build
    ./build/debug/ufb

# Build the agent (separate crate, not in workspace).
agent:
    cargo build --manifest-path agent/Cargo.toml --target x86_64-pc-windows-msvc

agent-release:
    cargo build --manifest-path agent/Cargo.toml --release --target x86_64-pc-windows-msvc

# Run the agent (debug). Stays in foreground for log streaming.
[windows]
agent-run: agent
    ./agent/target/x86_64-pc-windows-msvc/debug/ufb-agent.exe

# Run all Rust tests
test:
    cargo test --workspace
    cargo test --manifest-path agent/Cargo.toml

# Format Rust source
fmt:
    cargo fmt --all

# Lint Rust
lint:
    cargo clippy --workspace --all-targets -- -D warnings

# Clean Rust build artifacts
clean:
    cargo clean

# Wipe everything (Rust target + CMake build dirs)
[windows]
clean-all: clean
    @powershell -NoLogo -NoProfile -Command "if (Test-Path build) { Remove-Item -Recurse -Force build }"

[unix]
clean-all: clean
    rm -rf build
