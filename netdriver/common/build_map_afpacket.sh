#!/bin/bash

if [ "$EUID" -ne 0 ]; then
    echo "Run as root"
    exit 1
fi

OUT="/tmp/af_packet_map.txt"

# Если файл не существует — создаём
touch "$OUT"

ss -4 -p -e -t -n 2>/dev/null | tail -n +2 | while read -r line; do
    local=$(echo "$line" | awk '{print $4}')
    peer=$(echo "$line" | awk '{print $5}')
    pid=$(echo "$line" | grep -oP 'pid=\K[0-9]+' | head -1)
    name=$(echo "$line" | grep -oP 'users:\(\("\K[^"]+' | head -1 | tr -d '\r\n')

    [ -z "$pid" ] && continue
    [ -z "$name" ] && name="unknown"

    # Добавляем только если такой строки ещё нет
    grep -qF "$local $peer $pid $name" "$OUT" || echo "$local $peer $pid $name" >> "$OUT"
done

echo "Map updated: $OUT"