#!/bin/bash
# Сборка, загрузка, запуск тестов, выгрузка.
# Использование: sudo ./run.sh

set -e   # остановиться при первой ошибке

# --- проверка, что запущено от root ---
if [ "$EUID" -ne 0 ]; then
    echo "Запусти через sudo: sudo ./run.sh"
    exit 1
fi

# --- пути (скрипт лежит рядом с исходниками) ---
DIR="$(cd "$(dirname "$0")" && pwd)"
MODULE="rngdriver"
DEVICE="/dev/rngdriver"
TEST_BIN="rng_test"

cd "$DIR"

echo "=== 1. Сборка модуля ==="
make

echo "=== 2. Выгрузка старого модуля ==="
if lsmod | grep -q "^${MODULE} "; then
    # убить процессы, которые держат устройство
    if command -v fuser >/dev/null 2>&1; then
        fuser -k "$DEVICE" 2>/dev/null || true
        sleep 0.5
    fi
    rmmod "$MODULE" || { echo "Не удалось выгрузить $MODULE"; exit 1; }
    echo "  Старый модуль выгружен"
else
    echo "  Модуль не был загружен"
fi

echo "=== 3. Загрузка модуля ==="
insmod "${MODULE}.ko"
sleep 0.2
if [ ! -e "$DEVICE" ]; then
    echo "ОШИБКА: $DEVICE не появился"
    dmesg | tail -5
    exit 1
fi
echo "  $DEVICE готов"

echo "=== 4. Сборка user-программы ==="
gcc -Wall -O2 -o "$TEST_BIN" rng_test.c

echo "=== 5. Запуск тестов ==="
"./$TEST_BIN"

echo "=== 6. Выгрузка модуля ==="
if command -v fuser >/dev/null 2>&1; then
    fuser -k "$DEVICE" 2>/dev/null || true
    sleep 0.3
fi
rmmod "$MODULE"
echo "  Модуль выгружен"

echo "=== Готово ==="
dmesg | tail -3