# Создание словаря ip - pid
declare -A pid_map
while read -r local peer pid name; do
    pid_map["$local|$peer"]="$pid $name"
    # Получаем информацию о tcp соединениях, 
    # чтобы достать оттуда необходимые поля
done < <(ss -4 -p -e -t -n 2>/dev/null | tail -n +2 |
    while read -r line; do
    local=$(echo "$line" | awk '{print $4}')
    peer=$(echo "$line" | awk '{print $5}')
    pid=$(echo "$line" | grep -oP 'pid=\K[0-9]+' | head -1)
    name=$(echo "$line" | grep -oP 'users:\(\("\K[^"]+' | head -1)
    # Если поле pid пусто, оно нам не нужно
    [ -z "$pid" ] && continue
    # Данный набор значений отправится в цикл над текущим
    echo "$local $peer $pid ${name:-unknown}"
done)

# Словари для агрегации по pid
declare -A agg_sent
declare -A agg_recv
declare -A agg_name

while read -r sip sport dip dport sent recv; do
    # sip и dip — числа (network order, little-endian в памяти)
    # В памяти они хранятся сплошным числом, поэтому преобразование
    # необходимо для совпадения с ключом в словаре
    sip_str=$(printf "%d.%d.%d.%d" \
        $((sip & 0xFF)) $(((sip >> 8) & 0xFF)) $(((sip >> 16) & 0xFF)) $(((sip >> 24) & 0xFF)))
    dip_str=$(printf "%d.%d.%d.%d" \
        $((dip & 0xFF)) $(((dip >> 8) & 0xFF)) $(((dip >> 16) & 0xFF)) $(((dip >> 24) & 0xFF)))
    key1="$sip_str:$sport|$dip_str:$dport"
    key2="$dip_str:$dport|$sip_str:$sport"
    if [[ -n "${pid_map[$key1]}" ]]; then
        read -r pid name <<< "${pid_map[$key1]}"
    elif [[ -n "${pid_map[$key2]}" ]]; then
        read -r pid name <<< "${pid_map[$key2]}"
    else
        continue  # пропустить unknown
    fi
    agg_sent[$pid]=$(( ${agg_sent[$pid]:-0} + sent ))
    agg_recv[$pid]=$(( ${agg_recv[$pid]:-0} + recv ))
    agg_name[$pid]="$name"
# Выгрузка данных о пакетах в формате json
done < <(sudo bpftool map dump name stats_map -j |
jq -r '.[] | "\(.formatted.key.src_ip) \(.formatted.key.src_port) \(.formatted.key.dst_ip) \(.formatted.key.dst_port) \(.formatted.value.sent) \(.formatted.value.received)"')

echo "PID | name | sent | received"
# Перебор ключей ассоциативного массива
for pid in "${!agg_sent[@]}"; do
    echo "$pid | ${agg_name[$pid]} | ${agg_sent[$pid]} | ${agg_recv[$pid]}"
done
