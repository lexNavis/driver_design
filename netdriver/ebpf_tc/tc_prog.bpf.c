#include "vmlinux.h"
#include <bpf/bpf_helpers.h>

#define TC_ACT_OK 0
#define ETH_P_IP 0x0800

/* В eBPF нет libc, поэтому htons/ntohs реализуем через встроенную функцию компилятора */
#define bpf_htons(x) __builtin_bswap16(x)
#define bpf_ntohs(x) __builtin_bswap16(x)

/* Счётчики байт для конкретного соединения */
struct stats
{
  __u64 sent;
  __u64 received;
};

/* Ключ, по которому агрегируем статистику. 
 * IP и порты хранятся в host byte order (после ntohs/bswap). */
struct tuple
{
  __u32 src_ip;
  __u32 dst_ip;
  __u16 src_port;
  __u16 dst_port;
};

/* BPF-map (hash) для хранения статистики. 
 * Ключ - struct tuple, значение - struct stats. */
struct
{
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 1024);
  __type(key, struct tuple);
  __type(value, struct stats);
} stats_map SEC(".maps");

/* Извлекает 4-tuple из пакета, находит или создаёт запись в map и возвращает указатель на неё.
 * Возвращает NULL, если пакет не TCP/UDP или заголовки не влезли в буфер. */
static __always_inline struct stats *get_stats(const struct __sk_buff *skb)
{
  /* Указатели на начало и конец данных пакета. В eBPF они имеют тип __u32 (смещения) */
  void *data = (void *)(long)skb->data;
  void *data_end = (void *)(long)skb->data_end;

  /* Проверка границ: Ethernet-заголовок должен влезть целиком */
  struct ethhdr *eth = data;
  if ((void *)(eth + 1) > data_end)
  {
    return NULL;
  }

  /* Работаем только с IPv4 */
  if (eth->h_proto != bpf_htons(ETH_P_IP))
  {
    return NULL;
  }

  /* Проверка границ: IP-заголовок должен влезть целиком */
  struct iphdr *ip = (struct iphdr *)(eth + 1);
  if ((void *)(ip + 1) > data_end)
  {
    return NULL;
  }
  
  /* Минимальная длина IP-заголовка - 20 байт (5 слов по 4 байта) */
  if (ip->ihl < 5)
  {
    return NULL;
  }

  __u16 sport;
  __u16 dport;

  /* Разбор транспортного уровня */
  if (ip->protocol == IPPROTO_TCP)
  {
    /* Смещение до TCP-заголовка: начало IP + длина IP-заголовка (ihl * 4) */
    struct tcphdr *tcp = (struct tcphdr *)((void *)ip + ip->ihl * 4);
    if ((void *)ip + ip->ihl * 4 + sizeof(struct tcphdr) > data_end)
    {
      return NULL;
    }
    sport = bpf_ntohs(tcp->source);
    dport = bpf_ntohs(tcp->dest);
  }
  else if (ip->protocol == IPPROTO_UDP)
  {
    struct udphdr *udp = (struct udphdr *)((void *)ip + ip->ihl * 4);
    if ((void *)ip + ip->ihl * 4 + sizeof(struct udphdr) > data_end)
    {
      return NULL;
    }
    sport = bpf_ntohs(udp->source);
    dport = bpf_ntohs(udp->dest);
  }
  else
  {
    /* ICMP и прочее не имеют портов - пропускаем */
    return NULL;
  }

  /* Нормализация ключа: всегда сортируем пары (IP, порт), 
   * чтобы пакеты в прямом и обратном направлении попадали в одну запись */
  __u32 a_ip = ip->saddr, b_ip = ip->daddr;
  __u16 a_port = sport, b_port = dport;
  
  if (a_ip > b_ip || (a_ip == b_ip && a_port > b_port))
  {
    /* Меняем местами, если первая пара "больше" второй */
    __u32 t_ip = a_ip;
    a_ip = b_ip;
    b_ip = t_ip;
    __u16 t_port = a_port;
    a_port = b_port;
    b_port = t_port;
  }
  
  /* Формируем ключ для поиска в map */
  struct tuple key = {a_ip, b_ip, a_port, b_port};

  /* Ищем запись в map */
  struct stats *val = bpf_map_lookup_elem(&stats_map, &key);
  if (!val)
  {
    /* Если нет - создаём с нулями и сразу получаем указатель */
    struct stats zero = {0, 0};
    bpf_map_update_elem(&stats_map, &key, &zero, BPF_NOEXIST);
    val = bpf_map_lookup_elem(&stats_map, &key);
  }
  return val;
}

/* Программа для входящих пакетов (ingress) */
SEC("tc/ingress")
int prog_ingress(struct __sk_buff *skb)
{
  /* Определяем полную длину пакета как разницу между концом и началом данных */
  void *data = (void *)(long)skb->data;
  void *data_end = (void *)(long)skb->data_end;
  
  /* Ищем/создаём запись в map */
  struct stats *val = get_stats(skb);
  if (val)
  {
    /* Это входящий пакет - добавляем байты в received */
    val->received += (data_end - data);
  }
  return TC_ACT_OK;
}

/* Программа для исходящих пакетов (egress) */
SEC("tc/egress")
int prog_egress(struct __sk_buff *skb)
{
  void *data = (void *)(long)skb->data;
  void *data_end = (void *)(long)skb->data_end;
  
  struct stats *val = get_stats(skb);
  if (val)
  {
    /* Это исходящий пакет - добавляем байты в sent */
    val->sent += (data_end - data);
  }
  return TC_ACT_OK;
}

char _license[] SEC("license") = "GPL";