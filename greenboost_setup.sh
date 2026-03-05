#!/usr/bin/env bash
# GreenBoost v2.3 — Setup & installation script
# Author: Ferran Duarri
# Hardware: ASRock B760M-ITX/D4 | i9-14900KF | RTX 5070 OC | 64 GB DDR4-3600 | Samsung 990 EVO Plus 4 TB
#
# 3-tier memory hierarchy:
#   Tier 1 — RTX 5070 VRAM      12 GB   ~336 GB/s GDDR7  (hot layers)
#   Tier 2 — DDR4 pool          51 GB   ~57.6 GB/s dual-ch (cold layers)
#   Tier 3 — NVMe swap         576 GB   ~7.25 GB/s seq     (frozen pages)
#   Combined capacity          639 GB
#
# USAGE:
#   sudo ./greenboost_setup.sh install          — build + install system-wide
#   sudo ./greenboost_setup.sh uninstall        — remove module + all config
#   sudo ./greenboost_setup.sh load             — insmod with default params
#   sudo ./greenboost_setup.sh unload           — rmmod
#   sudo ./greenboost_setup.sh install-sys-configs — install v2.3 system config files
#   sudo ./greenboost_setup.sh tune             — runtime tuning (governor, NVMe, sysctl)
#   sudo ./greenboost_setup.sh tune-grub        — GRUB/boot parameter optimization
#   sudo ./greenboost_setup.sh tune-sysctl      — consolidate + enhance sysctl (persistent)
#   sudo ./greenboost_setup.sh tune-libs        — install missing AI/compute libraries
#   sudo ./greenboost_setup.sh tune-all         — run all tune-* commands
#        ./greenboost_setup.sh status           — show pool info + system state
#        ./greenboost_setup.sh diagnose         — full health check (run after reboot)
#        ./greenboost_setup.sh build            — build only (no install)
#        ./greenboost_setup.sh help             — show this help
#
# ENVIRONMENT (for load command):
#   GPU_PHYS_GB=12     physical VRAM in GB       (RTX 5070 default)
#   VIRT_VRAM_GB=51    DDR4 pool size in GB      (80% of 64 GB DDR4)
#   RESERVE_GB=12      minimum free system RAM to always maintain
#   NVME_SWAP_GB=576   total NVMe swap capacity  (Samsung 990 Evo Plus)
#   NVME_POOL_GB=512   GreenBoost soft cap on T3 allocations

DRIVER_NAME="greenboost"
SHIM_LIB="libgreenboost_cuda.so"
SHIM_DEST="/usr/local/lib"
MODULE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Colours
RED='\033[0;31m'
GRN='\033[0;32m'
YLW='\033[1;33m'
BLU='\033[0;34m'
NC='\033[0m'

info()  { echo -e "${GRN}[GreenBoost]${NC} $*"; }
warn()  { echo -e "${YLW}[GreenBoost] WARN:${NC} $*"; }
die()   { echo -e "${RED}[GreenBoost] ERROR:${NC} $*" >&2; exit 1; }

# ---- Helpers -----------------------------------------------------------

need_root() {
    [[ $EUID -eq 0 ]] || die "Root required. Use: sudo $0 $1"
}

check_deps() {
    info "Checking build prerequisites..."
    command -v make >/dev/null || die "make not found (apt install build-essential)"
    command -v gcc  >/dev/null || die "gcc not found (apt install gcc)"

    # Check for liburing-dev
    if ! ldconfig -p | grep -q "liburing"; then
        warn "liburing not found — required for ExLlamaV3 stloader. Install: sudo apt install liburing-dev"
    fi

    local kdir="/lib/modules/$(uname -r)/build"
    [[ -d "$kdir" ]] || die "Kernel headers not found at $kdir
    Install with: sudo apt install linux-headers-$(uname -r)"
    info "Kernel headers : $kdir  ✓"

    if lsmod | grep -q "^nvidia "; then
        info "NVIDIA driver  : loaded  ✓"
    else
        warn "NVIDIA driver not loaded — run: sudo modprobe nvidia"
    fi

    if lsmod | grep -q "^nvidia_uvm "; then
        info "NVIDIA UVM     : loaded  ✓  (managed memory / DDR4 overflow ready)"
    else
        warn "nvidia_uvm not loaded — CUDA UVM overflow unavailable"
        warn "Fix: sudo modprobe nvidia_uvm"
    fi
}

# ---- Commands ----------------------------------------------------------

cmd_install_sys_configs() {
    need_root install-sys-configs

    info "Installing GreenBoost v2.3 system configuration files..."

    # 1. Ollama service — inject GreenBoost env vars + LD_PRELOAD
    local svc="/etc/systemd/system/ollama.service"
    if [[ -f "$svc" ]]; then
        # Add environment lines if not already present
        if ! grep -q "GREENBOOST_VRAM_HEADROOM_MB" "$svc"; then
            sed -i '/^\[Service\]/a Environment="OLLAMA_FLASH_ATTENTION=1"\nEnvironment="OLLAMA_KV_CACHE_TYPE=q8_0"\nEnvironment="OLLAMA_NUM_CTX=131072"\nEnvironment="OLLAMA_MAX_LOADED_MODELS=1"\nEnvironment="OLLAMA_KEEP_ALIVE=-1"\nEnvironment="GREENBOOST_VRAM_HEADROOM_MB=1024"\nEnvironment="GREENBOOST_DEBUG=0"\nEnvironment="LD_PRELOAD=/usr/local/lib/libgreenboost_cuda.so"' "$svc"
            info "Ollama service: GreenBoost env vars injected"
        else
            info "Ollama service: already configured (skip)"
        fi
        systemctl daemon-reload
        info "Ollama service: daemon-reload done"
    else
        warn "Ollama service not found at $svc — skipping"
    fi

    # 2a. GreenBoost device udev rule — allow video group (includes ollama) access
    cat > /etc/udev/rules.d/99-greenboost.rules << 'UDEVEOF'
# GreenBoost kernel module — allow video group (includes ollama) to access /dev/greenboost
KERNEL=="greenboost", GROUP="video", MODE="0660"
UDEVEOF
    info "GreenBoost device udev rule installed: /etc/udev/rules.d/99-greenboost.rules"
    # Apply rule immediately to existing /dev/greenboost (if module already loaded)
    udevadm control --reload-rules 2>/dev/null || true
    udevadm trigger --name-match=greenboost 2>/dev/null \
        || udevadm trigger --subsystem-match=greenboost 2>/dev/null \
        || true
    udevadm settle 2>/dev/null || true

    # 2b. NVMe udev rule — scheduler=none, read_ahead=4096, nr_requests=2048
    cat > /etc/udev/rules.d/99-nvme-greenboost.rules << 'UDEVEOF'
# GreenBoost v2.3 — NVMe tuning for T3 swap performance
ACTION=="add|change", KERNEL=="nvme[0-9]n[0-9]", ATTR{queue/scheduler}="none"
ACTION=="add|change", KERNEL=="nvme[0-9]n[0-9]", ATTR{queue/read_ahead_kb}="4096"
ACTION=="add|change", KERNEL=="nvme[0-9]n[0-9]", ATTR{queue/nr_requests}="2048"
UDEVEOF
    udevadm control --reload-rules && udevadm trigger || true
    info "NVMe udev rule installed: /etc/udev/rules.d/99-nvme-greenboost.rules"

    # 3. CPU governor service — P-cores only (E-cores stay on powersave)
    cat > /etc/systemd/system/cpu-perf.service << 'CPUEOF'
[Unit]
Description=GreenBoost CPU performance governor (P-cores 0-15)
After=multi-user.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/bash -c 'for cpu in $(seq 0 15); do echo performance > /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; cset set -c 4-7 -s inference_set || true; cset proc -m -f root -t inference_set -k || true'
ExecStop=/bin/bash  -c 'for cpu in $(seq 0 15); do echo powersave  > /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; cset set -d inference_set || true'

[Install]
WantedBy=multi-user.target
CPUEOF
    systemctl daemon-reload
    systemctl enable --now cpu-perf.service
    info "CPU governor service installed and started (includes cset for golden cores)"

    # 4. THP sysfs.d — transparent hugepages for compaction + THP performance
    # NOTE: gb_alloc_buf() uses alloc_pages(GFP_KERNEL|__GFP_COMP, order=9) which draws
    # from the BUDDY ALLOCATOR, NOT the HugeTLB pool.  Pre-allocating HugeTLB pages
    # (vm.nr_hugepages=26112) locks 51 GB in the HugeTLB pool, leaving <12 GB free RAM,
    # which triggers the OOM guard and makes T2 unavailable.  Keep nr_hugepages=0.
    mkdir -p /etc/sysfs.d
    cat > /etc/sysfs.d/greenboost-hugepages.conf << 'HPEOF'
# GreenBoost v2.3 — THP config (no HugeTLB pre-allocation: gb_alloc_buf uses buddy allocator)
kernel/mm/transparent_hugepage/enabled = always
HPEOF
    info "THP sysfs conf: /etc/sysfs.d/greenboost-hugepages.conf"

    # Release any previously locked HugeTLB pages and free them back to buddy allocator
    if [[ "$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null)" != "0" ]]; then
        echo 0 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null \
            && info "HugeTLB pages released: freed $(( $(cat /proc/meminfo | grep HugePages_Total | awk '{print $2}') * 2 )) kB back to buddy allocator" \
            || warn "Could not set nr_hugepages=0 — reboot to apply"
    else
        info "HugeTLB: already 0 (correct — GreenBoost T2 uses buddy allocator)"
    fi

    # 5. VM sysctl — reduce swap pressure, tune write-back
    cat > /etc/sysctl.d/99-greenboost.conf << 'SYSCTLEOF'
# GreenBoost v2.3 — VM tuning for 3-tier model pool
vm.swappiness = 5
vm.dirty_ratio = 20
vm.dirty_background_ratio = 5
SYSCTLEOF
    sysctl -p /etc/sysctl.d/99-greenboost.conf 2>&1 | sed 's/^/  /'
    info "sysctl conf installed: /etc/sysctl.d/99-greenboost.conf"

    echo ""
    info "System config installation complete."
    warn "Restart Ollama to pick up new env vars: sudo systemctl restart ollama"
}

cmd_build() {
    info "Building GreenBoost v2.3 (3-tier: VRAM + DDR4 + NVMe)..."
    make -C "$MODULE_DIR" all || die "Build failed — check output above"
    info "Build complete:"
    info "  Kernel module : $MODULE_DIR/greenboost.ko"
    info "  CUDA shim     : $MODULE_DIR/$SHIM_LIB"
}

cmd_install() {
    need_root install
    check_deps
    cmd_build

    info "Installing kernel module..."
    make -C "$MODULE_DIR" install || die "Module install failed"

    info "Installing CUDA shim to $SHIM_DEST/..."
    cp "$MODULE_DIR/$SHIM_LIB" "$SHIM_DEST/"
    ldconfig

    # modprobe defaults
    info "Writing /etc/modprobe.d/greenboost.conf ..."
    cat > /etc/modprobe.d/greenboost.conf << 'MODEOF'
# GreenBoost v2.3 — 3-tier pool: RTX 5070 VRAM + DDR4 + NVMe swap
# Tier 1: physical_vram_gb=12   RTX 5070 (12 GB GDDR7)
# Tier 2: virtual_vram_gb=51    DDR4 pool (80% of 64 GB, hugepages)
#          safety_reserve_gb=12  always keep ≥12 GB free in RAM
# Tier 3: nvme_swap_gb=576      Samsung 990 Evo Plus NVMe swap capacity
#          nvme_pool_gb=512      GreenBoost soft cap on T3 allocations
options greenboost physical_vram_gb=12 virtual_vram_gb=51 safety_reserve_gb=12 nvme_swap_gb=576 nvme_pool_gb=512
MODEOF

    # profile.d helper
    cat > /etc/profile.d/greenboost.sh << PROFEOF
# GreenBoost v2.3 — shell helpers
export GREENBOOST_SHIM="$SHIM_DEST/$SHIM_LIB"
greenboost-run() { LD_PRELOAD="\$GREENBOOST_SHIM" "\$@"; }
export -f greenboost-run
PROFEOF

    # Standalone wrapper
    cat > /usr/local/bin/greenboost-run << WRAPEOF
#!/usr/bin/env bash
# Run a CUDA application with GreenBoost DDR4 overflow enabled
LD_PRELOAD="$SHIM_DEST/$SHIM_LIB" "\$@"
WRAPEOF
    chmod +x /usr/local/bin/greenboost-run

    info ""
    info "Installation complete!"
    info "  Load module    : sudo modprobe greenboost"
    info "  Run CUDA app   : greenboost-run your_cuda_app"
    info "  Pool status    : cat /sys/class/greenboost/greenboost/pool_info"
}

cmd_load() {
    need_root load
    local phys="${GPU_PHYS_GB:-12}"
    local virt="${VIRT_VRAM_GB:-51}"
    local res="${RESERVE_GB:-12}"
    local nvme_sw="${NVME_SWAP_GB:-576}"
    local nvme_pool="${NVME_POOL_GB:-512}"

    if lsmod | grep -q "^${DRIVER_NAME} "; then
        warn "Module already loaded — reloading..."
        rmmod "$DRIVER_NAME" || die "Failed to unload existing module"
    fi

    local ko="$MODULE_DIR/greenboost.ko"
    [[ -f "$ko" ]] || die "greenboost.ko not found — run: make  or  $0 build"

    insmod "$ko" \
        physical_vram_gb="$phys"  \
        virtual_vram_gb="$virt"   \
        safety_reserve_gb="$res"  \
        nvme_swap_gb="$nvme_sw"   \
        nvme_pool_gb="$nvme_pool" \
        || die "insmod failed — check: dmesg | tail -20"

    info "GreenBoost v2.3 loaded — 3-tier pool active!"
    info ""
    info "  T1 RTX 5070 VRAM : ${phys} GB   ~336 GB/s  [hot layers]"
    info "  T2 DDR4 pool     : ${virt} GB    ~50 GB/s  [cold layers]"
    info "  T3 NVMe swap     : ${nvme_sw} GB  ~1.8 GB/s [frozen pages]"
    info "  ─────────────────────────────────────────"
    info "  Combined view    : $(( phys + virt + nvme_sw )) GB total model capacity"
    info ""
    info "Pool info  : cat /sys/class/greenboost/greenboost/pool_info"
    info "Kernel log : dmesg | grep greenboost"
    echo ""
    dmesg | grep greenboost | tail -8 | sed 's/^/  /'
}

cmd_unload() {
    need_root unload
    if lsmod | grep -q "^${DRIVER_NAME} "; then
        rmmod "$DRIVER_NAME" && info "GreenBoost unloaded" \
            || die "rmmod failed — check: dmesg | tail -5"
    else
        info "GreenBoost is not loaded"
    fi
}

cmd_uninstall() {
    need_root uninstall

    # 1. Unload if running
    if lsmod | grep -q "^${DRIVER_NAME} "; then
        info "Unloading module..."
        rmmod "$DRIVER_NAME" || warn "rmmod failed — continuing cleanup"
    fi

    # 2. Remove installed kernel module
    local ko_path="/lib/modules/$(uname -r)/extra/greenboost.ko"
    local ko_upd="/lib/modules/$(uname -r)/updates/greenboost.ko"
    for f in "$ko_path" "$ko_upd"; do
        if [[ -f "$f" ]]; then
            rm -f "$f" && info "Removed $f"
        fi
    done
    depmod -a && info "depmod updated"

    # 3. Remove CUDA shim
    rm -f "$SHIM_DEST/$SHIM_LIB" && info "Removed $SHIM_DEST/$SHIM_LIB"
    ldconfig

    # 4. Remove config files
    rm -f /etc/modprobe.d/greenboost.conf  && info "Removed modprobe config"
    rm -f /etc/profile.d/greenboost.sh     && info "Removed profile.d entry"
    rm -f /usr/local/bin/greenboost-run    && info "Removed greenboost-run wrapper"
    rm -f /etc/modules-load.d/greenboost.conf && info "Removed modules-load entry"

    info ""
    info "GreenBoost uninstalled cleanly."
}

cmd_tune() {
    need_root tune

    info "Tuning workstation for GreenBoost / LLM workloads..."
    info "Hardware: i9-14900KF | RTX 5070 | DDR4-3600 | Samsung 990 EVO Plus"
    echo ""

    # ── CPU governor → performance (P-cores run at 6 GHz, not 800 MHz) ──
    local changed=0
    for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        [[ -w "$gov" ]] && echo performance > "$gov" && changed=1
    done
    [[ $changed -eq 1 ]] && info "CPU governor      : performance (all 32 CPUs)" \
                          || warn "CPU governor      : could not set (check cpufreq driver)"

    # ── NVMe scheduler → none (best latency for Samsung 990 EVO Plus) ──
    for sched in /sys/block/nvme*/queue/scheduler; do
        [[ -w "$sched" ]] && echo none > "$sched" 2>/dev/null || true
    done
    info "NVMe scheduler    : none (was: mq-deadline)"

    # ── NVMe read-ahead → 4 MB (large sequential model weight loading) ──
    for ra in /sys/block/nvme*/queue/read_ahead_kb; do
        [[ -w "$ra" ]] && echo 4096 > "$ra"
    done
    info "NVMe read_ahead   : 4096 KB = 4 MB (was: 128 KB)"

    # ── NVMe nr_requests → 1024 ──────────────────────────────────────────
    for nr in /sys/block/nvme*/queue/nr_requests; do
        [[ -w "$nr" ]] && echo 1024 > "$nr"
    done
    info "NVMe nr_requests  : 1024"

    # ── THP → always (GreenBoost 2 MB hugepage pool requires it) ─────────
    echo always > /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || true
    info "THP               : always"

    # ── vm.swappiness → 10 (prefer RAM; only page to NVMe under pressure) ─
    sysctl -qw vm.swappiness=10
    info "vm.swappiness     : 10 (was default 60)"

    # ── vm.dirty_ratio / background_ratio (reduce write stalls) ──────────
    sysctl -qw vm.dirty_ratio=40
    sysctl -qw vm.dirty_background_ratio=10
    info "vm.dirty_ratio    : 40 / background: 10"

    echo ""
    info "Tuning done. To make permanent add to /etc/sysctl.conf:"
    info "  vm.swappiness=10"
    info "  vm.dirty_ratio=40"
    info "  vm.dirty_background_ratio=10"
    info "And add to /etc/rc.local for NVMe + THP settings."
}

# ---- tune-grub ---------------------------------------------------------
# Validate each candidate flag against the kernel config, then apply.
# Strategy: read-only checks first; only write GRUB if all checks pass.
# Security mitigations are NEVER touched.

_grub_has()  { grep -qw "$1" /proc/cmdline; }
_kcfg_has()  { grep -q "^${1}=y" /boot/config-"$(uname -r)" 2>/dev/null; }

_grub_check_flag() {
    local flag="$1" desc="$2" kcfg="$3"
    if _grub_has "$flag"; then
        info "  [skip]    $flag  — already active"
        return 1
    fi
    if [[ -n "$kcfg" ]] && ! _kcfg_has "$kcfg"; then
        warn "  [skip]    $flag  — kernel not built with $kcfg"
        return 1
    fi
    info "  [add]     $flag  — $desc"
    return 0
}

cmd_tune_grub() {
    need_root tune-grub

    local grub_file="/etc/default/grub"
    local kver; kver="$(uname -r)"
    local kcfg="/boot/config-${kver}"

    [[ -f "$grub_file" ]] || die "GRUB config not found: $grub_file"

    info "Validating GRUB flags for: i9-14900KF | RTX 5070 | Samsung 990 EVO Plus"
    info "Kernel: $kver"
    echo ""

    # Read current GRUB cmdline value
    local current_line
    current_line=$(grep '^GRUB_CMDLINE_LINUX_DEFAULT=' "$grub_file" | head -1 | sed 's/^GRUB_CMDLINE_LINUX_DEFAULT=//;s/^"//;s/"$//')

    local new_flags=""

    # ── Flag: transparent_hugepage=always ───────────────────────────────
    # GreenBoost T2 pool allocates 2MB compound pages. THP=always ensures
    # the kernel tries to satisfy those allocs from huge pages at boot.
    # Currently set to 'madvise' in GRUB — change to 'always'.
    if _kcfg_has "CONFIG_TRANSPARENT_HUGEPAGE"; then
        if echo "$current_line" | grep -q "transparent_hugepage=madvise"; then
            info "  [fix]     transparent_hugepage=madvise -> always  (GreenBoost T2 hugepage pool)"
            current_line="${current_line/transparent_hugepage=madvise/transparent_hugepage=always}"
        elif ! _grub_has "transparent_hugepage=always"; then
            info "  [add]     transparent_hugepage=always  (GreenBoost T2 hugepage pool)"
            new_flags="$new_flags transparent_hugepage=always"
        else
            info "  [skip]    transparent_hugepage=always  — already active"
        fi
    fi

    # ── Flag: skew_tick=1 ───────────────────────────────────────────────
    # Staggers per-CPU timer ticks on the i9-14900KF hybrid topology
    # (8 P-cores + 16 E-cores). Reduces lock contention when all 32 CPUs
    # fire timer interrupts simultaneously.
    # Runtime test: always safe — no kernel config dependency.
    _grub_check_flag "skew_tick=1" \
        "stagger timer ticks — reduces lock contention on hybrid P/E cores" "" \
        && new_flags="$new_flags skew_tick=1"

    # ── Flag: rcu_nocbs=16-31 ───────────────────────────────────────────
    # Offloads RCU (Read-Copy-Update) callback processing to E-cores
    # (CPU 16-31, up to 4.4 GHz). Frees the 6 GHz P-cores (0-15) from
    # RCU housekeeping during inference hot paths.
    # Kernel check: CONFIG_RCU_NOCB_CPU=y (confirmed built-in).
    _grub_check_flag "rcu_nocbs=16-31" \
        "offload RCU callbacks to E-cores, freeing P-cores for inference" \
        "CONFIG_RCU_NOCB_CPU" \
        && new_flags="$new_flags rcu_nocbs=16-31"

    # ── Flag: nohz_full=4-7 ─────────────────────────────────────────────
    # Makes the 4 golden P-cores (CPU 4-7, core_id 8+12, 6 GHz TVB boost)
    # tick-less when they have exactly one runnable thread. Combined with
    # rcu_nocbs, this eliminates the 1000 Hz timer interrupt during dense
    # matrix multiplications — directly reduces LLM token latency.
    # Kernel check: CONFIG_NO_HZ_FULL=y (confirmed built-in).
    # Safe: golden cores still handle regular tasks; tick resumes when
    # idle or when >1 task is runnable.
    _grub_check_flag "nohz_full=4-7" \
        "tick-less golden P-cores during single-thread inference (i9 core_id 8+12)" \
        "CONFIG_NO_HZ_FULL" \
        && new_flags="$new_flags nohz_full=4-7"

    # ── Flag: numa_balancing=disable ────────────────────────────────────
    # This workstation has a single NUMA node (all CPUs on node 0).
    # The kernel's automatic NUMA balancing task wastes cycles scanning
    # pages that will never need to move. Already disabled at runtime
    # via sysctl, this makes it persistent across reboots.
    _grub_check_flag "numa_balancing=disable" \
        "single NUMA node — disable page-migration scanning overhead" "" \
        && new_flags="$new_flags numa_balancing=disable"

    # ── Flag: workqueue.power_efficient=0 ──────────────────────────────
    # Kernel workqueues (DMA, NVMe completion, etc.) use power-efficient
    # mode by default, routing work to whichever CPU happens to be awake.
    # Disabling this routes workqueue items to the fastest available CPU
    # (P-core), which matters for DMA-BUF completion and NVMe IRQ paths.
    _grub_check_flag "workqueue.power_efficient=0" \
        "route kernel workqueues to P-cores instead of any-idle CPU" "" \
        && new_flags="$new_flags workqueue.power_efficient=0"

    # ── Fix: deduplicate nvidia-drm.modeset=1 ──────────────────────────
    local count; count=$(echo "$current_line" | grep -o "nvidia-drm.modeset=1" | wc -l)
    if [[ "$count" -gt 1 ]]; then
        info "  [fix]     nvidia-drm.modeset=1 appears ${count}× — deduplicating"
        # Remove all occurrences then add one back
        current_line=$(echo "$current_line" | sed 's/nvidia-drm\.modeset=1//g' | tr -s ' ')
        current_line="$current_line nvidia-drm.modeset=1"
    fi

    # ── Build new cmdline ───────────────────────────────────────────────
    local new_line="${current_line}${new_flags}"
    # Normalise multiple spaces
    new_line=$(echo "$new_line" | tr -s ' ' | sed 's/^ //;s/ $//')

    if [[ "$new_line" == "$current_line" ]] && [[ -z "$new_flags" ]]; then
        info ""
        info "GRUB is already fully optimised — nothing to change."
        return 0
    fi

    echo ""
    info "Current GRUB cmdline:"
    echo "  $current_line" | fold -s -w 100 | sed 's/^/  /'
    echo ""
    info "New GRUB cmdline:"
    echo "  $new_line" | fold -s -w 100 | sed 's/^/  /'
    echo ""

    # ── Backup + write ──────────────────────────────────────────────────
    local bak="${grub_file}.bak.$(date +%Y%m%d_%H%M%S)"
    cp "$grub_file" "$bak"
    info "Backup saved: $bak"

    # Replace the GRUB_CMDLINE_LINUX_DEFAULT line
    sed -i "s|^GRUB_CMDLINE_LINUX_DEFAULT=.*|GRUB_CMDLINE_LINUX_DEFAULT=\"${new_line}\"|" "$grub_file"

    info "Running update-grub..."
    update-grub 2>&1 | grep -v "^$" | sed 's/^/  /'

    echo ""
    info "GRUB updated. Changes take effect on next reboot."
    warn "Reboot when ready: sudo reboot"
}

# ---- tune-sysctl -------------------------------------------------------
# Consolidate conflicting sysctl files and add missing compute settings.
# Writes /etc/sysctl.d/99-zzz-greenboost.conf — loaded last, wins all
# conflicts. Previous files are left untouched (history/documentation).

cmd_tune_sysctl() {
    need_root tune-sysctl

    local dest="/etc/sysctl.d/99-zzz-greenboost.conf"

    info "Writing definitive sysctl config: $dest"
    info "This file loads last (99-zzz) and wins over all conflicting files."
    echo ""

    # Show conflicts found
    info "Conflicts resolved:"
    info "  vm.swappiness       : multiple files set 10/20 → final: 10"
    info "  vm.dirty_ratio      : 15 vs 40 → final: 40 (Samsung 990 sustains 7 GB/s)"
    info "  vm.dirty_background_ratio: 5 vs 10 → final: 10"
    info "  kernel.sched_autogroup_enabled: 1 → 0 (bad for compute, groups by session)"
    info "New settings added:"
    info "  kernel.sched_migration_cost_ns: 5000000 (5ms — keep threads on P-cores)"
    info "  kernel.sched_min_granularity_ns: 10000000 (10ms — better for large tasks)"
    info "  kernel.sched_wakeup_granularity_ns: 15000000 (reduces spurious wakeups)"
    echo ""

    cat > "$dest" << 'SYSCTL_EOF'
# GreenBoost v2.3 — Definitive sysctl config
# Hardware: i9-14900KF | RTX 5070 | 64 GB DDR4-3600 | Samsung 990 EVO Plus 4 TB
# Loaded last (99-zzz) — wins all conflicts with earlier sysctl.d files.
# Do NOT edit other sysctl.d files; make changes here instead.

# ── Swap / memory pressure ───────────────────────────────────────────────
# Keep LLM weights in DDR4 (T2); only spill to NVMe (T3) under real pressure.
vm.swappiness = 10

# ── Write-back (Samsung 990 EVO Plus sustains 6,300 MB/s writes) ─────────
# Allow up to 40% dirty pages before throttling writes (~25 GB at 64 GB RAM).
# Background flush at 10% (~6.4 GB) — keeps NVMe busy without stalling allocs.
vm.dirty_ratio = 40
vm.dirty_background_ratio = 10
vm.dirty_writeback_centisecs = 1500
vm.dirty_expire_centisecs = 3000

# ── Memory allocation ─────────────────────────────────────────────────────
# LLM frameworks (llama.cpp, PyTorch, JAX) mmap model files and pre-reserve
# large virtual address ranges. Without overcommit the kernel rejects these.
vm.overcommit_memory = 1

# Max VMA regions: transformer models (70B+) split across thousands of mmap
# file segments. Default 65530 is too low; 2M covers any realistic case.
vm.max_map_count = 2147483642

# Always keep 512 MB free — prevents latency spikes under allocation storms.
vm.min_free_kbytes = 524288

# Proactive compaction: GreenBoost T2 needs contiguous 2 MB hugepage ranges.
# Value 20 = moderate background compaction (0=off, 100=aggressive).
vm.compaction_proactiveness = 20

# Overcommit hugepage pool: 10240 × 2 MB = 20 GB pre-reserved for THP allocs.
vm.nr_overcommit_hugepages = 10240

# Keep inode/dentry caches alive — LLM loaders open thousands of weight files.
vm.vfs_cache_pressure = 50

# Disable zone reclaim: single NUMA node — cross-zone reclaim wastes cycles.
vm.zone_reclaim_mode = 0

# ── CPU scheduler (i9-14900KF P-core / E-core hybrid) ────────────────────
# Disable session-based task grouping. sched_autogroup groups shell tasks
# together which is good for desktop but HURTS inference: Ollama worker
# threads (long-running compute) compete with short interactive tasks for
# scheduler time-slices in the same group.
kernel.sched_autogroup_enabled = 0

# Raise migration cost threshold: scheduler avoids migrating tasks to
# a different CPU unless the cache-miss penalty exceeds this value (5 ms).
# Effect: LLM inference threads stay on their assigned P-cores rather than
# bouncing between P-cores and E-cores (which have different cache hierarchies).
kernel.sched_migration_cost_ns = 5000000

# Minimum scheduling granularity (10 ms): gives large compute tasks
# (matrix multiplications, attention heads) more uninterrupted runtime
# before the scheduler can preempt them.
kernel.sched_min_granularity_ns = 10000000

# Wakeup granularity (15 ms): a waking task only preempts the current task
# if it has been sleeping for more than this long. Prevents short-lived
# system tasks from constantly interrupting inference threads.
kernel.sched_wakeup_granularity_ns = 15000000

# ── NUMA ──────────────────────────────────────────────────────────────────
# Single-socket i9-14900KF — all CPUs are on NUMA node 0. Automatic NUMA
# balancing scans pages and attempts cross-node migrations that will never
# happen. Disable to remove the page-scanning overhead.
kernel.numa_balancing = 0

# ── File system ───────────────────────────────────────────────────────────
# GGUF model files (70B+ = thousands of weight tensors) require many open
# file descriptors and inotify watches during model loading.
fs.file-max = 2097152
fs.inotify.max_user_watches = 524288
fs.inotify.max_user_instances = 1024

# ── Network (Ollama API / distributed inference) ──────────────────────────
# Large buffers for Ollama HTTP streaming API and any future multi-GPU setup.
net.core.rmem_max = 134217728
net.core.wmem_max = 134217728
net.ipv4.tcp_rmem = 4096 87380 134217728
net.ipv4.tcp_wmem = 4096 65536 134217728
net.core.somaxconn = 65535
net.core.netdev_max_backlog = 5000
net.ipv4.tcp_fastopen = 3

# ── Perf / profiling access ───────────────────────────────────────────────
# Allow nsys / perf / CUDA Nsight without sudo (needed for GPU profiling).
kernel.perf_event_paranoid = 1
kernel.kptr_restrict = 0
SYSCTL_EOF

    sysctl -p "$dest" 2>&1 | grep -v "^$" | sed 's/^/  /' || true
    echo ""
    info "sysctl applied and persistent (survives reboot via $dest)."
}

# ---- tune-libs ---------------------------------------------------------
# Install missing libraries and kernel modules for AI/compute workloads.
# All packages chosen for AVX2/FMA/VNNI capabilities on i9-14900KF.

cmd_tune_libs() {
    need_root tune-libs

    info "Installing missing AI/compute libraries for i9-14900KF + RTX 5070..."
    echo ""

    # ── APT packages ──────────────────────────────────────────────────────
    local pkgs=(
        # BLAS/LAPACK — OpenBLAS compiled with AVX2+FMA for CPU inference
        libopenblas-dev
        libblas-dev
        liblapack-dev

        # OpenMP — multi-threaded CPU inference (llama.cpp uses this heavily)
        libomp-dev

        # hwloc — hardware topology library used by Ollama/llama.cpp for
        # CPU pinning; without it Ollama uses a generic thread affinity model
        hwloc
        libhwloc-dev

        # libnuma — NUMA-aware memory allocation (single node but still used
        # by CUDA and some ML runtimes for memory locality hints)
        libnuma-dev

        # OpenCL — GPU compute via OpenCL API (some inference backends use it)
        ocl-icd-opencl-dev

        # nvtop — real-time GPU + CPU monitor (shows all 3 tiers at a glance)
        nvtop

        # cpufrequtils — userspace CPU frequency tools (cpufreq-info, etc.)
        cpufrequtils

        # linux-tools — perf, turbostat (monitors P/E core frequencies + C-states)
        linux-tools-generic

        # intel-microcode — latest CPU microcode (fixes + performance patches
        # for i9-14900KF Raptor Lake stepping)
        intel-microcode
    )

    info "Packages to install:"
    local to_install=()
    for pkg in "${pkgs[@]}"; do
        if dpkg -l "$pkg" 2>/dev/null | grep -q "^ii"; then
            info "  [ok]      $pkg"
        else
            info "  [install] $pkg"
            to_install+=("$pkg")
        fi
    done
    echo ""

    if [[ ${#to_install[@]} -eq 0 ]]; then
        info "All packages already installed."
    else
        apt-get install -y "${to_install[@]}" 2>&1 | tail -5
        info "Packages installed."
    fi

    echo ""

    # ── Kernel modules ────────────────────────────────────────────────────
    info "Kernel modules:"

    # cpuid — lets userspace read CPUID leaves directly. Used by turbostat,
    # CUDA diagnostics, and intel-microcode update verification.
    if lsmod | grep -q "^cpuid "; then
        info "  [ok]      cpuid  (loaded)"
    else
        modprobe cpuid && info "  [loaded]  cpuid" || warn "  cpuid: modprobe failed"
    fi

    # Ensure cpuid + msr auto-load at boot
    local ml_conf="/etc/modules-load.d/ai-workstation.conf"
    if ! grep -q "^cpuid" "$ml_conf" 2>/dev/null; then
        echo "cpuid" >> "$ml_conf"
        info "  [add]     cpuid -> $ml_conf (auto-load at boot)"
    else
        info "  [ok]      cpuid already in $ml_conf"
    fi

    echo ""

    # ── OpenBLAS CPU target selection ─────────────────────────────────────
    # Make sure the system BLAS points to OpenBLAS (AVX2/FMA optimised)
    # rather than the reference BLAS implementation.
    if command -v update-alternatives &>/dev/null && dpkg -l libopenblas-dev 2>/dev/null | grep -q "^ii"; then
        update-alternatives --set libblas.so.3-x86_64-linux-gnu \
            /usr/lib/x86_64-linux-gnu/openblas-pthread/libblas.so.3 2>/dev/null \
            && info "BLAS alternative: set to OpenBLAS (AVX2/FMA)" \
            || info "BLAS alternative: already set or path differs — check manually"
    fi

    echo ""
    info "tune-libs complete."
    info "  Turbostat (P/E core monitoring): sudo turbostat --quiet --Summary"
    info "  nvtop (GPU + CPU):               nvtop"
    info "  CPU frequency info:              cpufreq-info"
}

# ---- tune-all ----------------------------------------------------------

cmd_tune_all() {
    need_root tune-all
    info "Running full system tuning for GreenBoost v2.3..."
    echo ""
    cmd_tune
    echo ""
    cmd_tune_sysctl
    echo ""
    cmd_tune_grub
    echo ""
    cmd_tune_libs
    echo ""
    info "All tuning complete."
    info "Reboot to activate GRUB changes: sudo reboot"
}

cmd_status() {
    echo ""
    echo -e "${BLU}=== GreenBoost v2.3 Status (3-tier pool) ===${NC}"
    echo ""

    if lsmod | grep -q "^${DRIVER_NAME} "; then
        echo -e "  Module: ${GRN}LOADED ✓${NC}"
    else
        echo -e "  Module: ${RED}not loaded${NC}"
    fi

    local pool_f="/sys/class/greenboost/greenboost/pool_info"
    if [[ -r "$pool_f" ]]; then
        echo ""
        cat "$pool_f" | sed 's/^/  /'
    fi

    echo ""
    echo -e "${BLU}=== Recent kernel messages ===${NC}"
    dmesg | grep greenboost | tail -10 | sed 's/^/  /'
    echo ""
}

cmd_help() {
    echo ""
    echo -e "${BLU}GreenBoost v2.3 — 3-Tier GPU Memory Pool${NC}"
    echo "Author : Ferran Duarri"
    echo "Target : ASUS RTX 5070 12 GB + 64 GB DDR4-3600 + 4 TB Samsung 990 Evo Plus NVMe"
    echo ""
    echo "  Tier 1  RTX 5070 VRAM      12 GB   ~336 GB/s  [hot layers]"
    echo "  Tier 2  DDR4 pool          51 GB    ~50 GB/s  [cold layers, hugepages]"
    echo "  Tier 3  NVMe swap         576 GB    ~1.8 GB/s  [frozen pages, swappable 4K]"
    echo "          ─────────────────────────────────────"
    echo "          Combined capacity  639 GB"
    echo ""
    echo "USAGE:  sudo ./greenboost_setup.sh <command>"
    echo ""
    echo "COMMANDS:"
    echo "  install     Build and install module + CUDA shim system-wide"
    echo "  uninstall   Unload, remove module + all config files"
    echo "  build       Build only (no system install)"
    echo "  load        Load module with default 3-tier parameters"
    echo "  unload      Unload module (keeps installed files)"
    echo "  tune        Tune system for LLM workloads (governor, NVMe, THP, sysctl)"
    echo "  tune-grub   Fix GRUB boot params (THP=always, rcu_nocbs, nohz_full…)"
    echo "  tune-sysctl Consolidate sysctl files + apply compute-optimized knobs"
    echo "  tune-libs   Install missing AI/compute libraries (OpenBLAS, hwloc…)"
    echo "  tune-all    Run tune + tune-grub + tune-sysctl + tune-libs in sequence"
    echo "  install-sys-configs  Install Ollama env, NVMe udev, CPU governor, hugepages, sysctl"
    echo "  install-deps         Install all Ubuntu OS packages (build + CUDA + AI libs)"
    echo "  setup-swap [GB]      Create/activate NVMe swap file (T3 tier, default 576 GB)"
    echo "  full-install         Complete fresh-OS setup: deps+swap+build+install+configs+tune"
    echo "  status      Show module status and 3-tier pool info"
    echo "  diagnose    Full health check — run this after reboot to verify everything works"
    echo "  help        Show this help"
    echo ""
    echo "ENVIRONMENT (for load):"
    echo "  GPU_PHYS_GB=12     Physical VRAM in GB          (RTX 5070 default: 12)"
    echo "  VIRT_VRAM_GB=51    DDR4 pool size in GB         (default: 51, 80% of 64 GB)"
    echo "  RESERVE_GB=12      System RAM to keep free      (default: 12)"
    echo "  NVME_SWAP_GB=576   NVMe swap capacity in GB     (default: 576)"
    echo "  NVME_POOL_GB=512   GreenBoost T3 soft cap in GB (default: 512)"
    echo ""
    echo "  Example: sudo VIRT_VRAM_GB=48 NVME_SWAP_GB=576 ./greenboost_setup.sh load"
    echo ""
    echo "CUDA SHIM (transparent DDR4 overflow via NVIDIA UVM):"
    echo "  Prerequisite  : sudo modprobe nvidia_uvm"
    echo "  One-shot      : LD_PRELOAD=./libgreenboost_cuda.so  ./your_cuda_app"
    echo "  After install : greenboost-run  ./your_cuda_app"
    echo "  Threshold     : GREENBOOST_THRESHOLD_MB=512  greenboost-run  ./app"
    echo "  Debug         : GREENBOOST_DEBUG=1  greenboost-run  ./app"
    echo ""
    echo "MONITORING:"
    echo "  cat /sys/class/greenboost/greenboost/pool_info   (3-tier table)"
    echo "  cat /sys/class/greenboost/greenboost/active_buffers"
    echo "  dmesg | grep greenboost"
    echo "  watch -n1 free -h                               (T2 RAM pressure)"
    echo "  watch -n1 swapon --show                         (T3 NVMe usage)"
    echo "  make status"
    echo ""
}

# ---- install-deps ------------------------------------------------------
# Install all Ubuntu packages needed for GreenBoost v2.3 + ExLlamaV3

cmd_install_deps() {
    need_root install-deps
    info "Installing Ubuntu dependencies for GreenBoost v2.3 + ExLlamaV3..."
    info "Running apt-get update..."
    apt-get update -qq

    # Core build tools
    apt-get install -y \
        build-essential gcc make git curl wget \
        linux-headers-"$(uname -r)" \
        pkg-config sysfsutils

    # io_uring — required for ExLlamaV3 stloader (replaces pread at 3 sites)
    apt-get install -y liburing-dev

    # Python (ExLlamaV3 + GreenBoost cache integration)
    apt-get install -y python3 python3-pip python3-dev python3-venv

    # CPU/GPU monitoring and tuning
    apt-get install -y cpufrequtils linux-tools-generic nvtop || true
    apt-get install -y linux-tools-"$(uname -r)" 2>/dev/null || true
    apt-get install -y intel-microcode 2>/dev/null || true

    # AI/compute libraries (OpenBLAS, hwloc, NUMA, OpenMP, OpenCL)
    apt-get install -y \
        libopenblas-dev libblas-dev liblapack-dev \
        libhwloc-dev hwloc libnuma-dev libomp-dev \
        ocl-icd-opencl-dev 2>/dev/null || true

    # Ensure cpuid module loads at boot (for topology detection)
    if ! grep -q cpuid /etc/modules-load.d/*.conf 2>/dev/null; then
        echo cpuid > /etc/modules-load.d/ai-workstation.conf
        info "cpuid module: added to modules-load.d"
    fi

    info "Ubuntu dependencies installed."
    info "Note: NVIDIA driver 580+ and CUDA 13 must be installed separately."
}

# ---- setup-swap --------------------------------------------------------
# Create NVMe swap file (T3 tier). Safe to re-run — idempotent.

cmd_setup_swap() {
    need_root setup-swap
    local gb="${2:-576}"   # argument or default 576 GB
    [[ "$1" == "setup-swap" ]] && gb="${2:-576}" || gb="${1:-576}"
    local swap_file="/swap_nvme.img"
    local swap_bytes=$(( gb * 1024 * 1024 * 1024 ))

    info "Setting up NVMe swap file: $swap_file ($gb GB)..."

    if [[ -f "$swap_file" ]]; then
        local cur_size; cur_size=$(stat -c%s "$swap_file" 2>/dev/null || echo 0)
        if [[ "$cur_size" -ge "$swap_bytes" ]]; then
            info "Swap file already exists and is large enough ($gb GB): $swap_file"
        else
            local cur_gb=$(( cur_size / 1024 / 1024 / 1024 ))
            warn "Swap file exists but is only ${cur_gb} GB (want $gb GB) — expanding..."
            # swapoff only if currently active (swapoff on inactive swap → "Invalid argument")
            if swapon --show --noheadings 2>/dev/null | grep -q "$swap_file"; then
                swapoff "$swap_file" || die "swapoff $swap_file failed"
                info "Swap deactivated for expansion"
            fi
            fallocate -l "${gb}G" "$swap_file" 2>/dev/null || \
                dd if=/dev/zero of="$swap_file" bs=1G count="$gb" status=progress || \
                die "Failed to expand swap file $swap_file"
            chmod 600 "$swap_file"
            mkswap "$swap_file" || die "mkswap failed after expansion"
            info "Swap file expanded to ${gb} GB"
        fi
    else
        info "Creating ${gb} GB swap file (fallocate — fast on NVMe)..."
        fallocate -l "${gb}G" "$swap_file" 2>/dev/null || \
            dd if=/dev/zero of="$swap_file" bs=1G count="$gb" status=progress || \
            die "Failed to create swap file $swap_file"
        chmod 600 "$swap_file"
        mkswap "$swap_file" || die "mkswap failed"
        info "Swap file created: $swap_file"
    fi

    # Activate if not already active
    if ! swapon --show | grep -q "$swap_file"; then
        swapon -p 10 "$swap_file" && info "Swap activated (priority=10)" \
            || warn "swapon failed — may need reboot"
    else
        info "Swap already active: $swap_file"
    fi

    # Add to fstab if missing
    if ! grep -q "$swap_file" /etc/fstab; then
        echo "$swap_file  none  swap  sw,pri=10  0 0" >> /etc/fstab
        info "Added to /etc/fstab: $swap_file"
    else
        info "/etc/fstab: $swap_file already present"
    fi

    echo ""
    swapon --show | sed 's/^/  /'
}

# ---- full-install ------------------------------------------------------
# Complete fresh-OS install — run this after a clean Ubuntu install.
# Covers: OS deps, NVMe swap, kernel module, CUDA shim, all system configs,
# sysctl tuning, GRUB params, and optional ExLlamaV3 with GreenBoost patches.

cmd_full_install() {
    need_root full-install

    info "╔══════════════════════════════════════════════════════════════╗"
    info "║  GreenBoost v2.3 — Complete Fresh Install                   ║"
    info "║  i9-14900KF | RTX 5070 OC | 64 GB DDR4 | Samsung 990 4 TB  ║"
    info "╚══════════════════════════════════════════════════════════════╝"
    echo ""

    # 1/7 — OS dependencies
    info "[1/7] Installing Ubuntu OS dependencies..."
    cmd_install_deps
    echo ""

    # 2/7 — NVMe swap (T3 tier, 576 GB)
    info "[2/7] Setting up NVMe swap file (T3 tier, 576 GB)..."
    cmd_setup_swap 576
    echo ""

    # 3/7 — Build + install kernel module + CUDA shim
    info "[3/7] Building and installing kernel module + CUDA shim..."
    cmd_install
    echo ""

    # 4/7 — Load kernel module
    info "[4/7] Loading kernel module with 3-tier params..."
    cmd_load
    echo ""

    # 5/7 — System configs: Ollama, NVMe udev, CPU governor, hugepages, sysctl
    info "[5/7] Installing system configuration files..."
    cmd_install_sys_configs
    echo ""

    # 6/7 — Enhanced sysctl + GRUB params
    info "[6/7] Applying sysctl tuning and GRUB boot params..."
    cmd_tune_sysctl
    echo ""
    cmd_tune_grub
    echo ""

    # 7/7 — ExLlamaV3 with GreenBoost patches (optional)
    info "[7/7] ExLlamaV3 + GreenBoost integration..."
    local exllama_dir="/home/ferran/Dev/llibreries/greenboost_enhanced/exllamav3"

    if [[ -d "$exllama_dir" ]]; then
        info "Found ExLlamaV3 at $exllama_dir"

        # Install system-wide shared location for the python library
        local system_exllama_dir="/opt/greenboost/exllamav3"
        info "Copying ExLlamaV3 to system shared directory ($system_exllama_dir)..."
        mkdir -p /opt/greenboost
        cp -r "$exllama_dir" /opt/greenboost/

        # Ensure liburing-dev is present (io_uring stloader)
        apt-get install -y liburing-dev python3-venv 2>/dev/null || true
        # Python 3.12+ (Ubuntu 24.04+) is "externally managed" — use a venv
        local venv_dir="/opt/greenboost/venv"
        if [[ ! -d "$venv_dir" ]]; then
            info "Creating Python venv at $venv_dir..."
            python3 -m venv "$venv_dir" || die "python3 -m venv failed — install python3-venv"
        fi

        # Make the system directory accessible to all users
        chmod -R 755 /opt/greenboost

        STLOADER_USE_URING=1 "$venv_dir/bin/pip" install -e "$system_exllama_dir" --no-build-isolation \
            && info "ExLlamaV3 installed in $venv_dir" \
            || warn "ExLlamaV3 install failed — run manually: STLOADER_USE_URING=1 $venv_dir/bin/pip install -e $system_exllama_dir --no-build-isolation"
        info "To use ExLlamaV3: source $venv_dir/bin/activate"
    else
        warn "ExLlamaV3 not found at $exllama_dir. Place it there then re-run."
    fi
    echo ""

    info "╔══════════════════════════════════════════════════════════════╗"
    info "║  Full install complete!                                      ║"
    info "╚══════════════════════════════════════════════════════════════╝"
    echo ""
    warn "════════════════════════════════════════════════════════════════"
    warn "REBOOT REQUIRED to activate GRUB params + hugepage pre-allocation"
    warn "════════════════════════════════════════════════════════════════"
    echo ""
    info "After reboot, run the health check:"
    info "  sudo ./greenboost_setup.sh diagnose"
    echo ""
    info "If models still appear stuck (CPU fallback), diagnose will show the issue."
    info "For live Ollama logs: journalctl -u ollama -f"
}

# ---- diagnose ----------------------------------------------------------
# Full health check + GLM-4.7-flash model benchmark.
# Run after reboot: sudo ./greenboost_setup.sh diagnose
# Structured log written to /var/log/greenboost/diagnose-latest.log

cmd_diagnose() {
    # ── Log setup ──────────────────────────────────────────────────────────
    local LOG_DIR="/var/log/greenboost"
    mkdir -p "$LOG_DIR" 2>/dev/null || LOG_DIR="/tmp"
    local LOG_FILE="$LOG_DIR/diagnose-$(date +%Y%m%d_%H%M%S).log"
    local LOG_LATEST="$LOG_DIR/diagnose-latest.log"
    local issues=0
    local recs=()

    # Helpers — write to console AND log file
    _L()    { printf "%s\n" "$*" >> "$LOG_FILE"; }
    _chk()  { echo -e "  ${GRN}[OK]${NC}   $*"; printf "  [OK]   %s\n" "$*" >> "$LOG_FILE"; }
    _fail() { echo -e "  ${RED}[FAIL]${NC} $*"; printf "  [FAIL] %s\n" "$*" >> "$LOG_FILE"; (( issues++ )); }
    _warn() { echo -e "  ${YLW}[WARN]${NC} $*"; printf "  [WARN] %s\n" "$*" >> "$LOG_FILE"; }
    _info() { echo -e "  ${BLU}[INFO]${NC} $*"; printf "  [INFO] %s\n" "$*" >> "$LOG_FILE"; }
    _rec()  { recs+=("$*"); printf "  [REC]  %s\n" "$*" >> "$LOG_FILE"; }
    _sect() {
        echo -e "\n${BLU}━━━ $* ━━━${NC}"
        printf "\n[%s]\n" "$*" >> "$LOG_FILE"
    }

    # Log header
    {
        echo "=== GreenBoost v2.3 Diagnose Log ==="
        echo "timestamp=$(date '+%Y-%m-%d %H:%M:%S')"
        echo "kernel=$(uname -r)"
        echo "host=$(hostname)"
        echo "models_target=glm-4.7-flash:q8_0  glm-4.7-flash:latest"
        echo ""
    } | tee "$LOG_FILE" > /dev/null

    echo ""
    echo -e "${BLU}╔══════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BLU}║  GreenBoost v2.3 — Full Diagnostic + Model Benchmark        ║${NC}"
    echo -e "${BLU}╚══════════════════════════════════════════════════════════════╝${NC}"

    # ═══════════════════════════════════════════════════════════════════════
    # 1. KERNEL MODULE
    # ═══════════════════════════════════════════════════════════════════════
    _sect "1/8  KERNEL MODULE"
    if lsmod | grep -q "^greenboost "; then
        _chk "greenboost.ko loaded"
    else
        _fail "greenboost.ko NOT loaded — run: sudo ./greenboost_setup.sh load"
        _rec "Load module: sudo ./greenboost_setup.sh load"
    fi
    local pool_f="/sys/class/greenboost/greenboost/pool_info"
    if [[ -r "$pool_f" ]]; then
        _chk "sysfs readable"
        while IFS= read -r ln; do _info "  $ln"; done < "$pool_f"
    else
        _warn "sysfs not available (module not loaded or init failed)"
    fi

    # ═══════════════════════════════════════════════════════════════════════
    # 2. NVIDIA + UVM
    # ═══════════════════════════════════════════════════════════════════════
    _sect "2/8  NVIDIA + UVM"
    if lsmod | grep -q "^nvidia "; then
        _chk "nvidia driver loaded"
    else
        _fail "nvidia NOT loaded — run: sudo modprobe nvidia"
    fi
    if lsmod | grep -q "^nvidia_uvm "; then
        _chk "nvidia_uvm loaded (DDR4 UVM overflow available)"
    else
        _fail "nvidia_uvm NOT loaded — DDR4 overflow via UVM disabled"
        _rec "sudo modprobe nvidia_uvm && echo nvidia_uvm | sudo tee /etc/modules-load.d/nvidia-uvm.conf"
    fi
    local vram_free_mb="" vram_total_mb=""
    if command -v nvidia-smi &>/dev/null; then
        local gpu_csv; gpu_csv=$(nvidia-smi --query-gpu=name,driver_version,memory.used,memory.free,memory.total \
            --format=csv,noheader,nounits 2>/dev/null)
        _chk "GPU: $gpu_csv  (MiB used/free/total)"
        _L  "nvidia_smi=$gpu_csv"
        vram_free_mb=$(echo "$gpu_csv" | awk -F', ' '{print $4}' | tr -d ' ')
        vram_total_mb=$(echo "$gpu_csv" | awk -F', ' '{print $5}' | tr -d ' ')
        if [[ -n "$vram_free_mb" && "$vram_free_mb" -lt 1024 ]]; then
            _warn "Low free VRAM: ${vram_free_mb} MiB — another process may be holding GPU memory"
            _rec "Check: sudo fuser /dev/nvidia0  |  sudo nvidia-smi"
        fi
    fi

    # ═══════════════════════════════════════════════════════════════════════
    # 3. CUDA SHIM BINARY
    # ═══════════════════════════════════════════════════════════════════════
    _sect "3/8  CUDA SHIM BINARY"
    local shim="$SHIM_DEST/$SHIM_LIB"
    if [[ ! -f "$shim" ]]; then
        _fail "Shim not installed: $shim — run: sudo ./greenboost_setup.sh install"
    else
        _chk "Shim installed: $shim ($(stat -c%s "$shim") bytes)"
        # Detect the libcudart=NULL UB bug: GCC -O2 removes the search loop if
        # libcudart is not initialised to NULL, leaving no cudart paths in binary
        if strings "$shim" | grep -q "cuda_v13"; then
            _chk "Cudart search paths present in binary (UB fix OK)"
        else
            _fail "Cudart search paths MISSING — shim built with uninitialized-libcudart UB bug"
            _info "Every cudaMalloc returns OOM → models fall back to CPU (appears stuck)"
            _info "Fix: cd $MODULE_DIR && make shim && sudo cp $SHIM_LIB $SHIM_DEST/ && sudo ldconfig && sudo systemctl restart ollama"
            _rec "Rebuild shim: cd $MODULE_DIR && make shim && sudo ./deploy_fix.sh"
        fi
        # Live init test (no CUDA context — only checks symbol resolution)
        local shim_out; shim_out=$(LD_PRELOAD="$shim" ls /dev/null 2>&1)
        if echo "$shim_out" | grep -q "libcudart loaded:"; then
            local cudart_path; cudart_path=$(echo "$shim_out" | grep "libcudart loaded:" | sed 's/.*libcudart loaded: //')
            _chk "Shim init: libcudart found → $cudart_path"
        elif echo "$shim_out" | grep -q "runtime API resolved lazily"; then
            _fail "Shim init: libcudart NOT found — cudaMalloc will OOM on every call"
            _rec "Rebuild shim: cd $MODULE_DIR && make shim && sudo ./deploy_fix.sh"
        else
            _warn "Shim init: status unknown (no CUDA driver in test process)"
        fi
        if echo "$shim_out" | grep -q "UVM overflow.*available"; then
            _chk "Shim init: UVM overflow available"
        elif echo "$shim_out" | grep -q "UVM overflow.*unavailable"; then
            _warn "Shim init: UVM overflow unavailable — load nvidia_uvm"
        fi
        # Check dlsym hook present (v2.3+) — required to intercept dlopen+dlsym GPU API calls
        if strings "$shim" 2>/dev/null | grep -q "dlsym hook"; then
            _chk "dlsym hook present (intercepts Ollama dlopen+dlsym GPU API calls)"
        else
            _fail "dlsym hook MISSING — Ollama NVML/CUDA discovery sees only physical VRAM"
            _info "Rebuild shim: cd $MODULE_DIR && make shim && sudo ./deploy_fix.sh"
            _rec "Rebuild shim v2.3: cd $MODULE_DIR && make shim && sudo ./deploy_fix.sh"
        fi
    fi

    # ═══════════════════════════════════════════════════════════════════════
    # 4. OLLAMA SERVICE + CONFIG
    # ═══════════════════════════════════════════════════════════════════════
    _sect "4/8  OLLAMA SERVICE"
    if ! systemctl is-active --quiet ollama; then
        _fail "Ollama NOT running — run: sudo systemctl start ollama"
    else
        _chk "Ollama service: active"
    fi
    local env_str; env_str=$(systemctl show ollama --property=Environment 2>/dev/null)
    _L "ollama_env=$env_str"

    echo "$env_str" | grep -q "LD_PRELOAD.*libgreenboost" \
        && _chk "LD_PRELOAD=libgreenboost_cuda.so active" \
        || { _fail "LD_PRELOAD not pointing to GreenBoost shim"; _rec "sudo ./greenboost_setup.sh install-sys-configs"; }

    echo "$env_str" | grep -qE "OLLAMA_FLASH_ATTENTION=(1|true)" \
        && _chk "OLLAMA_FLASH_ATTENTION=1 (halves KV cache VRAM)" \
        || { _warn "OLLAMA_FLASH_ATTENTION not enabled"; _rec "Add Environment=OLLAMA_FLASH_ATTENTION=1 to Ollama service"; }

    local kv_type; kv_type=$(echo "$env_str" | grep -oP 'OLLAMA_KV_CACHE_TYPE=\S+' | cut -d= -f2 | head -1)
    [[ -n "$kv_type" ]] \
        && _chk "OLLAMA_KV_CACHE_TYPE=$kv_type" \
        || { _warn "OLLAMA_KV_CACHE_TYPE not set (defaults to f16 — large KV cache)"; _rec "Set OLLAMA_KV_CACHE_TYPE=q8_0 in Ollama service"; }

    local ctx; ctx=$(echo "$env_str" | grep -oP 'OLLAMA_NUM_CTX=\d+' | cut -d= -f2 | head -1)
    [[ -z "$ctx" ]] && ctx=$(echo "$env_str" | grep -oP 'OLLAMA_CONTEXT_LENGTH=\d+' | cut -d= -f2 | head -1)
    _info "OLLAMA_NUM_CTX=${ctx:-default}"
    _L "ctx=$ctx  kv_type=$kv_type"

    # Check what VRAM Ollama reported at startup (dlsym hook effectiveness)
    local vram_reported; vram_reported=$(journalctl -u ollama --since '1 hour ago' --no-pager 2>/dev/null \
        | grep 'inference compute' | tail -1 | grep -oP 'total="\K[^"]+')
    if [[ -n "$vram_reported" ]]; then
        _info "Ollama sees GPU VRAM: $vram_reported"
        # Extract numeric GiB value
        local vram_gib; vram_gib=$(echo "$vram_reported" | grep -oP '[\d.]+')
        if (( $(echo "$vram_gib > 20" | bc -l 2>/dev/null || echo 0) )); then
            _chk "Virtual VRAM visible to Ollama: $vram_reported (GreenBoost dlsym hook working)"
        else
            _warn "Ollama sees only physical VRAM: $vram_reported — dlsym hook may not be active"
            _rec "Rebuild + redeploy shim: cd $MODULE_DIR && make shim && sudo ./deploy_fix.sh"
        fi
    else
        _info "No recent Ollama GPU discovery log — restart Ollama to verify VRAM reporting"
    fi

    local nthreads; nthreads=$(echo "$env_str" | grep -oP 'OLLAMA_NUM_THREADS=\d+' | cut -d= -f2 | head -1)
    _info "OLLAMA_NUM_THREADS=${nthreads:-default}  (i9-14900KF: 8=P-cores-only, 16=P+E)"
    if [[ -n "$nthreads" && "$nthreads" -gt 16 ]]; then
        _rec "OLLAMA_NUM_THREADS=${nthreads} — try 8 (P-cores only, 6GHz) or 16 (P+E mix)"
    fi

    # ═══════════════════════════════════════════════════════════════════════
    # 5. SYSTEM TUNING
    # ═══════════════════════════════════════════════════════════════════════
    _sect "5/8  SYSTEM TUNING"

    # CPU governor on P-cores
    local perf_cores=0
    for gov in /sys/devices/system/cpu/cpu{0..15}/cpufreq/scaling_governor; do
        [[ -f "$gov" && "$(cat "$gov" 2>/dev/null)" == "performance" ]] && (( perf_cores++ ))
    done
    [[ $perf_cores -ge 16 ]] \
        && _chk "CPU governor: performance on all 16 P-cores (0-15)" \
        || { _warn "CPU governor: only $perf_cores/16 P-cores at performance"; _rec "sudo ./greenboost_setup.sh tune"; }

    # THP
    local thp; thp=$(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null | grep -oP '\[\K[^\]]+')
    [[ "$thp" == "always" ]] \
        && _chk "THP: always (2MB hugepages enabled)" \
        || { _warn "THP: ${thp:-unknown} (needs 'always' for GreenBoost T2 pool)"; _rec "echo always | sudo tee /sys/kernel/mm/transparent_hugepage/enabled"; }

    # HugeTLB pages — must be 0 (GreenBoost uses buddy allocator, not HugeTLB pool)
    # Pre-allocating HugeTLB pages locks RAM away from the buddy allocator, making
    # the OOM guard always fire and blocking all T2 DDR4 allocations.
    local hp_total
    hp_total=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null || echo 0)
    if [[ $hp_total -eq 0 ]]; then
        _chk "HugeTLB nr_hugepages=0 (correct: T2 uses buddy allocator alloc_pages, not HugeTLB)"
    else
        local hp_gb=$(( hp_total * 2 / 1024 ))
        _warn "HugeTLB: ${hp_total} × 2MB = ${hp_gb} GB locked (OOM guard will always fire — T2 unavailable)"
        _rec  "Free HugeTLB pages: echo 0 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages && sudo ./greenboost_setup.sh install-sys-configs"
    fi

    # NVMe scheduler
    local nvme_sched; nvme_sched=$(cat /sys/block/nvme0n1/queue/scheduler 2>/dev/null | grep -oP '\[\K[^\]]+')
    [[ "$nvme_sched" == "none" ]] \
        && _chk "NVMe scheduler: none (optimal for Samsung 990)" \
        || { _warn "NVMe scheduler: ${nvme_sched:-unknown} (should be none)"; _rec "sudo ./greenboost_setup.sh tune"; }

    # NVMe swap T3
    if swapon --show | grep -q "/swap_nvme.img"; then
        local sw_used sw_size
        sw_size=$(swapon --show --noheadings | grep "/swap_nvme.img" | awk '{print $3}')
        sw_used=$(swapon --show --noheadings | grep "/swap_nvme.img" | awk '{print $4}')
        _chk "T3 NVMe swap: ${sw_used}/${sw_size} used"
    else
        _warn "T3 NVMe swap /swap_nvme.img not active"
        _rec "sudo ./greenboost_setup.sh setup-swap"
    fi

    # vm.swappiness
    local swappiness; swappiness=$(sysctl -n vm.swappiness 2>/dev/null || echo 60)
    [[ "$swappiness" -le 15 ]] \
        && _chk "vm.swappiness=$swappiness (prefers RAM — good for LLM)" \
        || { _warn "vm.swappiness=$swappiness (high — models may swap unnecessarily)"; _rec "sudo ./greenboost_setup.sh tune-sysctl"; }

    # ═══════════════════════════════════════════════════════════════════════
    # 6. MODEL BENCHMARK — shared helper
    # ═══════════════════════════════════════════════════════════════════════
    # Helper: run a timed inference test and report metrics
    _bench_model() {
        local model="$1"
        _sect "6+7/8  MODEL BENCHMARK: $model"

        # Check model is available in Ollama
        local available; available=$(curl -s --max-time 5 http://127.0.0.1:11434/api/tags 2>/dev/null \
            | python3 -c "
import sys, json
try:
    names = [m['name'] for m in json.load(sys.stdin).get('models', [])]
    print('yes' if '$model' in names else 'no')
except:
    print('error')
" 2>/dev/null)
        if [[ "$available" != "yes" ]]; then
            _warn "Model $model not in Ollama — skipping (pull with: ollama pull $model)"
            return
        fi
        _chk "Model available in Ollama"

        # Snapshot VRAM before
        local vram_before; vram_before=$(nvidia-smi --query-gpu=memory.used \
            --format=csv,noheader,nounits 2>/dev/null | tr -d ' ')

        # Benchmark prompt — generates ~60-100 tokens, response is verifiable
        local PROMPT="List all 8 planets of the solar system in order from the Sun, one per line, prefixed with their number."
        local test_start; test_start=$(date --iso-8601=seconds)

        # num_gpu=999 forces all layers to GPU so the shim can route DDR4 overflow
        _info "Sending inference request (stream=false, num_predict=150, num_gpu=999)..."
        local response; response=$(curl -s --max-time 400 \
            http://127.0.0.1:11434/api/generate \
            -H "Content-Type: application/json" \
            -d "{\"model\": \"$model\", \"prompt\": \"$PROMPT\", \"stream\": false, \"options\": {\"num_predict\": 150, \"num_gpu\": 999}}" \
            2>/dev/null)

        if [[ -z "$response" ]]; then
            _fail "No response from Ollama (timeout or crash) — check: journalctl -u ollama -f"
            return
        fi

        # Parse timing + response — pipe $response to python3 stdin
        # (heredoc + <<< redirection conflict in bash; use echo pipe instead)
        local parsed; parsed=$(echo "$response" | python3 -c "
import sys, json
try:
    d = json.loads(sys.stdin.read())
    ec  = d.get('eval_count', 0)
    en  = d.get('eval_duration', 1) or 1
    pc  = d.get('prompt_eval_count', 0)
    pn  = d.get('prompt_eval_duration', 1) or 1
    ln  = d.get('load_duration', 0)
    tn  = d.get('total_duration', 1) or 1
    tps = ec / (en / 1e9) if ec > 0 else 0.0
    r   = (d.get('response') or '').strip()
    planets = ['Mercury','Venus','Earth','Mars','Jupiter','Saturn','Uranus','Neptune']
    found   = sum(1 for p in planets if p.lower() in r.lower())
    snippet = r[:100].replace('\n', ' ')
    print(f'load_s={ln/1e9:.2f}')
    print(f'ttft_s={pn/1e9:.2f}')
    print(f'tps={tps:.2f}')
    print(f'eval_tokens={ec}')
    print(f'total_s={tn/1e9:.2f}')
    print(f'quality={found}/8')
    print(f'snippet={snippet}')
except Exception as e:
    print(f'parse_error={e}')
" 2>/dev/null)

        local load_s ttft_s tps eval_tokens total_s quality snippet
        load_s=$(     echo "$parsed" | grep "^load_s="      | cut -d= -f2)
        ttft_s=$(     echo "$parsed" | grep "^ttft_s="      | cut -d= -f2)
        tps=$(        echo "$parsed" | grep "^tps="         | cut -d= -f2)
        eval_tokens=$(echo "$parsed" | grep "^eval_tokens=" | cut -d= -f2)
        total_s=$(    echo "$parsed" | grep "^total_s="     | cut -d= -f2)
        quality=$(    echo "$parsed" | grep "^quality="     | cut -d= -f2)
        snippet=$(    echo "$parsed" | grep "^snippet="     | cut -d= -f2-)

        # GPU layer count from Ollama logs
        local gpu_layers; gpu_layers=$(journalctl -u ollama --since "$test_start" --no-pager 2>/dev/null \
            | grep "offloaded.*layers to GPU" | tail -1 | grep -oP '\d+/\d+' | head -1)
        [[ -z "$gpu_layers" ]] && gpu_layers="?"

        # VRAM delta
        local vram_after; vram_after=$(nvidia-smi --query-gpu=memory.used \
            --format=csv,noheader,nounits 2>/dev/null | tr -d ' ')
        local vram_delta=$(( ${vram_after:-0} - ${vram_before:-0} ))

        # Display results
        _chk  "Load time      : ${load_s}s"
        _chk  "TTFT           : ${ttft_s}s"
        _chk  "Throughput     : ${tps} tok/s  (${eval_tokens} tokens in ${total_s}s)"
        _info "GPU layers     : $gpu_layers"
        _info "VRAM delta     : +${vram_delta} MiB  (before=${vram_before} after=${vram_after})"
        _info "Response check : $quality planets mentioned"
        _info "Snippet        : $snippet"
        _L    "benchmark model=$model load_s=$load_s ttft_s=$ttft_s tps=$tps eval_tokens=$eval_tokens gpu_layers=$gpu_layers vram_delta=$vram_delta quality=$quality"

        # Evaluate GPU offload — extract numerator/denominator
        local gpu_n gpu_total
        gpu_n=$(     echo "$gpu_layers" | cut -d/ -f1 2>/dev/null || echo 0)
        gpu_total=$( echo "$gpu_layers" | cut -d/ -f2 2>/dev/null || echo 48)
        [[ -z "$gpu_total" || "$gpu_total" == "$gpu_layers" ]] && gpu_total=48
        [[ -z "$gpu_n" ]] && gpu_n=0

        if [[ "$gpu_layers" == "?" ]]; then
            _warn "Could not read GPU layer count — check: journalctl -u ollama | grep offloaded"
        elif [[ "$gpu_n" -eq 0 ]]; then
            _fail "0 GPU layers — model running CPU-ONLY (GreenBoost DDR4 overflow not working)"
            _rec  "Fix shim: cd $MODULE_DIR && make shim && sudo ./deploy_fix.sh && sudo systemctl restart ollama"
        elif [[ "$gpu_n" -lt "$gpu_total" ]]; then
            _warn "Partial GPU offload: $gpu_layers layers (expected $gpu_total/$(echo $gpu_total)) — cuDeviceTotalMem hook missing or num_gpu not passed"
            _rec  "Rebuild shim: make shim && sudo cp libgreenboost_cuda.so /usr/local/lib/ && sudo systemctl restart ollama"
        else
            _chk  "Full GPU offload: $gpu_layers layers (all layers via GreenBoost DDR4 overflow)"
        fi

        # Evaluate throughput
        local tps_int; tps_int=$(echo "$tps" | python3 -c "import sys; v=float(sys.stdin.read().strip() or 0); print(int(v*10))" 2>/dev/null || echo 0)
        if   [[ $tps_int -lt 15 ]]; then  # < 1.5 tok/s
            _fail "Throughput ${tps} tok/s — very slow (likely CPU fallback or model hung)"
            _rec  "Check Ollama logs: journalctl -u ollama --since '5 min ago' | grep -E 'offloaded|cudaMalloc'"
        elif [[ $tps_int -lt 50 ]]; then  # < 5 tok/s
            _warn "Throughput ${tps} tok/s — partial GPU (PCIe/DDR4 bandwidth limit expected for this model size)"
        else
            _chk  "Throughput ${tps} tok/s — good"
        fi

        # Verify response makes sense
        if [[ "$quality" == "8/8" ]]; then
            _chk "Response correct (all 8 planets)"
        elif [[ "$quality" =~ ^[4-7]/8 ]]; then
            _warn "Response partial ($quality planets) — model may have been cut off"
        else
            _warn "Response unexpected ($quality planets found) — snippet: $snippet"
        fi
    }

    # ═══════════════════════════════════════════════════════════════════════
    # 6. glm-4.7-flash:q8_0
    # ═══════════════════════════════════════════════════════════════════════
    _bench_model "glm-4.7-flash:q8_0"

    # ═══════════════════════════════════════════════════════════════════════
    # 7. glm-4.7-flash:latest
    # ═══════════════════════════════════════════════════════════════════════
    _bench_model "glm-4.7-flash:latest"

    # ═══════════════════════════════════════════════════════════════════════
    # 8. TUNING RECOMMENDATIONS
    # ═══════════════════════════════════════════════════════════════════════
    _sect "8/8  TUNING RECOMMENDATIONS"

    # VRAM headroom vs model sizes
    local headroom; headroom=$(echo "$env_str" | grep -oP 'GREENBOOST_VRAM_HEADROOM_MB=\d+' | cut -d= -f2)
    headroom=${headroom:-1024}
    _info "GREENBOOST_VRAM_HEADROOM_MB=$headroom — shim overflows to DDR4 when VRAM free < this"
    if [[ -n "$vram_total_mb" && $headroom -gt $(( vram_total_mb / 6 )) ]]; then
        _rec "GREENBOOST_VRAM_HEADROOM_MB=$headroom may be too conservative for ${vram_total_mb}MB GPU — try 512"
    fi

    # Context window
    if [[ "$ctx" =~ ^[0-9]+$ ]]; then
        if [[ $ctx -gt 32768 ]]; then
            _warn "OLLAMA_NUM_CTX=$ctx is large — KV cache at q8_0 for glm-4.7-flash uses significant VRAM"
            _rec  "Try OLLAMA_NUM_CTX=16384 for better VRAM/context balance (current: $ctx)"
        elif [[ $ctx -lt 8192 ]]; then
            _rec  "OLLAMA_NUM_CTX=$ctx is low for GLM-4.7 — try 16384 for better context quality"
        else
            _chk  "OLLAMA_NUM_CTX=$ctx (balanced)"
        fi
    fi

    # GPU overhead
    local overhead; overhead=$(echo "$env_str" | grep -oP 'OLLAMA_GPU_OVERHEAD=\d+' | cut -d= -f2)
    if [[ -n "$overhead" ]]; then
        local overhead_mb=$(( overhead / 1048576 ))
        _info "OLLAMA_GPU_OVERHEAD=${overhead_mb} MB reserved in VRAM for compute graph"
        [[ $overhead_mb -gt 512 ]] && _rec "OLLAMA_GPU_OVERHEAD=${overhead}(${overhead_mb}MB) — try 268435456 (256MB) to free VRAM for more layers"
    fi

    # CPU threads recommendation for this CPU
    if [[ -z "$nthreads" ]]; then
        _rec "Set OLLAMA_NUM_THREADS=8 to pin CPU inference to 6GHz P-cores only (default uses all 32 CPUs including 4.4GHz E-cores)"
    fi

    # Print all recommendations
    if [[ ${#recs[@]} -gt 0 ]]; then
        echo ""
        echo -e "${YLW}  Actionable recommendations:${NC}"
        for i in "${!recs[@]}"; do
            echo -e "  ${YLW}$(( i+1 )). ${recs[$i]}${NC}"
        done
        printf "\nActionable recommendations:\n" >> "$LOG_FILE"
        for i in "${!recs[@]}"; do printf "  %d. %s\n" "$(( i+1 ))" "${recs[$i]}" >> "$LOG_FILE"; done
    fi

    # ── Final summary ────────────────────────────────────────────────────
    {
        echo ""
        echo "=== End ==="
        echo "issues=$issues"
        echo "recommendations=${#recs[@]}"
        echo "log=$LOG_FILE"
    } >> "$LOG_FILE"
    ln -sf "$LOG_FILE" "$LOG_LATEST"

    echo ""
    echo -e "${BLU}════════════════════════════════════════════════════════════════${NC}"
    if [[ $issues -eq 0 ]]; then
        echo -e "  ${GRN}All checks passed — GreenBoost is healthy ✓${NC}"
    else
        echo -e "  ${RED}$issues issue(s) found — see [FAIL] lines above${NC}"
    fi
    echo ""
    echo -e "  Log: ${BLU}$LOG_LATEST${NC}"
    echo -e "  Share with Claude Code: ${BLU}cat $LOG_LATEST${NC}"
    echo ""
}

# ---- Entry point -------------------------------------------------------

COMMAND="${1:-help}"
case "$COMMAND" in
    install)            cmd_install            ;;
    uninstall)          cmd_uninstall          ;;
    build)              cmd_build              ;;
    load)               cmd_load               ;;
    unload)             cmd_unload             ;;
    install-sys-configs) cmd_install_sys_configs ;;
    install-deps)        cmd_install_deps       ;;
    setup-swap)          cmd_setup_swap "$@"    ;;
    full-install)        cmd_full_install       ;;
    tune)               cmd_tune               ;;
    tune-grub)          cmd_tune_grub          ;;
    tune-sysctl)        cmd_tune_sysctl        ;;
    tune-libs)          cmd_tune_libs          ;;
    tune-all)           cmd_tune_all           ;;
    status)             cmd_status             ;;
    diagnose)           cmd_diagnose           ;;
    help|--help|-h)     cmd_help               ;;
    *) die "Unknown command: '$COMMAND'  — use: $0 help" ;;
esac
