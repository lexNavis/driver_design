#include <linux/module.h> /* MODULE_LICENSE, module_init/exit */
#include <linux/kernel.h> /* printk, KERN_INFO */
#include <linux/init.h>   /* __init, __exit */
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/netfilter.h>      /* nf_hook_ops, nf_register_net_hook */
#include <linux/netfilter_ipv4.h> /* NF_INET_LOCAL_IN, NF_INET_LOCAL_OUT */
#include <linux/ip.h>             /* struct iphdr */
#include <linux/tcp.h>            /* struct tcphdr */
#include <linux/udp.h>            /* struct udphdr */
#include <linux/skbuff.h>         /* skb_header_pointer, skb->... */
#include <linux/proc_fs.h>        /* proc_create, remove_proc_entry */
#include <linux/seq_file.h>       /* seq_file, single_open, seq_printf */
#include <linux/spinlock.h>       /* spin_lock, spin_unlock */
#include <linux/slab.h>           /* kmalloc, kfree */
#include <linux/hashtable.h>      /* hash_add, hash_lookup */
#include <net/net_namespace.h>    /* init_net */
#include <linux/string.h>

#define COMM_LEN 16

/* Прототипы */
static unsigned int netdriver_parse(void *, struct sk_buff *, const struct nf_hook_state *);
static ssize_t proc_read(struct file *, char __user *, size_t, loff_t *);
static ssize_t proc_write(struct file *, const char __user *, size_t, loff_t *);
static void free_inode_records(void);
static void free_pid_records(void);

/* Глобальные переменные */

/* Набор операций для виртуальной ФС */
struct proc_ops proc_ops = {
    .proc_read = proc_read,
    .proc_write = proc_write
  };
/* Число обработанных пакетов */
static unsigned long packet_count = 0;
/* Спинлок для КС в функции-обработчике */
static DEFINE_SPINLOCK(count_lock);
/* Спинлок для доступа к хэш таблице */
static DEFINE_SPINLOCK(table_lock);
/* Хэш-таблица с ключом по inode для хранения информации о процессах */
static DEFINE_HASHTABLE(h_inode, 8); // 256 pids :)
/* Хэш-таблица с ключом по pid для хранения информации о процессах */
static DEFINE_HASHTABLE(h_pid, 8); // 256 pids :)
/* Указатель на открытую виртуальную ФС */
static struct proc_dir_entry *proc_entry;
/* Хук для входящих пакетов */
struct nf_hook_ops income_hook = {
    /* Указание точки привязки хука. Данный хук будет
     * связан с точкой, в которой пакеты приходят
     * на локальную машину */
    .hooknum = NF_INET_LOCAL_IN,
    /* Назначение функции-обработчика, которую вызовет хук. */
    .hook = netdriver_parse,
    /* Указание наивысшего приоритета. Нашу функцию вызовут одной
     * из первых при появлении пакета */
    .priority = NF_IP_PRI_FIRST,
    /* Работаем с пакетами протокола IPv4 */
    .pf = NFPROTO_IPV4};
/* Хук для исходящих пакетов */
struct nf_hook_ops outcome_hook = {
    /* Указание точки привязки хука. Данный хук будет
     * связан с точкой возле пакетов от локальных
     * процессов, уходящие наружу. */
    .hooknum = NF_INET_LOCAL_OUT,
    /* Назначение функции-обработчика, которую вызовет хук. */
    .hook = netdriver_parse,
    /* Указание наивысшего приоритета. Нашу функцию вызовут одной
     * из первых при появлении пакета */
    .priority = NF_IP_PRI_FIRST,
    /* Работаем с пакетами протокола IPv4 */
    .pf = NFPROTO_IPV4};

/* Запись в хэш таблице inode */
struct inode_record
{
  struct hlist_node node;
  unsigned long key;
  pid_t pid;
  unsigned long sent;
  unsigned long received;
};

/* Запись в хэш таблице pid */
struct pid_record
{
  struct hlist_node node;
  pid_t key;
  char name[TASK_COMM_LEN];
  unsigned long sent;
  unsigned long received;
};

static int __init netdriver_init(void)
{
  printk(KERN_INFO "netdriver_parse: netdriver mounted:\n");
  /* Привяжем список операций к виртуальной ФС */
  proc_entry = proc_create("netdriver", 0, NULL, &proc_ops);
  if (!proc_entry)
  {
    printk(KERN_ERR "netdriver_parse: proc_create failed:\n");
    return -EINVAL;
  }
  /* Зарегистрируем хуки */
  nf_register_net_hook(&init_net, &income_hook);
  nf_register_net_hook(&init_net, &outcome_hook);
  return 0;
}

static void __exit netdriver_exit(void)
{
  printk(KERN_INFO "netdriver_parse: netdriver unmounted:\n");
  remove_proc_entry("netdriver", NULL);
  /* Разлогиним хуки */
  nf_unregister_net_hook(&init_net, &income_hook);
  nf_unregister_net_hook(&init_net, &outcome_hook);
  free_inode_records();
  free_pid_records();
}

static unsigned long get_ino(const struct sk_buff *skb)
{
  struct sock *sk = skb->sk;
  if (sk && sk->sk_socket && sk->sk_socket->file && file_inode(sk->sk_socket->file))
  {
    return file_inode(sk->sk_socket->file)->i_ino;
  }
  return 0;
}

static pid_t netdriver_get_pid(const struct sk_buff *skb)
{
  pid_t pid;
  struct sock *sk = skb->sk;
  if (sk && sk->sk_socket && sk->sk_socket->file && sk->sk_socket->file->f_owner.pid)
  {
    pid = pid_vnr(sk->sk_socket->file->f_owner.pid);
    if (pid)
    {
      return pid;
    }
  }
  return current->pid;
}

/* Функции для работы с хэш-таблицей по inode*/

static struct inode_record *find_inode_record(unsigned long ino)
{
  struct inode_record *record;
  hash_for_each_possible(h_inode, record, node, ino)
  {
    if (record->key == ino)
    {
      return record;
    }
  }
  return NULL;
}

static struct inode_record *add_inode_record(unsigned long ino, pid_t pid)
{
  /* Выделим память и проведем первичную инициализацию полей */
  struct inode_record *new = kmalloc(sizeof(struct inode_record), GFP_ATOMIC);
  if (!new)
  {
    return NULL;
  }
  new->key = ino;
  new->pid = pid;
  new->sent = 0;
  new->received = 0;
  hash_add(h_inode, &new->node, ino);
  return new;
}

static void update_inode_record(struct inode_record *rec, int byte_count, bool is_sent)
{
  if (is_sent)
  {
    rec->sent += byte_count;
  }
  else
  {
    rec->received += byte_count;
  }
}
static void free_inode_records(void)
{
  unsigned int bkt;
  struct hlist_node *tmp;
  struct inode_record *rec;
  hash_for_each_safe(h_inode, bkt, tmp, rec, node)
  {
    hash_del(&rec->node);
    kfree(rec);
  }
}

/* Функции для работы с хэш-таблицей по pid */

static struct pid_record *find_pid_record(pid_t pid)
{
  struct pid_record *record;
  hash_for_each_possible(h_pid, record, node, pid)
  {
    if (record->key == pid)
    {
      return record;
    }
  }
  return NULL;
}

static struct pid_record *add_pid_record(pid_t pid, const char *chr)
{
  /* Выделим память и проведем первичную инициализацию полей */
  struct pid_record *new = kmalloc(sizeof(struct pid_record), GFP_ATOMIC);
  if (!new)
  {
    return NULL;
  }
  new->key = pid;
  new->sent = 0;
  new->received = 0;
  strscpy(new->name, chr, TASK_COMM_LEN);
  hash_add(h_pid, &new->node, pid);
  return new;
}

static void update_pid_record(struct pid_record *rec, int byte_count, bool is_sent)
{
  if (is_sent)
  {
    rec->sent += byte_count;
  }
  else
  {
    rec->received += byte_count;
  }
}
static void free_pid_records(void)
{
  unsigned int bkt;
  struct hlist_node *tmp;
  struct pid_record *rec;
  hash_for_each_safe(h_pid, bkt, tmp, rec, node)
  {
    hash_del(&rec->node);
    kfree(rec);
  }
}

static unsigned int netdriver_parse(void *priv, struct sk_buff *skb, const struct nf_hook_state *state)
{
  /* КС для счётчика */
  spin_lock(&count_lock);
  ++packet_count;
  spin_unlock(&count_lock);

  /* Разбор пакета */

  /* Запасной буфер на случай, если skb->data содержит неполную информацию */
  struct iphdr ip_buf;
  struct iphdr *ip = skb_header_pointer(skb, 0, sizeof(ip_buf), &ip_buf);
  /* Проверка на Ip-пакет, если это
   * не он, тогда пропустим его. */
  if (!ip)
  {
    goto accept;
  }

  /* Информацию из полученного пакета необходимо занести в хэш-таблицу:
   * - Если pid новый, то создается новая запись.
   * - Иначе обновляется текущая.
   * Таблица - КС, поэтому берем спинлок. */
  unsigned long ino = get_ino(skb);
  if (ino == 0)
  {
    goto accept;
  }
  bool is_sent = (state->hook == NF_INET_LOCAL_OUT);
  int bytes_count = ntohs(ip->tot_len);
  spin_lock(&table_lock);
  struct inode_record *irecord = find_inode_record(ino);
  if (!irecord)
  {
    irecord = add_inode_record(ino, netdriver_get_pid(skb));
  }
  if (!irecord)
  {
    spin_unlock(&table_lock);
    goto accept;
  }
  pid_t pid = irecord->pid;
  struct pid_record *precord = find_pid_record(pid);
  if (!precord)
  {
    precord = add_pid_record(pid, current->comm);
  }
  if (!precord)
  {
    spin_unlock(&table_lock);
    goto accept;
  }
  update_inode_record(irecord, bytes_count, is_sent);
  update_pid_record(precord, bytes_count, is_sent);
  spin_unlock(&table_lock);
  /* Пакет будет пропущен дальше */
accept:
  return NF_ACCEPT;
}

static ssize_t proc_write(struct file *filp, const char __user *buf, size_t size, loff_t *offset)
{
  char *str = kmalloc(size + 1, GFP_KERNEL);
  if (!str)
    return -ENOMEM;

  ssize_t ret = (ssize_t)size;

  if (copy_from_user(str, buf, size)) {
    ret = -EFAULT;
    goto free_all;
  }
  str[size] = '\0';

  char *tmp = str;
  char *line;
  while ((line = strsep(&str, "\n")) != NULL)
  {
    if (line[0] == '\0')
      continue;

    char *token = strsep(&line, " ");
    if (!token) {
      ret = -EFAULT;
      goto free_all;
    }

    unsigned long ino = 0;
    int err = kstrtoul(token, 10, &ino);
    if (err) {
      ret = err;
      goto free_all;
    }

    token = strsep(&line, " ");
    if (!token) {
      ret = -EFAULT;
      goto free_all;
    }

    pid_t pid = 0;
    err = kstrtoint(token, 10, &pid);
    if (err) {
      ret = err;
      goto free_all;
    }

    spin_lock(&table_lock);
    struct inode_record *irecord = find_inode_record(ino);
    if (!irecord)
      irecord = add_inode_record(ino, pid);

    if (!irecord) {
      spin_unlock(&table_lock);
      ret = -ENOMEM;
      goto free_all;
    }

    struct pid_record *precord = find_pid_record(pid);
    if (!precord) {
      char name[64];
      if (line && line[0] != '\0')
        snprintf(name, sizeof(name), "%s", line);
      else
        snprintf(name, sizeof(name), "%s(c)", current->comm);
      precord = add_pid_record(pid, name);
    }

    if (!precord) {
      spin_unlock(&table_lock);
      ret = -ENOMEM;
      goto free_all;
    }
    spin_unlock(&table_lock);
  }

free_all:
  kfree(tmp);
  return ret;
}

static ssize_t proc_read(struct file *filp, char __user *buf, size_t size, loff_t *offset)
{
  if (*offset > 0)
  {
    printk(KERN_INFO "netdriver_parse: proc/netdriver EOF\n");
    return 0;
  }
  spin_lock(&table_lock);
  /* Два прохода: первый определит размер буфера, который нужен
   * для таблицы, а второй непосредственно сформирует таблицу. */
  int len = sizeof("PID | name | sent | received\n");
  unsigned int bkt;
  struct pid_record *rec;
  hash_for_each(h_pid, bkt, rec, node)
  {
    len += snprintf(NULL, 0, "%d | %s | %lu | %lu\n", rec->key, rec->name, rec->sent, rec->received);
  }
  char *str = kmalloc(len + 1, GFP_KERNEL);
  unsigned int pos = scnprintf(str, len, "PID | name | sent | received\n");
  hash_for_each(h_pid, bkt, rec, node)
  {
    pos += scnprintf(str + pos, len - pos + 256, "%d | %.*s | %lu | %lu\n",
          rec->key, COMM_LEN, rec->name, rec->sent, rec->received);
  }
  spin_unlock(&table_lock);
  unsigned long bytes_remain = copy_to_user(buf, str, pos);
  kfree(str);
  if (bytes_remain)
    return -EFAULT;
  *offset += pos;
  return (ssize_t)pos;
}

module_init(netdriver_init);
module_exit(netdriver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shein Denis");
MODULE_DESCRIPTION("Net driver, which shows amount of bytes sent/received by the processes");
