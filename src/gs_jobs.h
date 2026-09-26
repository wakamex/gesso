// A small pool of worker threads for background work (rendering instruments, decoding, loading).
// Jobs run in the order added. Where threads are unavailable (the web without pthreads), jobs run
// at once on the thread that adds them.
#pragma once
#include <stdbool.h>

typedef struct gs_jobs gs_jobs;
typedef void gs_job_fn(void *user);

gs_jobs *gs_jobs_new(int threads);  // 0: one fewer than the CPU's cores, at least 1
void gs_jobs_add(gs_jobs *j, gs_job_fn *fn, void *user);
int gs_jobs_pending(gs_jobs *j);    // queued and running
void gs_jobs_wait(gs_jobs *j);      // until none are pending
void gs_jobs_free(gs_jobs *j);      // finishes running jobs, drops queued ones
