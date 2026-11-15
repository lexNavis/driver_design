#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <arpa/inet.h>
#include <net/if.h>

#include <stdbool.h>
#include <time.h>

#define COMM_LEN 16
#define MAX_MAP_SIZE 1024
#define MAX_SOCKETS 8

/* Запись карты 4-tuple -> PID. 
 * IP хранятся в network byte order (как в пакете), порты - в host byte order. */
struct map_entry {
  uint32_t src_ip;   
  uint16_t src_port; 
  uint32_t dst_ip;   
  uint16_t dst_port; 
  pid_t pid;
  char name[COMM_LEN];
};

/* Агрегированные счетчики по PID */
struct pid_stats {
  pid_t pid;
  char name[COMM_LEN];
  unsigned long sent;
  unsigned long received;
};

static struct pid_stats *stats;
static size_t stats_count = 0;

static struct map_entry *entries;
static size_t entries_count = 0;

static const char *map_path = "/tmp/af_packet_map.txt";
static time_t last_mtime = 0;

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t dump_requested = 0;
static volatile sig_atomic_t reload_requested = 0;

static void handler(int sig) { (void)sig; running = 0; }
static void sigusr1_handler(int sig) { (void)sig; dump_requested = 1; }
static void sigusr2_handler(int sig) { (void)sig; reload_requested = 1; }

static void dump_table(struct pid_stats *s, size_t count)
{
  printf("PID | name | sent | received\n");
  for (size_t i = 0; i < count; i++) {
    if (s[i].pid == -1)
      printf("unknown | %s | %lu | %lu\n", s[i].name, s[i].sent, s[i].received);
    else
      printf("%d | %s | %lu | %lu\n", s[i].pid, s[i].name, s[i].sent, s[i].received);
  }
}

/* Загружает карту из файла. 
 * Формат строки в файле: a.b.c.d:port x.y.z.w:port pid name */
static size_t load_map(const char *path, struct map_entry *ent, size_t max)
{
  FILE *f = fopen(path, "r");
  if (!f) return 0;

  char line[256];
  size_t i = 0;

  while (fgets(line, sizeof(line), f) && i < max) {
    char src_ip_str[16], dst_ip_str[16], name[COMM_LEN] = "unknown";
    unsigned short src_port, dst_port;
    pid_t pid;

    /* Парсим строку. Если прочитали меньше 5 полей (нет имени или битая строка) - пропускаем */
    if (sscanf(line, "%15[^:]:%hu %15[^:]:%hu %d %15s",
               src_ip_str, &src_port, dst_ip_str, &dst_port, &pid, name) < 5)
      continue;

    struct in_addr a1, a2;
    /* Преобразуем строку "10.0.2.15" в 32-битное число в network byte order */
    if (inet_pton(AF_INET, src_ip_str, &a1) != 1) continue;
    if (inet_pton(AF_INET, dst_ip_str, &a2) != 1) continue;

    /* Заполняем запись карты */
    ent[i].src_ip = a1.s_addr;
    ent[i].dst_ip = a2.s_addr;
    ent[i].src_port = src_port;
    ent[i].dst_port = dst_port;
    ent[i].pid = pid;
    snprintf(ent[i].name, COMM_LEN, "%s", name);
    
    i++;
  }

  fclose(f);
  return i;
}

/* Перечитывает карту, если файл изменился (проверяем mtime) или пришел сигнал SIGUSR2 */
static void maybe_reload_map(void)
{
  struct stat st;
  if (stat(map_path, &st) != 0) return;

  /* Если файл не менялся и сигнала не было - выходим */
  if (!reload_requested && st.st_mtime == last_mtime) return;

  last_mtime = st.st_mtime;
  reload_requested = 0;

  /* Перечитываем карту и обновляем счетчик записей */
  size_t n = load_map(map_path, entries, MAX_MAP_SIZE);
  entries_count = n;
}

/* Ищет PID по 4-tuple в карте. 
 * Проверяем оба направления, так как пакет может идти как "от нас", так и "к нам". */
static const struct map_entry *
find_pid_by_tuple(const struct map_entry *ent, size_t count,
                   uint32_t src_ip, uint16_t src_port,
                   uint32_t dst_ip, uint16_t dst_port)
{
  for (size_t i = 0; i < count; i++) {
    /* Прямое направление (src -> dst) */
    if (src_ip == ent[i].src_ip && src_port == ent[i].src_port &&
        dst_ip == ent[i].dst_ip && dst_port == ent[i].dst_port) {
      return ent + i;
    }
    /* Обратное направление (dst -> src) */
    if (src_ip == ent[i].dst_ip && src_port == ent[i].dst_port &&
        dst_ip == ent[i].src_ip && dst_port == ent[i].src_port) {
      return ent + i;
    }
  }
  return NULL;
}

/* Обновляет агрегированные счетчики для конкретного PID.
 * Если PID встречается впервые - создает новую запись. */
static void update_stats(pid_t pid, const char *name, size_t len, bool is_sent)
{
  ssize_t target = -1;

  /* Ищем существующую запись по PID */
  for (size_t i = 0; i < stats_count; i++) {
    if (stats[i].pid == pid) { 
      target = (ssize_t)i; 
      break; 
    }
  }

  /* Если не нашли - создаем новую */
  if (target < 0) {
    if (stats_count >= MAX_MAP_SIZE) return; /* Защита от переполнения */
    
    target = (ssize_t)stats_count++;
    stats[target].pid = pid;
    stats[target].sent = 0;
    stats[target].received = 0;
    snprintf(stats[target].name, COMM_LEN, "%s", name);
  }

  /* Прибавляем байты в нужное поле */
  if (is_sent) stats[target].sent += len;
  else         stats[target].received += len;
}

/* Основная функция разбора пакета: Ethernet -> IP -> TCP/UDP */
static void parse_packet(const char *buf, size_t size)
{
  size_t cur = 0;

  /* Проверка на Ethernet */
  if (size < sizeof(struct ethhdr)) return;
  const struct ethhdr *eth = (const struct ethhdr *)buf;
  cur += sizeof(struct ethhdr); /* Сдвигаем указатель на 14 байт */

  /* Работаем только с IPv4 */
  if (eth->h_proto != htons(ETH_P_IP)) return;

  /* Проверка на IP */
  if (size < cur + sizeof(struct iphdr)) return;
  const struct iphdr *ip = (const struct iphdr *)(buf + cur);

  if (ip->version != 4) return;
  if (ip->ihl < 5) return; /* Минимум 20 байт заголовка */

  uint32_t isrc = ip->saddr; /* network order */
  uint32_t idst = ip->daddr; /* network order */
  uint16_t ilen = ntohs(ip->tot_len); /* Общая длина IP-пакета */

  cur += ip->ihl * 4; /* Сдвигаем указатель на длину IP-заголовка */

  uint16_t psrc = 0, pdst = 0;

  /* Определяем протокол транспортного уровня */
  if (ip->protocol == IPPROTO_TCP) {
    if (size < cur + sizeof(struct tcphdr)) return;
    const struct tcphdr *tcp = (const struct tcphdr *)(buf + cur);
    psrc = ntohs(tcp->source);
    pdst = ntohs(tcp->dest);
  } else if (ip->protocol == IPPROTO_UDP) {
    if (size < cur + sizeof(struct udphdr)) return;
    const struct udphdr *udp = (const struct udphdr *)(buf + cur);
    psrc = ntohs(udp->source);
    pdst = ntohs(udp->dest);
  } else {
    return; /* ICMP и прочее не имеют портов - пропускаем */
  }

  /* Ищем PID в карте */
  const struct map_entry *e = find_pid_by_tuple(entries, entries_count, isrc, psrc, idst, pdst);
  if (!e) {
    /* Если в карте нет - записываем в unknown */
    update_stats(-1, "unknown", ilen, false);
    return;
  }

  /* Определяем направление (отправлено или получено) */
  bool is_sent = (e->src_ip == isrc && e->src_port == psrc);
  
  /* Обновляем счетчики для найденного PID */
  update_stats(e->pid, e->name, ilen, is_sent);
}

/* Создает raw-сокет AF_PACKET и привязывает его к сетевому интерфейсу */
static int make_socket_for_iface(const char *ifname)
{
  int sd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if (sd < 0) {
    perror("socket(AF_PACKET) failed");
    return -1;
  }

  /* Получаем числовой индекс интерфейса по имени (enp0s3 -> 2) */
  unsigned int ifindex = if_nametoindex(ifname);
  if (!ifindex) {
    fprintf(stderr, "if_nametoindex(%s) failed\n", ifname);
    close(sd);
    return -1;
  }

  /* Заполняем структуру для bind */
  struct sockaddr_ll sll;
  memset(&sll, 0, sizeof(sll));
  sll.sll_family   = AF_PACKET;
  sll.sll_protocol = htons(ETH_P_ALL);
  sll.sll_ifindex  = (int)ifindex;

  if (bind(sd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
    perror("bind(AF_PACKET) failed");
    close(sd);
    return -1;
  }

  return sd;
}

int main()
{
  setbuf(stdout, NULL); /* Отключаем буферизацию stdout, чтобы логи шли сразу */

  /* Регистрируем обработчики сигналов */
  signal(SIGINT, handler);       /* Ctrl+C - выход */
  signal(SIGUSR1, sigusr1_handler); /* SIGUSR1 - вывести таблицу */
  signal(SIGUSR2, sigusr2_handler); /* SIGUSR2 - перечитать карту */

  stats = calloc(MAX_MAP_SIZE, sizeof(struct pid_stats));
  entries = calloc(MAX_MAP_SIZE, sizeof(struct map_entry));
  if (!stats || !entries) {
    perror("calloc failed");
    return 1;
  }

  /* Первичная загрузка карты */
  maybe_reload_map(); 

  /* Слушаем сразу два интерфейса: внешний и loopback (для локальных тестов) */
  const char *ifaces[] = { "eth0", "lo" };
  const int iface_count = 2;

  int sds[MAX_SOCKETS];
  int sd_count = 0;

  for (int i = 0; i < iface_count; i++) {
    int sd = make_socket_for_iface(ifaces[i]);
    if (sd >= 0) sds[sd_count++] = sd; /* Сохраняем успешно открытые сокеты */
  }

  if (sd_count == 0) {
    fprintf(stderr, "No AF_PACKET sockets created\n");
    return 1;
  }

  char buf[65536]; /* Максимальный размер пакета */

  while (running) {
    /* Проверяем, не нужно ли обновить карту */
    maybe_reload_map(); 

    fd_set rfds;
    FD_ZERO(&rfds);
    int maxfd = -1;
    
    /* Заполняем set файловых дескрипторов */
    for (int i = 0; i < sd_count; i++) {
      FD_SET(sds[i], &rfds);
      if (sds[i] > maxfd) maxfd = sds[i];
    }

    struct timeval tv;
    tv.tv_sec = 1;  /* Ждем 1 секунду (чтобы не блокироваться навсегда) */
    tv.tv_usec = 0;

    /* Ждем пакет на любом из сокетов */
    int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    if (r < 0) {
      if (errno == EINTR) continue; /* Если прервали сигналом - продолжаем */
      perror("select failed");
      break;
    }

    /* Обрабатываем пакеты на всех сокетах, где они появились */
    for (int i = 0; i < sd_count; i++) {
      if (FD_ISSET(sds[i], &rfds)) {
        ssize_t n = recvfrom(sds[i], buf, sizeof(buf), 0, NULL, NULL);
        if (n > 0) parse_packet(buf, (size_t)n);
      }
    }

    /* Если был сигнал на дамп - выводим таблицу */
    if (dump_requested) {
      dump_table(stats, stats_count);
      dump_requested = 0;
    }
  }

  /* Уборка */
  for (int i = 0; i < sd_count; i++) close(sds[i]);
  free(entries);
  free(stats);
  return 0;
}
