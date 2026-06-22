#!/bin/bash
# run-perf-test.sh — Run MiniMem VM performance tests at different memory sizes
#
# Usage:
#   ./run-perf-test.sh              # Default: 768M quick test
#   ./run-perf-test.sh --8gb         # 8GB full performance test
#   ./run-perf-test.sh --4gb         # 4GB medium test
#   ./run-perf-test.sh --2gb         # 2GB test
#   ./run-perf-test.sh --ram 8G      # Custom RAM
#
# The performance harness auto-scales test parameters based on available RAM.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Default: standard test
VM_RAM="768M"
VM_CPUS="4"
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --8gb)
            VM_RAM="8G"
            EXTRA_ARGS+=(--timeout 300)
            shift
            ;;
        --4gb)
            VM_RAM="4G"
            EXTRA_ARGS+=(--timeout 240)
            shift
            ;;
        --2gb)
            VM_RAM="2G"
            EXTRA_ARGS+=(--timeout 200)
            shift
            ;;
        --ram)
            VM_RAM="$2"
            shift 2
            ;;
        --custom-kernel|--rebuild|--skip-parallel|--shell)
            EXTRA_ARGS+=("$1")
            shift
            ;;
        *)
            EXTRA_ARGS+=("$1")
            shift
            ;;
    esac
done

echo ""
echo "=========================================="
echo "  MiniMem VM Performance Test"
echo "  RAM: $VM_RAM  CPUs: $VM_CPUS"
echo "=========================================="
echo ""
echo "This will:"
echo "  1. Build the kernel module (if needed)"
echo "  2. Build test binaries"
echo "  3. Create a QEMU VM with ${VM_RAM} RAM"
echo "  4. Run kselftest + E2E + stress + performance tests"
echo "  5. Print a detailed results summary"
echo ""
echo "Press Ctrl+C to cancel, or wait 5 seconds..."
sleep 5

# Build module and tests first
"$SCRIPT_DIR/build-kmod.sh" build
"$SCRIPT_DIR/build-kmod.sh" tests

# Run the VM test
"$SCRIPT_DIR/vm-test-minimem.sh" --ram "$VM_RAM" "${EXTRA_ARGS[@]}"

echo ""
echo "=========================================="
echo "  Performance test complete."
echo "  Results are above."
echo ""
echo "  For a quick check:"
echo "    cat .vm-test/vm_output.log | grep -A5 'Performance Harness'"
echo ""
echo "  To compare across RAM sizes:"
echo "    ./run-perf-test.sh --8gb 2>&1 | tee results-8gb.log"
echo "    ./run-perf-test.sh --4gb 2>&1 | tee results-4gb.log"
echo "    ./run-perf-test.sh --2gb 2>&1 | tee results-2gb.log"
echo "=========================================="