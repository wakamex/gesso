#include "gs_jobs.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

typedef struct job {
    gs_job_fn *fn;
    void *user;
    struct job *next;
} job;

#define MAX_THREADS 16

struct gs_jobs {
    SDL_Mutex *lock;
    SDL_Condition *work, *done;
    job *head, *tail;
    int pending, nthreads;
    bool quit;
    SDL_Thread *threads[MAX_THREADS];
};

static int SDLCALL worker(void *arg) {
    gs_jobs *j = arg;
    SDL_LockMutex(j->lock);
    for (;;) {
        while (!j->head && !j->quit) SDL_WaitCondition(j->work, j->lock);
        if (j->quit) break;
        job *w = j->head;
        j->head = w->next;
        if (!j->head) j->tail = NULL;
        SDL_UnlockMutex(j->lock);
        w->fn(w->user);
        free(w);
        SDL_LockMutex(j->lock);
        if (--j->pending == 0) SDL_BroadcastCondition(j->done);
    }
    SDL_UnlockMutex(j->lock);
    return 0;
}

gs_jobs *gs_jobs_new(int threads) {
    gs_jobs *j = calloc(1, sizeof *j);
    j->lock = SDL_CreateMutex();
    j->work = SDL_CreateCondition();
    j->done = SDL_CreateCondition();
    if (threads <= 0) threads = SDL_GetNumLogicalCPUCores() - 1;
    if (threads < 1) threads = 1;
    if (threads > MAX_THREADS) threads = MAX_THREADS;
    for (int i = 0; i < threads; i++) {
        SDL_Thread *t = SDL_CreateThread(worker, "gs_jobs", j);
        if (!t) break;  // no threads here: jobs run inline
        j->threads[j->nthreads++] = t;
    }
    return j;
}

void gs_jobs_add(gs_jobs *j, gs_job_fn *fn, void *user) {
    if (!j->nthreads) {
        fn(user);
        return;
    }
    job *w = malloc(sizeof *w);
    *w = (job){ fn, user, NULL };
    SDL_LockMutex(j->lock);
    if (j->tail) j->tail->next = w; else j->head = w;
    j->tail = w;
    j->pending++;
    SDL_SignalCondition(j->work);
    SDL_UnlockMutex(j->lock);
}

int gs_jobs_pending(gs_jobs *j) {
    SDL_LockMutex(j->lock);
    int n = j->pending;
    SDL_UnlockMutex(j->lock);
    return n;
}

void gs_jobs_wait(gs_jobs *j) {
    SDL_LockMutex(j->lock);
    while (j->pending) SDL_WaitCondition(j->done, j->lock);
    SDL_UnlockMutex(j->lock);
}

void gs_jobs_free(gs_jobs *j) {
    if (!j) return;
    SDL_LockMutex(j->lock);
    j->quit = true;
    SDL_BroadcastCondition(j->work);
    SDL_UnlockMutex(j->lock);
    for (int i = 0; i < j->nthreads; i++) SDL_WaitThread(j->threads[i], NULL);
    while (j->head) {
        job *next = j->head->next;
        free(j->head);
        j->head = next;
    }
    SDL_DestroyCondition(j->work);
    SDL_DestroyCondition(j->done);
    SDL_DestroyMutex(j->lock);
    free(j);
}
