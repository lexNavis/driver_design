#!/bin/bash

# Проверка root
if [ "$EUID" -ne 0 ]; then
    echo "Run as root"
    exit 1
fi

ss -p -e -t -u -n 2>/dev/null | grep -oP 'users:\(\("([^"]+)",pid=(\d+).*?ino:(\d+)' | while read -r match; do
    # Извлекаем через sed: name, pid, ino
    name=$(echo "$match" | sed -n 's/.*users:(("\([^"]*\)".*/\1/p' | tr ' ' '_')
    pid=$(echo "$match" | sed -n 's/.*pid=\([0-9]*\).*/\1/p')
    ino=$(echo "$match" | sed -n 's/.*ino:\([0-9]*\).*/\1/p')

    if [ -n "$name" ] && [ -n "$pid" ] && [ -n "$ino" ]; then
        echo "$ino $pid $name" >> /proc/netdriver
    fi
done

echo "Map loaded"