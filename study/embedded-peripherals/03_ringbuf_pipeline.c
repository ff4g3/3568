/*
 * 03_ringbuf_pipeline.c —— 采集线程 -> 缓冲池 -> 处理线程（流水线骨架）
 *
 * 对应你的视觉流水线：V4L2 采集线程把帧放进缓冲池，编码/推理线程取走处理。
 * 这里用"空闲池 + 就绪队列"而不是简单的环形数组，原因是：
 *   消费者处理一帧时不能一直占着锁，也不能让生产者覆盖正在处理的帧。
 *
 * 知识点：
 *   1. pthread_mutex + pthread_cond 实现生产者/消费者
 *   2. 固定缓冲池复用内存，避免每帧 malloc/free（这是零拷贝的第一步）
 *   3. 队列满时"丢弃最新帧"是一种背压策略（实时系统更怕越积越多）
 *   4. 每帧带 CLOCK_MONOTONIC 时间戳，统计端到端延迟分布
 *
 * 用法：
 *   ./03_ringbuf_pipeline            # 5 秒，30fps，处理耗时 5ms
 *   ./03_ringbuf_pipeline 10 60 2    # 10 秒，60fps，处理 2ms
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#define POOL_SLOTS	4
#define FRAME_BYTES	(640 * 480 * 3 / 2)	/* 模拟 640x480 NV12 */

struct frame {
	uint64_t seq;
	uint64_t t_gen_ns;
	size_t len;
	uint8_t data[FRAME_BYTES];
};

struct pipeline {
	struct frame frames[POOL_SLOTS];
	int free_stack[POOL_SLOTS];
	int free_top;
	int ready_q[POOL_SLOTS];
	int rq_head, rq_tail, rq_count;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int stop;
	uint64_t produced;
	uint64_t consumed;
	uint64_t dropped;
};

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

static int pool_take_free(struct pipeline *p)
{
	int idx;

	pthread_mutex_lock(&p->lock);
	if (p->free_top == 0) {
		p->dropped++;
		pthread_mutex_unlock(&p->lock);
		return -1;
	}
	idx = p->free_stack[--p->free_top];
	pthread_mutex_unlock(&p->lock);
	return idx;
}

static void pool_push_ready(struct pipeline *p, int idx)
{
	pthread_mutex_lock(&p->lock);
	p->ready_q[p->rq_tail] = idx;
	p->rq_tail = (p->rq_tail + 1) % POOL_SLOTS;
	p->rq_count++;
	p->produced++;
	pthread_cond_signal(&p->cond);
	pthread_mutex_unlock(&p->lock);
}

static int pool_pop_ready(struct pipeline *p)
{
	int idx = -1;

	pthread_mutex_lock(&p->lock);
	while (p->rq_count == 0 && !p->stop)
		pthread_cond_wait(&p->cond, &p->lock);
	if (p->rq_count > 0) {
		idx = p->ready_q[p->rq_head];
		p->rq_head = (p->rq_head + 1) % POOL_SLOTS;
		p->rq_count--;
	}
	pthread_mutex_unlock(&p->lock);
	return idx;
}

static void pool_return(struct pipeline *p, int idx)
{
	pthread_mutex_lock(&p->lock);
	p->free_stack[p->free_top++] = idx;
	p->consumed++;
	pthread_mutex_unlock(&p->lock);
}

struct worker_arg {
	struct pipeline *p;
	int fps;
	int seconds;
	int process_ms;
	uint64_t *lat;
	size_t lat_cap;
	size_t lat_n;
};

static volatile uint64_t g_sink;

static void *producer_thread(void *arg)
{
	struct worker_arg *wa = arg;
	struct pipeline *p = wa->p;
	uint64_t period_ns = 1000000000ULL / (uint64_t)wa->fps;
	uint64_t next = now_ns() + period_ns;
	uint64_t end = now_ns() + (uint64_t)wa->seconds * 1000000000ULL;
	uint64_t seq = 0;
	struct timespec ts;

	while (now_ns() < end) {
		int idx = pool_take_free(p);

		if (idx >= 0) {
			struct frame *f = &p->frames[idx];

			f->seq = seq;
			f->len = FRAME_BYTES;
			f->t_gen_ns = now_ns();
			/* 模拟 V4L2 拷贝/填充一帧 */
			memset(f->data, (int)(seq & 0xff), f->len);
			pool_push_ready(p, idx);
		}
		seq++;

		ts.tv_sec = (time_t)(next / 1000000000ULL);
		ts.tv_nsec = (long)(next % 1000000000ULL);
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
			;
		next += period_ns;
	}

	pthread_mutex_lock(&p->lock);
	p->stop = 1;
	pthread_cond_broadcast(&p->cond);
	pthread_mutex_unlock(&p->lock);
	return NULL;
}

static void *consumer_thread(void *arg)
{
	struct worker_arg *wa = arg;
	struct pipeline *p = wa->p;

	for (;;) {
		struct frame *f;
		uint64_t t_done;
		uint8_t sum = 0;
		size_t i;
		int idx = pool_pop_ready(p);

		if (idx < 0)
			break;	/* stop 且队列已空 */

		f = &p->frames[idx];

		/* 模拟编码/推理：读一遍数据 + 固定耗时 */
		for (i = 0; i < f->len; i += 64)
			sum ^= f->data[i];
		g_sink += sum;
		usleep((useconds_t)wa->process_ms * 1000);

		t_done = now_ns();
		if (wa->lat_n < wa->lat_cap)
			wa->lat[wa->lat_n++] = t_done - f->t_gen_ns;

		pool_return(p, idx);
	}
	return NULL;
}

int main(int argc, char *argv[])
{
	static struct pipeline p;
	struct worker_arg wa;
	pthread_t prod, cons;
	int fps = (argc > 2) ? atoi(argv[2]) : 30;
	int seconds = (argc > 1) ? atoi(argv[1]) : 5;
	int process_ms = (argc > 3) ? atoi(argv[3]) : 5;
	size_t cap;
	uint64_t sum = 0;
	int i;

	if (fps <= 0 || seconds <= 0 || process_ms < 0) {
		fprintf(stderr, "用法: %s [秒数] [fps] [处理耗时ms]\n", argv[0]);
		return 1;
	}

	cap = (size_t)seconds * (size_t)fps + 1024;
	memset(&p, 0, sizeof(p));
	for (i = 0; i < POOL_SLOTS; i++)
		p.free_stack[i] = i;
	p.free_top = POOL_SLOTS;

	pthread_mutex_init(&p.lock, NULL);
	pthread_cond_init(&p.cond, NULL);

	wa.p = &p;
	wa.fps = fps;
	wa.seconds = seconds;
	wa.process_ms = process_ms;
	wa.lat = calloc(cap, sizeof(*wa.lat));
	wa.lat_cap = cap;
	wa.lat_n = 0;
	if (!wa.lat)
		return 1;

	printf("运行 %d 秒: %d fps, 处理耗时 %d ms, 缓冲池 %d 帧 (每帧 %d 字节)\n",
	       seconds, fps, process_ms, POOL_SLOTS, FRAME_BYTES);

	pthread_create(&prod, NULL, producer_thread, &wa);
	pthread_create(&cons, NULL, consumer_thread, &wa);
	pthread_join(prod, NULL);
	pthread_join(cons, NULL);

	printf("\n生成 %llu, 消费 %llu, 丢弃 %llu, 实测吞吐 %.1f fps\n",
	       (unsigned long long)p.produced, (unsigned long long)p.consumed,
	       (unsigned long long)p.dropped,
	       (double)p.consumed / seconds);

	if (wa.lat_n > 0) {
		qsort(wa.lat, wa.lat_n, sizeof(*wa.lat), cmp_u64);
		for (i = 0; i < (int)wa.lat_n; i++)
			sum += wa.lat[i];
		printf("端到端延迟(us): min=%llu avg=%llu p50=%llu p95=%llu p99=%llu max=%llu\n",
		       (unsigned long long)(wa.lat[0] / 1000),
		       (unsigned long long)(sum / wa.lat_n / 1000),
		       (unsigned long long)(wa.lat[wa.lat_n * 50 / 100] / 1000),
		       (unsigned long long)(wa.lat[wa.lat_n * 95 / 100] / 1000),
		       (unsigned long long)(wa.lat[wa.lat_n * 99 / 100] / 1000),
		       (unsigned long long)(wa.lat[wa.lat_n - 1] / 1000));
	}

	free(wa.lat);
	pthread_cond_destroy(&p.cond);
	pthread_mutex_destroy(&p.lock);
	return 0;
}
