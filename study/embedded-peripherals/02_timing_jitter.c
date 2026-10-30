/*
 * 02_timing_jitter.c —— 周期任务抖动测量（控制环路的"心跳"质量）
 *
 * 对应你项目里的"测量端到端控制延迟和周期抖动"。
 *
 * 知识点：
 *   1. clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)：按"绝对时间"睡眠，
 *      不会因为每次唤醒晚一点而累积漂移
 *   2. 误差 = 实际唤醒时刻 - 目标时刻；抖动就是误差的分布（p99/max 比 avg 更重要）
 *   3. --spin 参数：睡前把最后 N 微秒改成忙等，能显著降低抖动（代价是占 CPU）
 *   4. 统计要排序后取分位数，不能只看平均值
 *
 * 用法：
 *   ./02_timing_jitter                 # 5ms 周期，2000 次
 *   ./02_timing_jitter 1000 5000       # 1ms 周期，5000 次
 *   ./02_timing_jitter 5000 2000 300   # 最后 300us 忙等
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <errno.h>
#include <math.h>

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cmp_s64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a;
	int64_t y = *(const int64_t *)b;

	return (x > y) - (x < y);
}

static void sleep_until(uint64_t target_ns, uint64_t spin_ns)
{
	struct timespec ts;

	if (spin_ns < (uint64_t)(target_ns - now_ns())) {
		uint64_t sleep_to = target_ns - spin_ns;

		ts.tv_sec = (time_t)(sleep_to / 1000000000ULL);
		ts.tv_nsec = (long)(sleep_to % 1000000000ULL);
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
			;
	}
	while (now_ns() < target_ns)
		;	/* 忙等最后 spin_ns */
}

int main(int argc, char *argv[])
{
	int64_t period_us = (argc > 1) ? atoll(argv[1]) : 5000;
	int count = (argc > 2) ? atoi(argv[2]) : 2000;
	int64_t spin_us = (argc > 3) ? atoll(argv[3]) : 0;
	int64_t *err = malloc((size_t)count * sizeof(*err));
	int64_t *interval = malloc((size_t)count * sizeof(*interval));
	uint64_t period_ns = (uint64_t)period_us * 1000ULL;
	uint64_t spin_ns = (uint64_t)spin_us * 1000ULL;
	uint64_t next, prev;
	int i;
	int buckets[6] = { 0 };
	int64_t sum = 0, sum_sq = 0;
	uint64_t busy_fail = 0;

	if (!err || !interval || period_us <= 0 || count <= 0) {
		fprintf(stderr, "参数错误\n");
		return 1;
	}

	/* 尽量提高优先级；失败也不影响测量（普通用户通常没权限） */
	{
		struct sched_param sp = { .sched_priority = 80 };

		if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
			fprintf(stderr, "提示: 设置 SCHED_FIFO 失败(%s)，以普通优先级运行\n",
				strerror(errno));
		else
			puts("已切换 SCHED_FIFO 实时调度");
	}

	next = now_ns() + period_ns;
	prev = now_ns();

	printf("周期 %ld us, 次数 %d, 忙等 %ld us\n", (long)period_us, count,
	       (long)spin_us);

	for (i = 0; i < count; i++) {
		int64_t e;
		uint64_t t;

		sleep_until(next, spin_ns);
		t = now_ns();
		e = (int64_t)(t - next);
		err[i] = e;
		interval[i] = (int64_t)(t - prev);
		prev = t;
		next += period_ns;

		sum += e;
		sum_sq += e * e;
		if (e < 50000)
			buckets[0]++;
		else if (e < 100000)
			buckets[1]++;
		else if (e < 200000)
			buckets[2]++;
		else if (e < 500000)
			buckets[3]++;
		else if (e < 1000000)
			buckets[4]++;
		else
			buckets[5]++;

		if (t > next + period_ns)
			busy_fail++;	/* 已经落后一个周期 */
	}

	qsort(err, (size_t)count, sizeof(*err), cmp_s64);
	qsort(interval, (size_t)count, sizeof(*interval), cmp_s64);

	printf("\n唤醒误差(us):  min=%ld  avg=%.2f  p50=%ld  p95=%ld  p99=%ld  max=%ld\n",
	       (long)(err[0] / 1000), (double)sum / count / 1000.0,
	       (long)(err[count * 50 / 100] / 1000),
	       (long)(err[count * 95 / 100] / 1000),
	       (long)(err[count * 99 / 100] / 1000),
	       (long)(err[count - 1] / 1000));
	printf("实际周期(us):  min=%ld  p50=%ld  p99=%ld  max=%ld\n",
	       (long)(interval[0] / 1000),
	       (long)(interval[count * 50 / 100] / 1000),
	       (long)(interval[count * 99 / 100] / 1000),
	       (long)(interval[count - 1] / 1000));

	{
		double avg = (double)sum / count;
		double var = (double)sum_sq / count - avg * avg;

		if (var < 0)
			var = 0;
		printf("误差标准差: %.2f us, 落后超过一周期: %llu 次\n",
		       sqrt(var) / 1000.0,
		       (unsigned long long)busy_fail);
	}

	printf("\n误差分布: <50us %d | <100us %d | <200us %d | <500us %d | <1ms %d | >=1ms %d\n",
	       buckets[0], buckets[1], buckets[2], buckets[3], buckets[4], buckets[5]);

	free(err);
	free(interval);
	return 0;
}
