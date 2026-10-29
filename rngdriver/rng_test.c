#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "rng_ioctl.h"

#define DEVICE "/dev/rngdriver"
#define NBYTES 4096 /* сколько байт читаем для тестов */

/* Прочитать n байт из устройства. Возвращает 0 при успехе, -1 при ошибке. */
static int read_bytes(int fd, uint8_t *buf, size_t n)
{
  size_t got = 0;
  while (got < n)
  {
    ssize_t r = read(fd, buf + got, n - got);
    if (r < 0)
    {
      if (errno == EINTR)
        continue; /* прервали сигналом — повторить */
      perror("read");
      return -1;
    }
    if (r == 0)
    {
      fprintf(stderr, "read: unexpected EOF\n");
      return -1;
    }
    got += (size_t)r;
  }
  return 0;
}

/* Частотный тест: считаем долю единичных бит.
 * Для случайных данных ожидается ~0.5.
 * Возвращает долю единиц (0..1). */
static double freq_test(const uint8_t *buf, size_t n)
{
  size_t ones = 0;
  size_t total_bits = n * 8;

  for (size_t i = 0; i < n; i++)
  {
    uint8_t b = buf[i];
    /* Считаем единицы в байте: классический трюк Кернигана */
    while (b)
    {
      ones += b & 1u;
      b >>= 1;
    }
  }
  return (double)ones / (double)total_bits;
}

/* Тест серий: считаем количество серий (подряд идущих одинаковых бит)
 * и среднюю длину серии. Для случайных данных:
 *   - средняя длина ≈ 2
 *   - число серий ≈ total_bits / 2
 * Возвращает среднюю длину серии. */
static double runs_test(const uint8_t *buf, size_t n, size_t *runs_out)
{
  size_t runs = 1; /* первая серия уже началась */
  size_t total_bits = n * 8;
  int prev_bit;
  int cur_bit;

  /* Первый бит */
  prev_bit = buf[0] & 1u;

  /* Идём по всем битам, начиная со второго */
  for (size_t i = 0; i < n; i++)
  {
    uint8_t b = buf[i];
    for (int bit = 0; bit < 8; bit++)
    {
      /* Пропускаем самый первый бит — он уже учтён в prev_bit */
      if (i == 0 && bit == 0)
        continue;
      cur_bit = (b >> bit) & 1u;
      if (cur_bit != prev_bit)
      {
        runs++;
        prev_bit = cur_bit;
      }
    }
  }
  *runs_out = runs;
  return (double)total_bits / (double)runs;
}

/* Среднее и дисперсия байтов.
 * Для равномерного распределения на 0..255:
 *   среднее ≈ 127.5
 *   дисперсия ≈ 5461.25
 * Возвращает среднее через *mean_out, дисперсию через *var_out. */
static void mean_var_test(const uint8_t *buf, size_t n,
                          double *mean_out, double *var_out)
{
  double sum = 0.0;
  double sum_sq = 0.0;

  for (size_t i = 0; i < n; i++)
  {
    double x = (double)buf[i];
    sum += x;
    sum_sq += x * x;
  }
  double mean = sum / (double)n;
  /* Дисперсия */
  double var = sum_sq / (double)n - mean * mean;

  *mean_out = mean;
  *var_out = var;
}

/* Прогоняет все три теста на буфере и печатает результаты.
 * label — название канала ("Аппаратный" / "Программный"). */
static void run_all_tests(const uint8_t *buf, size_t n, const char *label)
{
  double f = freq_test(buf, n);
  size_t runs;
  double avg_run = runs_test(buf, n, &runs);
  double mean, var;
  mean_var_test(buf, n, &mean, &var);

  printf("--- %s ---\n", label);
  printf("  Частотный: доля единиц = %.4f %s\n",
         f, (f > 0.45 && f < 0.55) ? "OK" : "ПОДОЗРИТЕЛЬНО");
  printf("  Серии:    серий = %zu, средняя = %.4f %s\n",
         runs, avg_run, (avg_run > 1.8 && avg_run < 2.2) ? "OK" : "ПОДОЗРИТЕЛЬНО");
  printf("  Среднее/дисперсия: %.2f / %.2f %s\n",
         mean, var,
         (mean > 120.0 && mean < 135.0 && var > 5000.0 && var < 5900.0)
             ? "OK"
             : "ПОДОЗРИТЕЛЬНО");
}

/* Замер времени чтения n байт. Возвращает секунды. */
static double timed_read(int fd, uint8_t *buf, size_t n)
{
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  if (read_bytes(fd, buf, n) != 0)
    return -1.0;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  return (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
}

int main(void)
{
  int fd = open(DEVICE, O_RDWR);
  if (fd < 0)
  {
    perror("open");
    return 1;
  }

  uint8_t *buf = malloc(NBYTES);
  if (!buf)
  {
    perror("malloc");
    close(fd);
    return 1;
  }

  /* ---------- Прогон A: аппаратный (RDRAND через таймер) ---------- */
  ioctl(fd, RNG_IOC_RESET, 0);

  double t_a = timed_read(fd, buf, NBYTES);
  if (t_a < 0)
  {
    free(buf);
    close(fd);
    return 1;
  }
  printf("Первые 32 байта (аппаратный): ");
  for (int i = 0; i < 32; i++)
    printf("%02x ", buf[i]);
  printf("\n");
  run_all_tests(buf, NBYTES, "Аппаратный (RDRAND + пул)");
  printf("  Скорость: %.0f байт/с\n", NBYTES / t_a);

  struct rng_stats st_a;
  ioctl(fd, RNG_IOC_STATS, &st_a);

  /* ---------- Прогон B: программный (write + пул) ---------- */
  if (ioctl(fd, RNG_IOC_RESET, 0) != 0)
  {
    perror("ioctl RESET");
    return 1;
  }
  struct rng_stats tmp;
  ioctl(fd, RNG_IOC_STATS, &tmp);
  /* Заполняем буфер «своими» данными и пишем в драйвер */
  for (size_t i = 0; i < NBYTES; i++)
    buf[i] = (uint8_t)(i * 37 + 11); /* детерминированный шаблон */
  if (write(fd, buf, NBYTES) != (ssize_t)NBYTES)
  {
    perror("write");
    free(buf);
    close(fd);
    return 1;
  }

  double t_b = timed_read(fd, buf, NBYTES);
  if (t_b < 0)
  {
    free(buf);
    close(fd);
    return 1;
  }
  printf("Первые 32 байта (программный): ");
  for (int i = 0; i < 32; i++)
    printf("%02x ", buf[i]);
  printf("\n");
  run_all_tests(buf, NBYTES, "Программный (write + пул)");
  printf("  Скорость: %.0f байт/с\n", NBYTES / t_b);

  struct rng_stats st_b;
  ioctl(fd, RNG_IOC_STATS, &st_b);

  printf("\n=== Сравнение ===\n");
  printf("Аппаратный:  from_hw=%llu total_bits=%llu mix_count=%llu\n",
         (unsigned long long)st_a.bytes_from_hw,
         (unsigned long long)st_a.total_bits,
         (unsigned long long)st_a.mix_count);
  printf("Программный: written=%llu total_bits=%llu mix_count=%llu\n",
         (unsigned long long)st_b.bytes_written,
         (unsigned long long)st_b.total_bits,
         (unsigned long long)st_b.mix_count);
  printf("Бит на событие: аппаратный %llu, программный %llu\n",
         (unsigned long long)(st_a.mix_count ? st_a.total_bits / st_a.mix_count : 0),
         (unsigned long long)(st_b.mix_count ? st_b.total_bits / st_b.mix_count : 0));
  printf("Скорость: аппаратный %.0f байт/с, программный %.0f байт/с\n",
         NBYTES / t_a, NBYTES / t_b);

  free(buf);
  close(fd);
  return 0;
}