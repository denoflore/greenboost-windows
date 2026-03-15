#!/usr/bin/env bash
# GreenBoost v2.3 — Setup & installation script
# Author: Ferran Duarri
# Hardware: ASRock B760M-ITX/D4 | i9-14900KF | RTX 5070 OC | 64 GB DDR4-3600 | Samsung 990 EVO Plus 4 TB
#
# 3-tier memory hierarchy:
#   Tier 1 — RTX 5070 VRAM      12 GB   ~336 GB/s GDDR7  (hot layers)
#   Tier 2 — DDR4 pool          51 GB   ~57.6 GB/s dual-ch (cold layers)
#   Tier 3 — NVMe swap          64 GB   ~7.25 GB/s seq     (frozen pages, auto-sized)
#   Combined capacity           75 GB   (T3 expandable up to 200+ GB)
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
#   NVME_SWAP_GB=64    total NVMe swap capacity  (auto-detected; 64 GB default)
#   NVME_POOL_GB=58    GreenBoost soft cap on T3 allocations

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

# ---- Hardware detection -----------------------------------------------
# Populates GB_PHYS, GB_VIRT, GB_RESERVE, GB_NVME_SWAP, GB_NVME_POOL,
# GB_PCORES_MAX, GB_GOLDEN_MIN, GB_GOLDEN_MAX, GB_PCORES_ONLY,
# RAM_TYPE, RAM_SPEED_MT, GPU_NAME, CPU_NAME, NVME_SIZE_GB.
# Safe to call multiple times — idempotent.

detect_hardware() {
    # ── GPU ──────────────────────────────────────────────────────────────
    if command -v nvidia-smi &>/dev/null; then
        local gpu_mem_mib
        gpu_mem_mib=$(nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits \
            2>/dev/null | head -1 | tr -d ' ')
        GPU_NAME=$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1 \
            | sed 's/^[[:space:]]*//')
        GB_PHYS=$(( ${gpu_mem_mib:-12288} / 1024 ))
        [[ $GB_PHYS -lt 1 ]] && GB_PHYS=1
    else
        GPU_NAME="Unknown (nvidia-smi not found)"
        GB_PHYS=8
        warn "nvidia-smi not found — assuming 8 GB VRAM. Install NVIDIA driver."
    fi

    # ── System RAM ───────────────────────────────────────────────────────
    local total_ram_kb
    total_ram_kb=$(grep MemTotal /proc/meminfo | awk '{print $2}')
    local total_ram_gb=$(( total_ram_kb / 1024 / 1024 ))

    RAM_TYPE=$(dmidecode -t memory 2>/dev/null \
        | awk '/^\s*Type:/{t=$2} /^\s*Configured Memory Speed:/{s=$4; print t,s; exit}' \
        | awk '{print $1}')
    [[ -z "$RAM_TYPE" || "$RAM_TYPE" == "Unknown" ]] && RAM_TYPE="DDR"
    RAM_SPEED_MT=$(dmidecode -t memory 2>/dev/null \
        | awk '/^\s*Configured Memory Speed:/{print $4; exit}')
    [[ -z "$RAM_SPEED_MT" ]] && RAM_SPEED_MT="?"

    # safety reserve: 15% of RAM, min 8 GB, max 16 GB
    GB_RESERVE=$(( total_ram_gb * 15 / 100 ))
    [[ $GB_RESERVE -lt 8  ]] && GB_RESERVE=8
    [[ $GB_RESERVE -gt 16 ]] && GB_RESERVE=16

    # virtual VRAM pool: 80% of RAM minus safety reserve
    GB_VIRT=$(( total_ram_gb * 80 / 100 - GB_RESERVE ))
    [[ $GB_VIRT -lt 4 ]] && GB_VIRT=4

    # ── CPU topology ─────────────────────────────────────────────────────
    CPU_NAME=$(grep "model name" /proc/cpuinfo | head -1 | cut -d: -f2 | sed 's/^[[:space:]]*//')
    local total_cpus; total_cpus=$(nproc)

    # Detect Intel hybrid P/E core split via sysfs core_type
    local has_ecores=0
    if ls /sys/devices/system/cpu/cpu*/topology/core_type &>/dev/null; then
        grep -l "Efficient core" /sys/devices/system/cpu/cpu*/topology/core_type \
            &>/dev/null && has_ecores=1
    fi

    if [[ $has_ecores -eq 1 ]]; then
        # Find highest P-core CPU number
        GB_PCORES_MAX=0
        local f ct
        for f in /sys/devices/system/cpu/cpu*/topology/core_type; do
            ct=$(cat "$f" 2>/dev/null)
            if [[ "$ct" == *"Performance core"* ]]; then
                local cpunum; cpunum=$(echo "$f" | grep -oP 'cpu\K[0-9]+')
                [[ $cpunum -gt $GB_PCORES_MAX ]] && GB_PCORES_MAX=$cpunum
            fi
        done
        GB_PCORES_ONLY=1
        # Golden cores: assume last 4 P-core siblings by frequency
        # For i9-14900KF: CPUs 4-7 are the 6 GHz golden cores
        local pcores_count=$(( GB_PCORES_MAX + 1 ))
        GB_GOLDEN_MIN=$(( pcores_count / 4 ))
        GB_GOLDEN_MAX=$(( pcores_count / 4 + 3 ))
    else
        # Non-hybrid (AMD or older Intel): all CPUs are uniform
        GB_PCORES_MAX=$(( total_cpus - 1 ))
        GB_PCORES_ONLY=0
        GB_GOLDEN_MIN=0
        GB_GOLDEN_MAX=$(( total_cpus > 4 ? 3 : total_cpus - 1 ))
    fi

    # ── NVMe ─────────────────────────────────────────────────────────────
    NVME_SIZE_GB=0
    local name size
    while read -r name size; do
        local sz_gb=$(( size / 1024 / 1024 / 1024 ))
        [[ $sz_gb -gt $NVME_SIZE_GB ]] && NVME_SIZE_GB=$sz_gb
    done < <(lsblk -b -d -o NAME,SIZE 2>/dev/null | awk '$1~/^nvme/{print $1,$2}')
    [[ $NVME_SIZE_GB -eq 0 ]] && NVME_SIZE_GB=128  # fallback

    # ── NVMe swap sizing (T3) ─────────────────────────────────────────────
    # Goal: enough swap as overflow safety net.
    # For glm-4.7-flash:q8_0 (32GB model): T1+T2 covers it; 64GB is generous.
    # General formula: 4×VRAM but at least 32 GB and at most 200 GB.
    GB_NVME_SWAP=$(( GB_PHYS * 4 ))
    [[ $GB_NVME_SWAP -lt 32  ]] && GB_NVME_SWAP=32
    [[ $GB_NVME_SWAP -gt 200 ]] && GB_NVME_SWAP=200
    # Never allocate more than half the NVMe drive
    local half_nvme=$(( NVME_SIZE_GB / 2 ))
    [[ $GB_NVME_SWAP -gt $half_nvme && $half_nvme -gt 32 ]] && GB_NVME_SWAP=$half_nvme

    GB_NVME_POOL=$(( GB_NVME_SWAP * 9 / 10 ))

    # ── Ollama CTX based on available pool ───────────────────────────────
    # Heuristic: large context needs ~12GB KV cache; use 131K if pool >= 40 GB
    local total_pool=$(( GB_PHYS + GB_VIRT ))
    if   [[ $total_pool -ge 40 ]]; then GB_OLLAMA_CTX=131072
    elif [[ $total_pool -ge 24 ]]; then GB_OLLAMA_CTX=65536
    elif [[ $total_pool -ge 16 ]]; then GB_OLLAMA_CTX=32768
    else                                 GB_OLLAMA_CTX=16384
    fi
}

print_detected_hardware() {
    info "Detected hardware:"
    info "  GPU   : ${GPU_NAME}  (${GB_PHYS} GB VRAM)"
    info "  RAM   : ${RAM_TYPE}-${RAM_SPEED_MT} MT/s  ->  pool ${GB_VIRT} GB  (reserve ${GB_RESERVE} GB)"
    info "  CPU   : ${CPU_NAME}"
    info "  NVMe  : ${NVME_SIZE_GB} GB  ->  swap ${GB_NVME_SWAP} GB"
    info "  Pool  : T1=${GB_PHYS}GB + T2=${GB_VIRT}GB + T3=${GB_NVME_SWAP}GB = $(( GB_PHYS + GB_VIRT + GB_NVME_SWAP )) GB"
    info "  CTX   : OLLAMA_NUM_CTX=${GB_OLLAMA_CTX}"
}

# Owner-workstation preset — hard-coded optimal for known hardware:
# ASRock B760M-ITX/D4 | i9-14900KF | RTX 5070 OC 12GB | 64GB DDR4-3600 dual-ch | Samsung 990 EVO Plus 4TB
set_owner_workstation_params() {
    GPU_NAME="ASUS RTX 5070 OC (GB205)"
    CPU_NAME="Intel Core i9-14900KF (8Px2HT + 16E = 32 logical, golden CPU 4-7 @ 6GHz)"
    RAM_TYPE="DDR4"
    RAM_SPEED_MT="3600"
    NVME_SIZE_GB=4000
    GB_PHYS=12
    GB_VIRT=51
    GB_RESERVE=12
    GB_NVME_SWAP=64
    GB_NVME_POOL=58
    GB_PCORES_MAX=15
    GB_GOLDEN_MIN=4
    GB_GOLDEN_MAX=7
    GB_PCORES_ONLY=1
    GB_OLLAMA_CTX=131072
    info "Owner-workstation preset applied (i9-14900KF | RTX 5070 12GB | 64GB DDR4-3600 | 4TB NVMe)"
}

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
    detect_hardware

    info "Installing GreenBoost v2.3 system configuration files..."

    # 1. Ollama service — inject GreenBoost env vars + LD_PRELOAD
    local svc="/etc/systemd/system/ollama.service"
    if [[ -f "$svc" ]]; then
        # Add environment lines if not already present
        if ! grep -q "GREENBOOST_VRAM_HEADROOM_MB" "$svc"; then
            sed -i "/^\[Service\]/a Environment=\"OLLAMA_FLASH_ATTENTION=1\"\nEnvironment=\"OLLAMA_KV_CACHE_TYPE=q8_0\"\nEnvironment=\"OLLAMA_NUM_CTX=${GB_OLLAMA_CTX}\"\nEnvironment=\"OLLAMA_MAX_LOADED_MODELS=1\"\nEnvironment=\"OLLAMA_KEEP_ALIVE=-1\"\nEnvironment=\"GREENBOOST_VRAM_HEADROOM_MB=2048\"\nEnvironment=\"GREENBOOST_DEBUG=0\"\nEnvironment=\"LD_PRELOAD=/usr/local/lib/libgreenboost_cuda.so\"" "$svc"
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
    cat > /etc/systemd/system/cpu-perf.service << CPUEOF
[Unit]
Description=GreenBoost CPU performance governor (P-cores 0-${GB_PCORES_MAX})
After=multi-user.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/bash -c 'for cpu in \$(seq 0 ${GB_PCORES_MAX}); do echo performance > /sys/devices/system/cpu/cpu\${cpu}/cpufreq/scaling_governor; done; cset set -c ${GB_GOLDEN_MIN}-${GB_GOLDEN_MAX} -s inference_set || true; cset proc -m -f root -t inference_set -k || true'
ExecStop=/bin/bash  -c 'for cpu in \$(seq 0 ${GB_PCORES_MAX}); do echo powersave  > /sys/devices/system/cpu/cpu\${cpu}/cpufreq/scaling_governor; done; cset set -d inference_set || true'

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
    detect_hardware
    check_deps
    cmd_build

    info "Installing kernel module..."
    make -C "$MODULE_DIR" install || die "Module install failed"

    info "Installing CUDA shim to $SHIM_DEST/..."
    cp "$MODULE_DIR/$SHIM_LIB" "$SHIM_DEST/"
    ldconfig

    # modprobe defaults
    info "Writing /etc/modprobe.d/greenboost.conf ..."
    cat > /etc/modprobe.d/greenboost.conf << MODEOF
# GreenBoost — 3-tier pool (auto-configured for detected hardware)
# GPU   : ${GPU_NAME}  (${GB_PHYS} GB VRAM)
# RAM   : ${RAM_TYPE}-${RAM_SPEED_MT}  (pool ${GB_VIRT} GB, reserve ${GB_RESERVE} GB)
# NVMe  : swap ${GB_NVME_SWAP} GB
options greenboost physical_vram_gb=${GB_PHYS} virtual_vram_gb=${GB_VIRT} safety_reserve_gb=${GB_RESERVE} nvme_swap_gb=${GB_NVME_SWAP} nvme_pool_gb=${GB_NVME_POOL} pcores_max_cpu=${GB_PCORES_MAX} golden_cpu_min=${GB_GOLDEN_MIN} golden_cpu_max=${GB_GOLDEN_MAX} pcores_only=${GB_PCORES_ONLY}
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
    detect_hardware
    local phys="${GPU_PHYS_GB:-${GB_PHYS}}"
    local virt="${VIRT_VRAM_GB:-${GB_VIRT}}"
    local res="${RESERVE_GB:-${GB_RESERVE}}"
    local nvme_sw="${NVME_SWAP_GB:-${GB_NVME_SWAP}}"
    local nvme_pool="${NVME_POOL_GB:-${GB_NVME_POOL}}"
    local pcores_max="${GB_PCORES_MAX}"
    local golden_min="${GB_GOLDEN_MIN}"
    local golden_max="${GB_GOLDEN_MAX}"
    local pcores_only="${GB_PCORES_ONLY}"

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
        pcores_max_cpu="$pcores_max" \
        golden_cpu_min="$golden_min" \
        golden_cpu_max="$golden_max" \
        pcores_only="$pcores_only" \
        || die "insmod failed — check: dmesg | tail -20"

    info "GreenBoost v2.3 loaded — 3-tier pool active!"
    info ""
    info "  T1 ${GPU_NAME} : ${phys} GB  [hot layers]"
    info "  T2 ${RAM_TYPE} pool         : ${virt} GB  [cold layers]"
    info "  T3 NVMe swap               : ${nvme_sw} GB  [frozen pages]"
    info "  ─────────────────────────────────────────"
    info "  Combined view              : $(( phys + virt + nvme_sw )) GB total model capacity"
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
    detect_hardware

    info "Tuning workstation for GreenBoost / LLM workloads..."
    info "Hardware: ${CPU_NAME} | ${GPU_NAME} | ${RAM_TYPE}-${RAM_SPEED_MT} | ${NVME_SIZE_GB} GB NVMe"
    echo ""

    # ── CPU governor → performance (P-cores run at max boost, not idle) ──
    local changed=0
    for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        [[ -w "$gov" ]] && echo performance > "$gov" && changed=1
    done
    [[ $changed -eq 1 ]] && info "CPU governor      : performance ($(nproc) CPUs)" \
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
    detect_hardware

    local grub_file="/etc/default/grub"
    local kver; kver="$(uname -r)"
    local kcfg="/boot/config-${kver}"

    [[ -f "$grub_file" ]] || die "GRUB config not found: $grub_file"

    info "Validating GRUB flags for: ${CPU_NAME} | ${GPU_NAME}"
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

    # ── Flag: rcu_nocbs=<ecores> ─────────────────────────────────────────
    # Offloads RCU (Read-Copy-Update) callback processing to E-cores or
    # non-golden CPUs, freeing the high-frequency P-cores for inference.
    # Range is derived from detected CPU topology (GB_PCORES_MAX).
    if [[ $GB_PCORES_ONLY -eq 1 ]]; then
        local ecores_start=$(( GB_PCORES_MAX + 1 ))
        local ecores_end=$(( $(nproc) - 1 ))
        if [[ $ecores_start -le $ecores_end ]]; then
            _grub_check_flag "rcu_nocbs=${ecores_start}-${ecores_end}" \
                "offload RCU callbacks to E-cores (CPU ${ecores_start}-${ecores_end}), freeing P-cores for inference" \
                "CONFIG_RCU_NOCB_CPU" \
                && new_flags="$new_flags rcu_nocbs=${ecores_start}-${ecores_end}"
        fi
    fi

    # ── Flag: nohz_full=<golden> ─────────────────────────────────────────
    # Makes the highest-frequency cores tick-less when they have exactly
    # one runnable thread. Eliminates timer interrupts during dense matrix
    # multiplications — reduces LLM token latency.
    # Range derived from detected golden-core topology (GB_GOLDEN_MIN/MAX).
    if [[ $GB_PCORES_ONLY -eq 1 && $GB_GOLDEN_MIN -lt $GB_GOLDEN_MAX ]]; then
        _grub_check_flag "nohz_full=${GB_GOLDEN_MIN}-${GB_GOLDEN_MAX}" \
            "tick-less golden P-cores (CPU ${GB_GOLDEN_MIN}-${GB_GOLDEN_MAX}) during single-thread inference" \
            "CONFIG_NO_HZ_FULL" \
            && new_flags="$new_flags nohz_full=${GB_GOLDEN_MIN}-${GB_GOLDEN_MAX}"
    fi

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
    detect_hardware

    local dest="/etc/sysctl.d/99-zzz-greenboost.conf"

    info "Writing definitive sysctl config: $dest"
    info "Hardware: ${CPU_NAME} | ${GPU_NAME} | ${total_ram_gb:-$(( $(grep MemTotal /proc/meminfo | awk '{print $2}') / 1024 / 1024 ))} GB RAM | ${NVME_SIZE_GB} GB NVMe"
    info "This file loads last (99-zzz) and wins over all conflicting files."
    echo ""

    # Show conflicts found
    info "Conflicts resolved:"
    info "  vm.swappiness       : multiple files set 10/20 → final: 10"
    info "  vm.dirty_ratio      : 15 vs 40 → final: 40"
    info "  vm.dirty_background_ratio: 5 vs 10 → final: 10"
    info "  kernel.sched_autogroup_enabled: 1 → 0 (bad for compute, groups by session)"
    info "New settings added:"
    info "  kernel.sched_migration_cost_ns: 5000000 (5ms — keep threads on P-cores)"
    info "  kernel.sched_min_granularity_ns: 10000000 (10ms — better for large tasks)"
    info "  kernel.sched_wakeup_granularity_ns: 15000000 (reduces spurious wakeups)"
    echo ""

    # Write the header line with detected hardware (variables don't expand in 'HEREDOC')
    printf '# GreenBoost v2.3 — Definitive sysctl config\n' > "$dest"
    printf '# Hardware: %s | %s | %s-%s | %s GB NVMe\n' \
        "${CPU_NAME}" "${GPU_NAME}" "${RAM_TYPE}" "${RAM_SPEED_MT}" "${NVME_SIZE_GB}" >> "$dest"
    printf '# Loaded last (99-zzz) — wins all conflicts with earlier sysctl.d files.\n' >> "$dest"
    printf '# Do NOT edit other sysctl.d files; make changes here instead.\n' >> "$dest"

    cat >> "$dest" << 'SYSCTL_EOF'

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
    detect_hardware

    info "Installing missing AI/compute libraries for: ${CPU_NAME} | ${GPU_NAME}"
    echo ""

    # ── APT packages ──────────────────────────────────────────────────────
    local pkgs=(
        # BLAS/LAPACK — OpenBLAS compiled with AVX2/FMA or equivalent
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

        # cpufrequtils — userspace CPU frequency tools
        cpufrequtils

        # linux-tools — perf, turbostat (monitors core frequencies + C-states)
        linux-tools-generic
    )

    # CPU vendor-specific microcode
    local cpu_vendor
    cpu_vendor=$(grep -m1 "vendor_id" /proc/cpuinfo | awk '{print $3}')
    if [[ "$cpu_vendor" == "GenuineIntel" ]]; then
        pkgs+=(intel-microcode)
        info "CPU vendor: Intel — adding intel-microcode"
    elif [[ "$cpu_vendor" == "AuthenticAMD" ]]; then
        pkgs+=(amd64-microcode)
        info "CPU vendor: AMD — adding amd64-microcode"
    fi

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
    detect_hardware 2>/dev/null
    echo ""
    echo -e "${BLU}GreenBoost v2.3 — 3-Tier GPU Memory Pool${NC}"
    echo "Author : Ferran Duarri"
    echo "CPU    : ${CPU_NAME}"
    echo "GPU    : ${GPU_NAME}  (${GB_PHYS} GB VRAM)"
    echo "RAM    : ${RAM_TYPE}-${RAM_SPEED_MT}  (pool ${GB_VIRT} GB)"
    echo "NVMe   : ${NVME_SIZE_GB} GB  (swap ${GB_NVME_SWAP} GB)"
    echo ""
    echo "  Tier 1  ${GPU_NAME}  ${GB_PHYS} GB  [hot layers]"
    echo "  Tier 2  ${RAM_TYPE} pool           ${GB_VIRT} GB  [cold layers, hugepages]"
    echo "  Tier 3  NVMe swap               ${GB_NVME_SWAP} GB  [frozen pages, swappable 4K]"
    echo "          ─────────────────────────────────────"
    echo "          Combined capacity       $(( GB_PHYS + GB_VIRT + GB_NVME_SWAP )) GB"
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
    echo "  setup-swap [GB]      Create/activate NVMe swap (default: auto-sized, ~64 GB for target model)"
    echo "  full-install [--owner-workstation]  Complete install — hardware auto-detected or owner preset"
    echo "  status      Show module status and 3-tier pool info"
    echo "  diagnose    Full health check — run this after reboot to verify everything works"
    echo "  optimize-model [--model M] [--strategy tensorrt|lora|exllama|all]"
    echo "               Optimize LLM for max speed: TRT-LLM, LoRA, ExLlamaV3"
    echo "  help        Show this help"
    echo ""
    echo "ENVIRONMENT (for load, detected defaults shown):"
    echo "  GPU_PHYS_GB=${GB_PHYS}     Physical VRAM in GB          (detected: ${GPU_NAME})"
    echo "  VIRT_VRAM_GB=${GB_VIRT}    DDR4 pool size in GB         (detected: ${GB_VIRT} GB)"
    echo "  RESERVE_GB=${GB_RESERVE}      System RAM to keep free      (detected: ${GB_RESERVE} GB)"
    echo "  NVME_SWAP_GB=${GB_NVME_SWAP}    NVMe swap capacity in GB     (detected: ${GB_NVME_SWAP} GB)"
    echo "  NVME_POOL_GB=${GB_NVME_POOL}    GreenBoost T3 soft cap in GB (detected: ${GB_NVME_POOL} GB)"
    echo ""
    echo "  Example: sudo VIRT_VRAM_GB=48 NVME_SWAP_GB=64 ./greenboost_setup.sh load"
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
    detect_hardware 2>/dev/null  # populate GB_NVME_SWAP if not already set
    local _default_swap="${GB_NVME_SWAP:-64}"
    local gb="${2:-${_default_swap}}"
    [[ "$1" == "setup-swap" ]] && gb="${2:-${_default_swap}}" || gb="${1:-${_default_swap}}"
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

    # ── Hardware preset: --owner-workstation overrides auto-detection ────
    local _owner_ws=0
    for arg in "$@"; do
        [[ "$arg" == "--owner-workstation" ]] && _owner_ws=1
    done

    if [[ $_owner_ws -eq 1 ]]; then
        set_owner_workstation_params
        info "╔══════════════════════════════════════════════════════════════╗"
        info "║  GreenBoost — Owner-Workstation Install                     ║"
        info "║  i9-14900KF | RTX 5070 OC | 64 GB DDR4-3600 | 4 TB NVMe   ║"
        info "╚══════════════════════════════════════════════════════════════╝"
    else
        detect_hardware
        info "╔══════════════════════════════════════════════════════════════╗"
        info "║  GreenBoost — Dynamic Install (hardware auto-detected)      ║"
        info "╚══════════════════════════════════════════════════════════════╝"
        print_detected_hardware
    fi
    echo ""

    # 1/7 — OS dependencies
    info "[1/7] Installing Ubuntu OS dependencies..."
    cmd_install_deps
    echo ""

    # 2/7 — NVMe swap (T3 tier, auto-sized)
    info "[2/7] Setting up NVMe swap file (T3 tier, ${GB_NVME_SWAP} GB)..."
    cmd_setup_swap "${GB_NVME_SWAP}"
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

    # 7/7 — ExLlamaV3 + Python tools (kvpress, modelopt, unsloth)
    info "[7/7] Setting up Python venv + ExLlamaV3 + inference tools..."
    local exllama_dir="$MODULE_DIR/libraries/exllamav3"
    local venv_dir="/opt/greenboost/venv"

    # Ensure dependencies (cargo/rustfmt needed for pydantic-core on Python 3.14)
    apt-get install -y liburing-dev python3-venv python3-pip cargo rustfmt -q 2>/dev/null || true

    # Create venv if absent
    if [[ ! -d "$venv_dir" ]]; then
        info "Creating Python venv at $venv_dir ..."
        python3 -m venv "$venv_dir" || die "python3 -m venv failed — install python3-venv"
    fi

    # Bootstrap pip (Ubuntu 26.04 venvs ship without pip by default)
    if ! "$venv_dir/bin/python" -m pip --version &>/dev/null 2>&1; then
        info "Bootstrapping pip in venv ..."
        "$venv_dir/bin/python" -m ensurepip --upgrade \
            || die "ensurepip failed — install python3-pip: apt-get install -y python3-pip"
    fi
    # Upgrade pip + install build tools.
    # maturin is required to compile pydantic-core (Rust) on Python 3.14 where
    # no pre-built wheels exist. setuptools/wheel cover C-extension packages.
    "$venv_dir/bin/python" -m pip install --upgrade pip setuptools wheel maturin -q

    chmod -R 755 /opt/greenboost

    # Install ExLlamaV3 from bundled library.
    # Strategy: skip ALL source compilation at install time.
    #   - ExLlamaV3's own CUDA kernels: fail on Python 3.14 + Blackwell toolchain;
    #     ExLlamaV3 v3 compiles them lazily via torch.utils.cpp_extension.load()
    #     at first import — no install-time build needed.
    #   - flash_attn: source-only on Python 3.14, takes 30+ min; ExLlamaV3 has
    #     a fallback attention path and works without it.
    #   - pydantic-core: Rust/PyO3 0.24 doesn't support Python 3.14; install
    #     a newer pydantic (pre-built wheel) instead of the pinned 2.11.0.
    #
    # Implementation: add exllamav3 dir to site-packages via .pth file,
    # then pip-install only deps that have binary wheels for Python 3.14.
    if [[ -d "$exllama_dir" ]]; then
        info "Installing ExLlamaV3 from $exllama_dir ..."

        # Resolve site-packages path
        local site_packages
        site_packages="$("$venv_dir/bin/python" -c \
            'import sysconfig; print(sysconfig.get_path("purelib"))')"

        # Remove any partial pip state left by previous failed "pip install -e"
        # attempts. Those runs register dist-info metadata even when the CUDA/Rust
        # build fails, making pip report spurious dependency conflict warnings.
        "$venv_dir/bin/python" -m pip uninstall exllamav3 -y -q 2>/dev/null || true
        # Also remove any stale editable .pth or egg-link artefacts
        rm -f "$site_packages"/__editable__.exllamav3*.pth \
              "$site_packages"/exllamav3.egg-link 2>/dev/null || true

        # Add ExLlamaV3 source directory to Python path (no CUDA build at install)
        echo "$exllama_dir" > "$site_packages/exllamav3.pth"
        info "ExLlamaV3 Python path registered: $site_packages/exllamav3.pth"

        # Install ExLlamaV3's pure-Python / binary-wheel dependencies.
        # Omit flash_attn (source-only, 30+ min) and pydantic 2.11.0 (Rust build).
        # Use pydantic>=2.11.0 without upper pin so pip finds a Python 3.14 wheel.
        # exllamav3 is managed via .pth (not pip), so no resolver conflicts.
        "$venv_dir/bin/python" -m pip install -q \
            ninja "marisa-trie" "kbnf>=0.4.2" "formatron>=0.5.0" \
            "pydantic>=2.11.0" \
            && info "ExLlamaV3 deps installed." \
            || warn "Some ExLlamaV3 deps failed — check above."

        info "ExLlamaV3 ready. CUDA kernels compile on first use (~1-2 min)."
        info "Optional: install flash_attn for faster attention:"
        info "  PATH=$venv_dir/bin:\$PATH $venv_dir/bin/python -m pip install flash_attn --no-build-isolation"
    else
        warn "ExLlamaV3 not found at $exllama_dir — skipping."
    fi

    # Install trl + datasets>=3 first (trl requires datasets>=3.0.0).
    # Must happen BEFORE kvpress to avoid kvpress downgrading datasets to <3.
    info "Installing trl, datasets>=3 (used by LoRA tools) ..."
    "$venv_dir/bin/python" -m pip install "trl" "datasets>=3.0.0" -q 2>/dev/null || true

    # Install kvpress (runtime KV cache compression, no retraining).
    # kvpress pins datasets<3 which conflicts with trl. Install with --no-deps
    # to skip the stale version constraint; its runtime deps are already present.
    if "$venv_dir/bin/python" -c "import kvpress" &>/dev/null 2>&1; then
        info "kvpress already installed."
    else
        info "Installing kvpress ..."
        "$venv_dir/bin/python" -m pip install kvpress --no-deps -q \
            && info "kvpress installed (--no-deps; trl provides datasets>=3)." \
            || warn "kvpress install failed — run: $venv_dir/bin/python -m pip install kvpress --no-deps"
    fi

    info "Python venv ready: $venv_dir"
    info "Activate with:  source $venv_dir/bin/activate"
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

# ---- optimize-model ----------------------------------------------------
# Optimize the target LLM for maximum inference speed on this workstation.
# Strategies:
#   1. TensorRT-LLM  — NVIDIA's highest-throughput inference engine (GPU)
#   2. LoRA adapters — apply lightweight adaptation without full fine-tuning
#   3. ExLlamaV3     — token-streaming with GreenBoost-aware KV cache
#
# Usage:
#   sudo ./greenboost_setup.sh optimize-model
#   sudo ./greenboost_setup.sh optimize-model --model glm-4.7-flash:q8_0
#   sudo ./greenboost_setup.sh optimize-model --strategy tensorrt
#   sudo ./greenboost_setup.sh optimize-model --strategy lora --adapter /path/to/adapter

cmd_optimize_model() {
    detect_hardware

    local model="glm-4.7-flash:q8_0"
    local strategy="status"
    local lora_adapter=""
    local lora_data=""
    local quant="fp8"
    local compression="0.5"
    local hf_model=""

    # Parse args
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --model)       model="$2";       shift 2 ;;
            --hf-model)    hf_model="$2";    shift 2 ;;
            --strategy)    strategy="$2";    shift 2 ;;
            --adapter)     lora_adapter="$2"; shift 2 ;;
            --data)        lora_data="$2";   shift 2 ;;
            --quant)       quant="$2";       shift 2 ;;
            --compression) compression="$2"; shift 2 ;;
            optimize-model) shift ;;
            *) shift ;;
        esac
    done

    _sect_opt() { echo -e "\n${BLU}── $* ──${NC}"; }
    local venv_dir="/opt/greenboost/venv"
    local exllama_dir="$MODULE_DIR/libraries/exllamav3"
    local trt_edge_dir="$MODULE_DIR/libraries/TensorRT-Edge-LLM"
    local modelopt_dir="$MODULE_DIR/libraries/Model-Optimizer"
    local kvcompress_dir="$MODULE_DIR/libraries/kvcompress"

    info "╔══════════════════════════════════════════════════════════════╗"
    info "║  GreenBoost — Model Optimization                            ║"
    info "║  Copyright (C) 2024-2026 Ferran Duarri                     ║"
    info "╚══════════════════════════════════════════════════════════════╝"
    info "Model    : $model"
    info "GPU      : ${GPU_NAME}  (${GB_PHYS} GB VRAM)"
    info "Strategy : $strategy"
    echo ""

    # ── status: show available strategies and library state ──────────────
    if [[ "$strategy" == "status" || "$strategy" == "all" ]]; then
        _sect_opt "Library Status"
        local ok="${GRN}[ok]${NC}" miss="${YLW}[missing]${NC}"

        echo -e "  ExLlamaV3 (KV cache offload, EXL3 inference):"
        [[ -d "$exllama_dir" ]] \
            && echo -e "    $ok  $exllama_dir" \
            || echo -e "    $miss  $exllama_dir"
        local elv3_cache="$exllama_dir/exllamav3/cache/greenboost.py"
        [[ -f "$elv3_cache" ]] \
            && echo -e "    $ok  CacheLayer_greenboost (GreenBoost DDR4 KV cache bridge)" \
            || echo -e "    $miss  CacheLayer_greenboost not found"

        echo ""
        echo -e "  KVCompress / kvpress (runtime KV cache compression):"
        [[ -d "$kvcompress_dir" ]] \
            && echo -e "    $ok  $kvcompress_dir" \
            || echo -e "    $miss  $kvcompress_dir"

        echo ""
        echo -e "  Model-Optimizer / ModelOpt (FP8/INT4 post-training quantization):"
        [[ -d "$modelopt_dir" ]] \
            && echo -e "    $ok  $modelopt_dir" \
            || echo -e "    $miss  $modelopt_dir"

        echo ""
        echo -e "  TensorRT-Edge-LLM (quantize+export to TRT engine):"
        [[ -d "$trt_edge_dir" ]] \
            && echo -e "    $ok  $trt_edge_dir" \
            || echo -e "    $miss  $trt_edge_dir"

        echo ""
        echo -e "  LoRA fine-tuning (Unsloth, fits 30B in 12 GB VRAM):"
        [[ -d "$MODULE_DIR/libraries/LoRA" ]] \
            && echo -e "    $ok  $MODULE_DIR/libraries/LoRA" \
            || echo -e "    $miss  $MODULE_DIR/libraries/LoRA"

        echo ""
        echo -e "  Python venv: ${venv_dir}"
        [[ -d "$venv_dir" ]] && echo -e "    $ok" || echo -e "    $miss  run: sudo $0 full-install"

        echo ""
        info "Run a specific strategy:"
        info "  sudo $0 optimize-model --strategy exllama   [--hf-model THUDM/glm-4.7-flash-hf]"
        info "  sudo $0 optimize-model --strategy kvcompress [--compression 0.5]"
        info "  sudo $0 optimize-model --strategy modelopt-ptq --hf-model THUDM/glm-4.7-flash-hf"
        info "  sudo $0 optimize-model --strategy tensorrt-edge --hf-model THUDM/glm-4.7-flash-hf"
        info "  sudo $0 optimize-model --strategy lora --hf-model THUDM/glm-4.7-flash-hf --data /path/data.jsonl"
        [[ "$strategy" == "status" ]] && return 0
    fi

    # ── Ollama runtime tuning (always applied for non-status strategies) ──
    if [[ "$strategy" != "status" ]]; then
        _sect_opt "Ollama Runtime Tuning"
        local svc="/etc/systemd/system/ollama.service"
        if [[ -f "$svc" ]]; then
            local changed=0
            local pcore_count=$(( GB_PCORES_MAX + 1 ))
            if ! grep -q "OLLAMA_NUM_THREADS" "$svc"; then
                sed -i "/LD_PRELOAD/a Environment=\"OLLAMA_NUM_THREADS=${pcore_count}\"" "$svc"
                info "  OLLAMA_NUM_THREADS=${pcore_count} (P-cores only, max freq)"
                changed=1
            fi
            if ! grep -q "OLLAMA_GPU_OVERHEAD" "$svc"; then
                sed -i "/LD_PRELOAD/a Environment=\"OLLAMA_GPU_OVERHEAD=268435456\"" "$svc"
                info "  OLLAMA_GPU_OVERHEAD=256MB (more VRAM for model layers)"
                changed=1
            fi
            if ! grep -q "OLLAMA_MAX_QUEUE" "$svc"; then
                sed -i "/LD_PRELOAD/a Environment=\"OLLAMA_MAX_QUEUE=1\"" "$svc"
                info "  OLLAMA_MAX_QUEUE=1 (dedicate all GPU to one request)"
                changed=1
            fi
            [[ $changed -eq 1 ]] && systemctl daemon-reload \
                && info "  Ollama updated — restart: sudo systemctl restart ollama" \
                || info "  Ollama already tuned"
        fi
    fi

    # ── ExLlamaV3 + GreenBoost DDR4 KV cache ─────────────────────────────
    if [[ "$strategy" == "exllama" || "$strategy" == "all" ]]; then
        _sect_opt "ExLlamaV3 + GreenBoost DDR4 KV Cache"
        if [[ ! -d "$exllama_dir" ]]; then
            warn "  ExLlamaV3 not found at $exllama_dir"
        else
            info "  ExLlamaV3: $exllama_dir"
            info "  CacheLayer_greenboost: routes KV cache → Tier 2 DDR4 DMA-BUF pages"
            info "  Benefit: 131K context without VRAM OOM (KV cache in DDR4, not VRAM)"
            echo ""
            # Install if needed
            if [[ -d "$venv_dir" ]]; then
                if ! "$venv_dir/bin/python" -c "import exllamav3" &>/dev/null 2>&1; then
                    info "  Installing ExLlamaV3 ..."
                    "$venv_dir/bin/pip" install -e "$exllama_dir" --no-build-isolation -q \
                        && info "  ExLlamaV3 installed" \
                        || warn "  ExLlamaV3 install failed — run manually"
                else
                    info "  ExLlamaV3: already installed ✓"
                fi
            fi
            echo ""
            info "  Usage (interactive chat with GreenBoost KV cache):"
            info "    $MODULE_DIR/tools/greenboost-exllama.sh --model /path/to/model"
            info ""
            info "  Usage (OpenAI-compatible server on port 8080):"
            info "    $MODULE_DIR/tools/greenboost-exllama.sh --model /path/to/model --mode server"
            echo ""
            info "  Convert HF model to EXL3 (4× smaller, faster decode):"
            info "    $MODULE_DIR/tools/greenboost-exllama.sh --model THUDM/glm-4.7-flash-hf --exl3-convert --bpw 4.0"
            info "  After EXL3 (4bpw, ~8 GB): fits in VRAM — no DDR4 overflow needed"
        fi
    fi

    # ── KVCompress / kvpress ──────────────────────────────────────────────
    if [[ "$strategy" == "kvcompress" || "$strategy" == "all" ]]; then
        _sect_opt "KV Cache Compression (kvpress)"
        if [[ ! -d "$kvcompress_dir" ]]; then
            warn "  kvcompress not found at $kvcompress_dir"
        else
            info "  kvpress: runtime KV cache compression (no retraining required)"
            info "  Compression ratio $compression → keeps ${compression} of KV cache"
            info "  Methods: ExpAttn (recommended), SnapKV, KnormPress"
            info "  Benefit: 2× longer effective context at same VRAM"
            echo ""
            info "  Benchmark on this hardware:"
            info "    $MODULE_DIR/tools/greenboost-kvpress.sh --benchmark --model ${hf_model:-THUDM/glm-4.7-flash-hf}"
            info ""
            info "  Single inference:"
            info "    $MODULE_DIR/tools/greenboost-kvpress.sh \\"
            info "      --model ${hf_model:-THUDM/glm-4.7-flash-hf} \\"
            info "      --compression $compression --method ExpAttn \\"
            info "      --prompt 'Explain quantum entanglement'"
        fi
    fi

    # ── ModelOpt PTQ ──────────────────────────────────────────────────────
    if [[ "$strategy" == "modelopt-ptq" || "$strategy" == "all" ]]; then
        _sect_opt "Post-Training Quantization (ModelOpt ${quant})"
        if [[ ! -d "$modelopt_dir" ]]; then
            warn "  Model-Optimizer not found at $modelopt_dir"
        else
            info "  ModelOpt PTQ: no retraining — calibration only (~5-30 min)"
            info "  Scheme: $quant  |  Model: ${hf_model:-$model}"
            info "  Result: 2-4× smaller model → less Tier 2 DDR4 pressure"
            echo ""
            if [[ -n "$hf_model" ]]; then
                info "  Running PTQ on $hf_model ..."
                local ptq_out="/opt/greenboost/models/$(basename "${hf_model}")-${quant}"
                local py="${venv_dir}/bin/python"
                [[ -x "$py" ]] || py="python3"
                "$py" "$MODULE_DIR/tools/greenboost-ptq.py" \
                    --model "$hf_model" \
                    --quant "$quant" \
                    --output "$ptq_out" \
                    --create-modelfile \
                    || warn "  PTQ failed — check output above"
            else
                info "  To quantize a model:"
                info "    sudo $0 optimize-model --strategy modelopt-ptq --hf-model THUDM/glm-4.7-flash-hf"
                info "    sudo $0 optimize-model --strategy modelopt-ptq --hf-model THUDM/glm-4.7-flash-hf --quant int4_awq"
                info ""
                info "  Manual: $MODULE_DIR/tools/greenboost-ptq.py --model THUDM/glm-4.7-flash-hf"
            fi
        fi
    fi

    # ── TensorRT-Edge-LLM ─────────────────────────────────────────────────
    if [[ "$strategy" == "tensorrt-edge" || "$strategy" == "all" ]]; then
        _sect_opt "TensorRT-Edge-LLM Export"
        if [[ ! -d "$trt_edge_dir" ]]; then
            warn "  TensorRT-Edge-LLM not found at $trt_edge_dir"
        else
            info "  TRT-Edge-LLM: quantize + export to TRT engine (no retraining)"
            info "  Supports: FP8, NVFP4, INT4-AWQ"
            local cc
            cc=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d ' ')
            info "  GPU CC: ${cc:-unknown}  (Blackwell RTX 50xx needs TRT-Edge >= 0.5)"
            echo ""
            if [[ -n "$hf_model" ]]; then
                local trt_out="/opt/greenboost/trt-engines/$(basename "${hf_model}")-${quant}"
                info "  Quantizing $hf_model → $trt_out ..."
                local py="${venv_dir}/bin/python"
                [[ -x "$py" ]] || py="python3"
                if "$py" -c "import tensorrt_edgellm" &>/dev/null 2>&1; then
                    mkdir -p "$trt_out"
                    "$py" -m tensorrt_edgellm.scripts.quantize_llm \
                        --model_dir="$hf_model" \
                        --output_dir="${trt_out}/quantized" \
                        --quantization_scheme="$quant" \
                        || warn "  Quantization failed — see output above"
                else
                    warn "  tensorrt_edgellm not installed in venv"
                    info "  Install: $venv_dir/bin/pip install -e $trt_edge_dir"
                    info "  Then re-run: sudo $0 optimize-model --strategy tensorrt-edge --hf-model $hf_model"
                fi
            else
                info "  To export a model to TRT engine:"
                info "    sudo $0 optimize-model --strategy tensorrt-edge --hf-model THUDM/glm-4.7-flash-hf"
                info "    sudo $0 optimize-model --strategy tensorrt-edge --hf-model /path/to/model --quant nvfp4"
            fi
        fi
    fi

    # ── LoRA fine-tuning ──────────────────────────────────────────────────
    if [[ "$strategy" == "lora" ]]; then
        _sect_opt "LoRA Fine-tuning (Unsloth)"
        info "  Trains LoRA adapter on custom data. Fits 30B in ${GB_PHYS} GB VRAM via 4-bit base."
        info "  GPU: ${GPU_NAME}  (${GB_PHYS} GB VRAM)"
        echo ""
        if [[ -n "$hf_model" && -n "$lora_data" ]]; then
            local py="${venv_dir}/bin/python"
            [[ -x "$py" ]] || py="python3"
            "$py" "$MODULE_DIR/tools/greenboost-lora-train.py" \
                --model "$hf_model" \
                --data  "$lora_data" \
                --output /opt/greenboost/models/lora-adapter \
                || warn "  LoRA training failed — see output above"
        else
            info "  To fine-tune:"
            info "    sudo $0 optimize-model --strategy lora \\"
            info "      --hf-model THUDM/glm-4.7-flash-hf \\"
            info "      --data /path/to/training_data.jsonl"
            info ""
            info "  Data format (JSONL):"
            info "    {\"instruction\": \"...\", \"input\": \"...\", \"output\": \"...\"}"
            info ""
            info "  After training, Ollama GGUF is auto-created:"
            info "    ollama create glm-lora -f /opt/greenboost/models/lora-adapter/gguf/Modelfile"
        fi
    fi

    echo ""
    info "Optimization complete."
    info "Run benchmark: sudo $0 diagnose"
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
    local pcores_count=$(( GB_PCORES_MAX + 1 ))
    local total_cpus_detected; total_cpus_detected=$(nproc)
    _info "OLLAMA_NUM_THREADS=${nthreads:-default}  (detected: ${pcores_count}=P-cores-only, ${total_cpus_detected}=all CPUs)"
    if [[ -n "$nthreads" && "$nthreads" -gt "$total_cpus_detected" ]]; then
        _rec "OLLAMA_NUM_THREADS=${nthreads} exceeds CPU count — try ${pcores_count} (P-cores only) or ${total_cpus_detected} (all CPUs)"
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
    full-install)        cmd_full_install "$@"  ;;
    tune)               cmd_tune               ;;
    tune-grub)          cmd_tune_grub          ;;
    tune-sysctl)        cmd_tune_sysctl        ;;
    tune-libs)          cmd_tune_libs          ;;
    tune-all)           cmd_tune_all           ;;
    status)             cmd_status             ;;
    diagnose)           cmd_diagnose           ;;
    optimize-model)      cmd_optimize_model "$@" ;;
    help|--help|-h)     cmd_help               ;;
    *) die "Unknown command: '$COMMAND'  — use: $0 help" ;;
esac
