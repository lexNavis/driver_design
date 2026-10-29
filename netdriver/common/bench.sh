#!/bin/bash
# bench.sh — замер производительности трёх механизмов (вариант 8)
# Запуск: sudo ./bench.sh
set -u
cd "$(dirname "$0")"

IFACE="enp0s3"
LOOP_IFACE="lo"
IPERF_DUR=10           # секунд на прогон
IPERF_RATE="200M"      # ограничение скорости, чтобы VirtualBox не захлебнулся
REPEATS=3              # повторов на механизм (берём медиану)
OUT="../results/bench"
RAW="$OUT/raw"
mkdir -p "$RAW"

# ---------- проверка инструментов ----------
need() { command -v "$1" >/dev/null 2>&1 || { echo "НЕТ: $1"; MISSING=1; }; }
MISSING=0
need iperf3; need perf; need pidstat; need bpftool; need jq; need tc
[ "$MISSING" = 1 ] && { echo "Установи недостающее и повтори."; exit 1; }

# ---------- проверка root ----------
[ "$EUID" -ne 0 ] && { echo "Запускай от root"; exit 1; }

# ---------- вспомогательные ----------
median() { sort -n | awk '{a[NR]=$1} END{print (NR%2)?a[(NR+1)/2]:(a[NR/2]+a[NR/2+1])/2}'; }

# Запуск iperf3-сервера один раз
pkill -f "iperf3 -s" 2>/dev/null
iperf3 -s -p 5201 >/dev/null 2>&1 &
IPERF_SRV=$!
sleep 1

# Функция: прогон iperf3 через loopback + perf stat -a, возвращает "cpu_cycles throughput_mbps"
run_iperf() {
    local tag="$1"
    local perf_out="$RAW/${tag}_perf.txt"
    local iperf_out="$RAW/${tag}_iperf.txt"

    # perf stat -a в фоне на время прогона
    LC_ALL=C perf stat -a -e cycles,instructions,cpu-clock -o "$perf_out" -- sleep "$IPERF_DUR" &
    local PERF_PID=$!

    # сам трафик
    iperf3 -c 127.0.0.1 -p 5201 -t "$IPERF_DUR" -b "$IPERF_RATE" -J > "$iperf_out" 2>/dev/null

    wait "$PERF_PID" 2>/dev/null

    # парсим throughput (Mbps) из JSON iperf3
    local mbps
    mbps=$(jq -r '.end.sum_received.bits_per_second // 0' "$iperf_out" 2>/dev/null)
    mbps=$(awk -v b="$mbps" 'BEGIN{printf "%.2f", b/1e6}')

    # парсим cpu-clock (seconds) из perf
    local cpu_s
    cpu_s=$(LC_ALL=C awk '/cpu-clock/ {gsub(",","",$1); print $1}' "$perf_out" 2>/dev/null | head -1)
    [ -z "$cpu_s" ] && cpu_s=0

    echo "$cpu_s $mbps"
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

    # запускаем трафик с perf
    read -r cpu tp < <(run_iperf "nf_$i")

    # снимаем счётчики ПОСЛЕ трафика
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
AF_CPU_PROC=(); AF_TP=()
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

    # обновляем карту перед трафиком
    sudo ../common/build_map_afpacket.sh >/dev/null 2>&1

    # perf stat -a + pidstat на процесс af_packet
    perf stat -a -e cycles,instructions,cpu-clock -o "$RAW/af_${i}_perf.txt" -- sleep "$IPERF_DUR" &
    PERF_PID=$!
    pidstat -p "$AF_PID" 1 "$IPERF_DUR" > "$RAW/af_${i}_pidstat.txt" 2>/dev/null &
    PIDSTAT_PID=$!

    iperf3 -c 127.0.0.1 -p 5201 -t "$IPERF_DUR" -b "$IPERF_RATE" -J > "$RAW/af_${i}_iperf.txt" 2>/dev/null

    wait "$PERF_PID" 2>/dev/null
    wait "$PIDSTAT_PID" 2>/dev/null

    # сигналы: USR2=reload, USR1=dump, INT=exit
    kill -USR2 "$AF_PID" 2>/dev/null; sleep 0.3
    kill -USR1 "$AF_PID" 2>/dev/null; sleep 0.5
    kill -INT  "$AF_PID" 2>/dev/null
    wait "$AF_PID" 2>/dev/null

    # CPU процесса af_packet (средний %CPU из pidstat, приводим к секундам)
    af_cpu_s=$(awk '/Average:/ && $NF!="CPU" {sum+=$8; n++} END{if(n) printf "%.4f", (sum/n)*'"$IPERF_DUR"'/100; else print 0}' "$RAW/af_${i}_pidstat.txt")
    AF_CPU_PROC+=("$af_cpu_s")

    mbps=$(jq -r '.end.sum_received.bits_per_second // 0' "$RAW/af_${i}_iperf.txt" 2>/dev/null)
    mbps=$(awk -v b="$mbps" 'BEGIN{printf "%.2f", b/1e6}')
    AF_TP+=("$mbps")

    cd ../common
    echo "  прогон $i: cpu_af_packet=${af_cpu_s}s tp=${mbps}Mbps"
done
AF_CPU_M=$(printf '%s\n' "${AF_CPU_PROC[@]}" | median)
AF_TP_M=$(printf '%s\n' "${AF_TP[@]}" | median)

# ============================================================
# 4. eBPF/TC
# ============================================================
echo "=== [4/4] eBPF/TC ==="
EB_CPU=(); EB_TP=(); EB_STATS=()
for i in $(seq 1 $REPEATS); do
    cd ../ebpf_tc
    # чистим предыдущее
    tc qdisc del dev "$IFACE" clsact 2>/dev/null
    rm -rf /sys/fs/bpf/tc_prog
    mkdir -p /sys/fs/bpf/tc_prog

    clang -O2 -g -target bpf -c tc_prog.bpf.c -o tc_prog.bpf.o 2>/dev/null || { echo "clang fail"; cd ../common; continue; }
    bpftool prog loadall tc_prog.bpf.o /sys/fs/bpf/tc_prog 2>/dev/null || { echo "bpftool load fail"; cd ../common; continue; }

    tc qdisc add dev "$IFACE" clsact 2>/dev/null
    tc filter add dev "$IFACE" ingress bpf da pinned /sys/fs/bpf/tc_prog/prog_ingress 2>/dev/null
    tc filter add dev "$IFACE" egress  bpf da pinned /sys/fs/bpf/tc_prog/prog_egress  2>/dev/null

    read -r cpu tp < <(run_iperf "eb_$i")

    # снимаем статистику map
    bpftool map dump name stats_map -j > "$RAW/eb_${i}_map.json" 2>/dev/null
    n_entries=$(jq 'length' "$RAW/eb_${i}_map.json" 2>/dev/null || echo 0)
    EB_STATS+=("$n_entries")

    # чистим tc
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
echo " Замер производительности (вариант 8), iface=$IFACE, iperf3 -b $IPERF_RATE, ${IPERF_DUR}s, медиана из $REPEATS"
echo "================================================================"
printf "%-18s | %-14s | %-16s | %-14s | %s\n" "Механизм" "CPU системы(s)" "ΔCPU vs база(s)" "Throughput(Mbps)" "Примечание"
printf -- "-------------------+----------------+------------------+----------------+-------------------------\n"
printf "%-18s | %-14s | %-16s | %-14s | %s\n" "Базовая линия" "$BASE_CPU_M" "0" "$BASE_TP_M" "iperf3 без механизмов"
printf "%-18s | %-14s | %-16s | %-14s | %s\n" "netfilter" "$NF_CPU_M" "$(awk -v a=$NF_CPU_M -v b=$BASE_CPU_M 'BEGIN{printf "%.2f", a-b}')" "$NF_TP_M" "строк с данными: $NF_NONZERO_M"
printf "%-18s | %-14s | %-16s | %-14s | %s\n" "AF_PACKET" "см. ниже" "см. ниже" "$AF_TP_M" "CPU процесса: ${AF_CPU_M}s"
printf "%-18s | %-14s | %-16s | %-14s | %s\n" "eBPF/TC" "$EB_CPU_M" "$(awk -v a=$EB_CPU_M -v b=$BASE_CPU_M 'BEGIN{printf "%.2f", a-b}')" "$EB_TP_M" "записей в map: $EB_STATS_M"
echo "----------------------------------------------------------------"
echo ""
echo "ВАЖНО:"
echo " - Для AF_PACKET 'CPU системы' включает и ядро, и userspace; отдельно"
echo "   показан CPU самого процесса af_packet (${AF_CPU_M}s за прогон)."
echo " - AF_PACKET не может дропать/менять чужие пакеты: его throughput —"
echo "   это пропущенный iperf3-трафик, а не 'отброшенный'. Счётчики —"
echo "   сколько байт программа успела прочитать и сопоставить с PID."
echo " - netfilter и eBPF/TC работают в ядре, поэтому их оверхед виден в"
echo "   perf stat -a (cycles, cpu-clock)."
echo " - Если у netfilter 'строк с данными' = 0, значит skb->sk не даёт ino"
echo "   для этого трафика (см. отчёт), и замер счётчиков невалиден."
echo ""
echo "Сырые логи: $RAW/"
} | tee "$SUMMARY"

echo ""
echo "Готово: $SUMMARY"