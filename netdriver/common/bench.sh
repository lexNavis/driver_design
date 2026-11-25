#!/bin/bash
# bench.sh — замер производительности трёх механизмов (без perf/pidstat)
# Запуск: sudo ./bench.sh
set -u
cd "$(dirname "$0")"

export PATH=$PATH:/usr/sbin:/usr/bin

IFACE="eth0"
IPERF_DUR=10           # секунд на прогон
IPERF_RATE="100M"      # ограничение скорости для QEMU
REPEATS=3              # повторов на механизм
OUT="../results/bench"
RAW="$OUT/raw"
mkdir -p "$RAW"

# ---------- проверка инструментов ----------
need() { command -v "$1" >/dev/null 2>&1 || { echo "НЕТ: $1"; MISSING=1; }; }
MISSING=0
need iperf; need bpftool; need tc; need jq
[ "$MISSING" = 1 ] && { echo "Установи: apt install iperf iproute2 bpftool jq"; exit 1; }

# ---------- проверка root ----------
[ "$EUID" -ne 0 ] && { echo "Запускай от root"; exit 1; }

# ---------- вспомогательные ----------
median() { sort -n | awk '{a[NR]=$1} END{print (NR%2)?a[(NR+1)/2]:(a[NR/2]+a[NR/2+1])/2}'; }

# Функция для получения тиков CPU из /proc/stat
get_cpu_ticks() {
    awk '/^cpu / {print $2+$3+$4+$5+$6+$7+$8}' /proc/stat
}

# Запуск iperf-сервера (версия 2)
pkill -f "iperf -s" 2>/dev/null
iperf -s -p 5201 >/dev/null 2>&1 &
IPERF_SRV=$!
sleep 1

# Функция: прогон iperf, возвращает "cpu_seconds throughput_mbps"
run_iperf() {
    local tag="$1"
    local iperf_out="$RAW/${tag}_iperf.txt"

    # Замер тиков ДО
    local ticks_before=$(get_cpu_ticks)

    # Сам трафик. iperf2: -c client, -t time, -b bandwidth, -y C (CSV output)
    iperf -c 127.0.0.1 -p 5201 -t "$IPERF_DUR" -b "$IPERF_RATE" -y C > "$iperf_out" 2>/dev/null

    # Замер тиков ПОСЛЕ
    local ticks_after=$(get_cpu_ticks)
    
    # Считаем разницу (обычно 1 тик = 1/100 секунды)
    local ticks_diff=$((ticks_after - ticks_before))
    local cpu_sec=$(awk -v t="$ticks_diff" 'BEGIN{printf "%.2f", t/100}')

    # Парсим throughput (Mbps) из CSV iperf2 (поле 9)
    local mbps=$(awk -F, '/^[0-9]/{val=$9} END{printf "%.2f", val/1e6}' "$iperf_out" 2>/dev/null)
    [ -z "$mbps" ] && mbps=0

    echo "$cpu_sec $mbps"
}

# ============================================================
# 1. БАЗОВАЯ ЛИНИЯ
# ============================================================
echo "=== [1/4] Базовая линия (без механизмов) ==="
BASE_CPU=(); BASE_TP=()
for i in $(seq 1 $REPEATS); do
    read -r cpu tp < <(run_iperf "base_$i")
    BASE_CPU+=("$cpu"); BASE_TP+=("$tp")
    echo "  прогон $i: cpu=${cpu}s tp=${tp}Mbps"
done
BASE_CPU_M=$(printf '%s\n' "${BASE_CPU[@]}" | median)
BASE_TP_M=$(printf '%s\n' "${BASE_TP[@]}" | median)

# ============================================================
# 2. NETFILTER
# ============================================================
echo "=== [2/4] netfilter ==="
NF_CPU=(); NF_TP=(); NF_COUNTERS=()
for i in $(seq 1 $REPEATS); do
    cd ../netfilter
    insmod netdriver.ko || { echo "insmod failed"; cd ../common; continue; }
    sudo ../common/load_map.sh >/dev/null 2>&1
    sleep 0.5

    read -r cpu tp < <(run_iperf "nf_$i")

    cat /proc/netdriver > "$RAW/nf_${i}_counters.txt" 2>/dev/null
    local_nonzero=$(awk 'NR>1 && ($3>0 || $4>0)' "$RAW/nf_${i}_counters.txt" | wc -l)
    NF_COUNTERS+=("$local_nonzero")

    rmmod netdriver 2>/dev/null
    cd ../common
    NF_CPU+=("$cpu"); NF_TP+=("$tp")
    echo "  прогон $i: cpu=${cpu}s tp=${tp}Mbps строк_с_данными=$local_nonzero"
done
NF_CPU_M=$(printf '%s\n' "${NF_CPU[@]}" | median)
NF_TP_M=$(printf '%s\n' "${NF_TP[@]}" | median)
NF_NONZERO_M=$(printf '%s\n' "${NF_COUNTERS[@]}" | median)

# ============================================================
# 3. AF_PACKET
# ============================================================
echo "=== [3/4] AF_PACKET ==="
AF_CPU=(); AF_TP=()
for i in $(seq 1 $REPEATS); do
    pkill -9 af_packet 2>/dev/null
    rm -f /tmp/af_packet_map.txt
    cd ../af_packet

    ./af_packet > "$RAW/af_${i}_out.txt" 2>&1 &
    AF_PID=$!
    sleep 1
    if ! kill -0 "$AF_PID" 2>/dev/null; then
        echo "  af_packet не запустился"; cd ../common; continue
    fi

    sudo ../common/build_map_afpacket.sh >/dev/null 2>&1

    # Замер тиков ДО
    ticks_before=$(get_cpu_ticks)

    iperf -c 127.0.0.1 -p 5201 -t "$IPERF_DUR" -b "$IPERF_RATE" -y C > "$RAW/af_${i}_iperf.txt" 2>/dev/null

    # Замер тиков ПОСЛЕ
    ticks_after=$(get_cpu_ticks)
    ticks_diff=$((ticks_after - ticks_before))
    cpu_s=$(awk -v t="$ticks_diff" 'BEGIN{printf "%.2f", t/100}')

    kill -USR2 "$AF_PID" 2>/dev/null; sleep 0.3
    kill -USR1 "$AF_PID" 2>/dev/null; sleep 0.5
    kill -INT  "$AF_PID" 2>/dev/null
    wait "$AF_PID" 2>/dev/null

    AF_CPU+=("$cpu_s")

    mbps=$(awk -F, '/^[0-9]/{val=$9} END{printf "%.2f", val/1e6}' "$RAW/af_${i}_iperf.txt" 2>/dev/null)
    [ -z "$mbps" ] && mbps=0
    AF_TP+=("$mbps")

    cd ../common
    echo "  прогон $i: cpu=${cpu_s}s tp=${mbps}Mbps"
done
AF_CPU_M=$(printf '%s\n' "${AF_CPU[@]}" | median)
AF_TP_M=$(printf '%s\n' "${AF_TP[@]}" | median)

# ============================================================
# 4. eBPF/TC
# ============================================================
echo "=== [4/4] eBPF/TC ==="
EB_CPU=(); EB_TP=(); EB_STATS=()
for i in $(seq 1 $REPEATS); do
    cd ../ebpf_tc
    tc qdisc del dev "$IFACE" clsact 2>/dev/null
    rm -rf /sys/fs/bpf/tc_prog
    mkdir -p /sys/fs/bpf/tc_prog

    clang -O2 -g -target bpf -c tc_prog.bpf.c -o tc_prog.bpf.o 2>/dev/null || { echo "clang fail"; cd ../common; continue; }
    bpftool prog loadall tc_prog.bpf.o /sys/fs/bpf/tc_prog 2>/dev/null || { echo "bpftool load fail"; cd ../common; continue; }

    tc qdisc add dev "$IFACE" clsact 2>/dev/null
    tc filter add dev "$IFACE" ingress bpf da pinned /sys/fs/bpf/tc_prog/prog_ingress 2>/dev/null
    tc filter add dev "$IFACE" egress  bpf da pinned /sys/fs/bpf/tc_prog/prog_egress  2>/dev/null

    read -r cpu tp < <(run_iperf "eb_$i")

    bpftool map dump name stats_map -j > "$RAW/eb_${i}_map.json" 2>/dev/null
    n_entries=$(jq 'length' "$RAW/eb_${i}_map.json" 2>/dev/null || echo 0)
    EB_STATS+=("$n_entries")

    tc filter del dev "$IFACE" ingress 2>/dev/null
    tc filter del dev "$IFACE" egress  2>/dev/null
    tc qdisc del dev "$IFACE" clsact 2>/dev/null
    rm -rf /sys/fs/bpf/tc_prog

    cd ../common
    EB_CPU+=("$cpu"); EB_TP+=("$tp")
    echo "  прогон $i: cpu=${cpu}s tp=${tp}Mbps записей_в_map=$n_entries"
done
EB_CPU_M=$(printf '%s\n' "${EB_CPU[@]}" | median)
EB_TP_M=$(printf '%s\n' "${EB_TP[@]}" | median)
EB_STATS_M=$(printf '%s\n' "${EB_STATS[@]}" | median)

# ---------- остановка сервера ----------
kill "$IPERF_SRV" 2>/dev/null

# ============================================================
# ИТОГОВАЯ ТАБЛИЦА
# ============================================================
SUMMARY="$OUT/summary.txt"
{
echo "================================================================"
echo " Замер производительности (вариант 8), iface=$IFACE, iperf2 -b $IPERF_RATE, ${IPERF_DUR}s, медиана из $REPEATS"
echo "================================================================"
printf "%-18s | %-14s | %-16s | %-14s | %s\n" "Механизм" "CPU системы(s)" "ΔCPU vs база(s)" "Throughput(Mbps)" "Примечание"
printf -- "-------------------+----------------+------------------+----------------+-------------------------\n"
printf "%-18s | %-14s | %-16s | %-14s | %s\n" "Базовая линия" "$BASE_CPU_M" "0" "$BASE_TP_M" "iperf2 без механизмов"
printf "%-18s | %-14s | %-16s | %-14s | %s\n" "netfilter" "$NF_CPU_M" "$(awk -v a=$NF_CPU_M -v b=$BASE_CPU_M 'BEGIN{printf "%.2f", a-b}')" "$NF_TP_M" "строк с данными: $NF_NONZERO_M"
printf "%-18s | %-14s | %-16s | %-14s | %s\n" "AF_PACKET" "$AF_CPU_M" "$(awk -v a=$AF_CPU_M -v b=$BASE_CPU_M 'BEGIN{printf "%.2f", a-b}')" "$AF_TP_M" "CPU системы"
printf "%-18s | %-14s | %-16s | %-14s | %s\n" "eBPF/TC" "$EB_CPU_M" "$(awk -v a=$EB_CPU_M -v b=$BASE_CPU_M 'BEGIN{printf "%.2f", a-b}')" "$EB_TP_M" "записей в map: $EB_STATS_M"
echo "----------------------------------------------------------------"
echo ""
echo "Сырые логи: $RAW/"
} | tee "$SUMMARY"

echo ""
echo "Готово: $SUMMARY"
