/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "internal.h"
#include <stdlib.h>
#include <limits.h>  /* PTHREAD_STACK_MIN */
#include <unistd.h>  /* sysconf() */

struct threadpool_ctx {
  uv_cond_t cond;
  uv_mutex_t mutex;
  ngx_queue_t work_queue;
  ngx_queue_t thread_queue;
  unsigned int cur_threads;
  unsigned int max_threads;
};

struct thread_ctx {
  ngx_queue_t queue;
  pthread_t thread;
};

static uv_once_t once = UV_ONCE_INIT;
static volatile int initialized;
static ngx_queue_t exit_message;
static struct threadpool_ctx threadpools[UV__THREADPOOL_MAX];


static void uv__cancelled(struct uv__work* w) {
  abort();
}


/* To avoid deadlock with uv_cancel() it's crucial that the worker
 * never holds the global mutex and the loop-local mutex at the same time.
 */
static void* worker(void* arg) {
  struct threadpool_ctx* ctx;
  struct uv__work* w;
  ngx_queue_t* q;

  ctx = arg;

  for (;;) {
    uv_mutex_lock(&ctx->mutex);

    while (ngx_queue_empty(&ctx->work_queue))
      uv_cond_wait(&ctx->cond, &ctx->mutex);

    q = ngx_queue_head(&ctx->work_queue);

    if (q == &exit_message)
      uv_cond_signal(&ctx->cond);
    else {
      ngx_queue_remove(q);
      ngx_queue_init(q);  /* Signal uv_cancel() that the work req is
                             executing. */
    }

    uv_mutex_unlock(&ctx->mutex);

    if (q == &exit_message)
      break;

    w = ngx_queue_data(q, struct uv__work, wq);
    w->work(w);

    uv_mutex_lock(&w->loop->wq_mutex);
    w->work = NULL;  /* Signal uv_cancel() that the work req is done
                        executing. */
    ngx_queue_insert_tail(&w->loop->wq, &w->wq);
    uv_async_send(&w->loop->wq_async);
    uv_mutex_unlock(&w->loop->wq_mutex);
  }

  return NULL;
}


static void threadpool_init(struct threadpool_ctx* ctx,
                            unsigned int max_threads) {
  if (uv_cond_init(&ctx->cond))
    abort();

  if (uv_mutex_init(&ctx->mutex))
    abort();

  ngx_queue_init(&ctx->work_queue);
  ngx_queue_init(&ctx->thread_queue);

  ctx->cur_threads = 0;
  ctx->max_threads = max_threads;
}


static void threadpool_destroy(struct threadpool_ctx* ctx) {
  uv_mutex_lock(&ctx->mutex);
  uv_mutex_unlock(&ctx->mutex);
  uv_mutex_destroy(&ctx->mutex);
  uv_cond_destroy(&ctx->cond);
}


static void threadpool_grow(struct threadpool_ctx* ctx, size_t stack_size) {
  struct thread_ctx* tc;
  pthread_attr_t attr;
  int new_thread;

  /* Cheap but safe check: max_threads is immutable, cur_threads is a naturally
   * aligned integer and doesn't need locking to read. Worst case, we first see
   * that cur_threads < max_threads, acquire the lock, discover that another
   * thread has preempted us and that cur_threads == max_threads now.
   */
  if (ACCESS_ONCE(unsigned int, ctx->cur_threads) == ctx->max_threads)
    return;

  uv_mutex_lock(&ctx->mutex);

  new_thread = (ctx->cur_threads < ctx->max_threads);
  if (new_thread != 0)
    ctx->cur_threads++;

  uv_mutex_unlock(&ctx->mutex);

  if (new_thread == 0)
    return;

  tc = malloc(sizeof(*tc));
  if (tc == NULL) {
    uv_mutex_lock(&ctx->mutex);
    assert(ctx->cur_threads > 0);
    ctx->cur_threads--;
    uv_mutex_unlock(&ctx->mutex);
    return;
  }

  if (pthread_attr_init(&attr))
    abort();

  if (stack_size > 0)
    if (pthread_attr_setstacksize(&attr, stack_size))
      abort();

  /* XXX set scheduling policy as well? */

  if (pthread_create(&tc->thread, &attr, worker, ctx))
    abort();

  if (pthread_attr_destroy(&attr))
    abort();

  uv_mutex_lock(&ctx->mutex);
  ngx_queue_insert_tail(&ctx->thread_queue, &tc->queue);
  uv_mutex_unlock(&ctx->mutex);
}


static void post(struct threadpool_ctx* ctx, ngx_queue_t* q) {
  const size_t stack_sizes[UV__THREADPOOL_MAX] = { 262144, 32768 };
  unsigned int index;
  size_t stack_size;

  index = ctx - threadpools;
  assert(index < ARRAY_SIZE(stack_sizes));
  stack_size = stack_sizes[index];

  if (stack_size < PTHREAD_STACK_MIN)
    stack_size = PTHREAD_STACK_MIN;

  threadpool_grow(ctx, stack_size);
  uv_mutex_lock(&ctx->mutex);
  ngx_queue_insert_tail(&ctx->work_queue, q);
  uv_cond_signal(&ctx->cond);
  uv_mutex_unlock(&ctx->mutex);
}


static void init_once(void) {
  long numcpus;

  numcpus = sysconf(_SC_NPROCESSORS_ONLN);
  if (numcpus <= 0)
    numcpus = 1;

  threadpool_init(threadpools + UV__THREADPOOL_CPU, numcpus);
  threadpool_init(threadpools + UV__THREADPOOL_IO, numcpus * 16);
  initialized = 1;
}


/* This destructor is here mostly to please Valgrind. It is up for debate
 * if draining the thread pools after main() has returned is a good thing:
 * any global or shared state the work and done callbacks rely on, is most
 * likely gone by now. If it turns out to be a problem, we'll hide it behind
 * a Valgrind-only #define.
 */
#if defined(__GNUC__)
__attribute__((destructor))
static void cleanup(void) {
  struct threadpool_ctx* ctx;
  struct thread_ctx* tc;
  ngx_queue_t* q;
  unsigned int i;
  int empty;

  if (initialized == 0)
    return;

  for (i = 0; i < ARRAY_SIZE(threadpools); i++)
    post(threadpools + i, &exit_message);

  for (i = 0; i < ARRAY_SIZE(threadpools); i++) {
    ctx = threadpools + i;

    for (;;) {
      uv_mutex_lock(&ctx->mutex);

      empty = ngx_queue_empty(&ctx->thread_queue);
      if (empty == 0) {
        q = ngx_queue_head(&ctx->thread_queue);
        ngx_queue_remove(q);
      }

      uv_mutex_unlock(&ctx->mutex);

      if (empty != 0)
        break;

      tc = ngx_queue_data(q, struct thread_ctx, queue);

      if (pthread_join(tc->thread, NULL))
        abort();

      free(tc);
    }

    threadpool_destroy(ctx);
  }

  initialized = 0;
}
#endif


void uv__work_submit(uv_loop_t* loop,
                     struct uv__work* w,
                     void (*work)(struct uv__work* w),
                     void (*done)(struct uv__work* w, int status),
                     unsigned int type) {
  assert(type < ARRAY_SIZE(threadpools));
  uv_once(&once, init_once);
  w->loop = loop;
  w->work = work;
  w->done = done;
  post(threadpools + type, &w->wq);
}


static int uv__work_cancel(uv_loop_t* loop,
                           uv_req_t* req,
                           struct uv__work* w,
                           unsigned int type) {
  struct threadpool_ctx* ctx;
  int cancelled;

  assert(type < ARRAY_SIZE(threadpools));

  ctx = threadpools + type;
  uv_mutex_lock(&ctx->mutex);
  uv_mutex_lock(&w->loop->wq_mutex);

  cancelled = !ngx_queue_empty(&w->wq) && w->work != NULL;
  if (cancelled)
    ngx_queue_remove(&w->wq);

  uv_mutex_unlock(&w->loop->wq_mutex);
  uv_mutex_unlock(&ctx->mutex);

  if (!cancelled)
    return -1;

  w->work = uv__cancelled;
  uv_mutex_lock(&loop->wq_mutex);
  ngx_queue_insert_tail(&loop->wq, &w->wq);
  uv_async_send(&loop->wq_async);
  uv_mutex_unlock(&loop->wq_mutex);

  return 0;
}


void uv__work_done(uv_async_t* handle, int status) {
  struct uv__work* w;
  uv_loop_t* loop;
  ngx_queue_t* q;
  ngx_queue_t wq;
  int err;

  loop = container_of(handle, uv_loop_t, wq_async);
  ngx_queue_init(&wq);

  uv_mutex_lock(&loop->wq_mutex);
  if (!ngx_queue_empty(&loop->wq)) {
    q = ngx_queue_head(&loop->wq);
    ngx_queue_split(&loop->wq, q, &wq);
  }
  uv_mutex_unlock(&loop->wq_mutex);

  while (!ngx_queue_empty(&wq)) {
    q = ngx_queue_head(&wq);
    ngx_queue_remove(q);

    w = container_of(q, struct uv__work, wq);
    err = (w->work == uv__cancelled) ? -UV_ECANCELED : 0;
    w->done(w, err);
  }
}


static void uv__queue_work(struct uv__work* w) {
  uv_work_t* req = container_of(w, uv_work_t, work_req);

  req->work_cb(req);
}


static void uv__queue_done(struct uv__work* w, int status) {
  uv_work_t* req;

  req = container_of(w, uv_work_t, work_req);
  uv__req_unregister(req->loop, req);

  if (req->after_work_cb == NULL)
    return;

  if (status == -UV_ECANCELED)
    uv__set_artificial_error(req->loop, UV_ECANCELED);

  req->after_work_cb(req, status ? -1 : 0);
}


int uv_queue_work(uv_loop_t* loop,
                  uv_work_t* req,
                  uv_work_cb work_cb,
                  uv_after_work_cb after_work_cb) {
  if (work_cb == NULL)
    return uv__set_artificial_error(loop, UV_EINVAL);

  uv__req_init(loop, req, UV_WORK);
  req->loop = loop;
  req->work_cb = work_cb;
  req->after_work_cb = after_work_cb;
  uv__work_submit(loop,
                  &req->work_req,
                  uv__queue_work,
                  uv__queue_done,
                  UV__THREADPOOL_CPU);
  return 0;
}


int uv_cancel(uv_req_t* req) {
  struct uv__work* wreq;
  unsigned int type;
  uv_loop_t* loop;

  switch (req->type) {
  case UV_FS:
    loop =  ((uv_fs_t*) req)->loop;
    wreq = &((uv_fs_t*) req)->work_req;
    type = UV__THREADPOOL_IO;
    break;
  case UV_GETADDRINFO:
    loop =  ((uv_getaddrinfo_t*) req)->loop;
    wreq = &((uv_getaddrinfo_t*) req)->work_req;
    type = UV__THREADPOOL_IO;
    break;
  case UV_WORK:
    loop =  ((uv_work_t*) req)->loop;
    wreq = &((uv_work_t*) req)->work_req;
    type = UV__THREADPOOL_CPU;
    break;
  default:
    return -1;
  }

  return uv__work_cancel(loop, req, wreq, type);
}
