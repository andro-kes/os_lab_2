// median_filter_pool.c
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <stdatomic.h>

// Очередь задач
typedef struct {
    int *buf;
    int capacity; 
    int head, tail, size;
    pthread_mutex_t mutex;
    pthread_cond_t cond_nonempty;
    int shutdown;  
} TaskQueue;

/* Global shared state visible to workers */
static int *g_in = NULL;
static int *g_out = NULL;
static int g_R = 0;
static int g_C = 0;
static int g_win = 3;

/* For measuring concurrency */
static atomic_int g_active = 0;
static atomic_int g_max_active = 0;

/* For signalling main that current iteration completed */
static int completed = 0;
static pthread_mutex_t completed_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t completed_cond = PTHREAD_COND_INITIALIZER;

/* Task queue helpers */
static int tq_init(TaskQueue *q, int capacity) {
    q->buf = malloc(sizeof(int) * capacity);
    if (!q->buf) return -1;
    q->capacity = capacity;
    q->head = q->tail = q->size = 0;
    q->shutdown = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond_nonempty, NULL);
    return 0;
}
static void tq_destroy(TaskQueue *q) {
    if (!q) return;
    free(q->buf);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond_nonempty);
}
static void tq_push(TaskQueue *q, int row) {
    pthread_mutex_lock(&q->mutex);
    // assume capacity >= R, so shouldn't be full in normal use
    q->buf[q->tail] = row;
    q->tail = (q->tail + 1) % q->capacity;
    q->size++;
    pthread_cond_signal(&q->cond_nonempty);
    pthread_mutex_unlock(&q->mutex);
}
static int tq_pop(TaskQueue *q, int *out_row) {
    pthread_mutex_lock(&q->mutex);
    while (q->size == 0 && !q->shutdown) {
        pthread_cond_wait(&q->cond_nonempty, &q->mutex);
    }
    if (q->size == 0 && q->shutdown) {
        pthread_mutex_unlock(&q->mutex);
        return 0; // no task, shutdown
    }
    *out_row = q->buf[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->size--;
    pthread_mutex_unlock(&q->mutex);
    return 1;
}
static void tq_shutdown(TaskQueue *q) {
    pthread_mutex_lock(&q->mutex);
    q->shutdown = 1;
    pthread_cond_broadcast(&q->cond_nonempty);
    pthread_mutex_unlock(&q->mutex);
}

/* compare for qsort */
static int cmp_int(const void *a, const void *b) {
    int ia = *(const int*)a;
    int ib = *(const int*)b;
    return (ia > ib) - (ia < ib);
}

/* worker thread */
typedef struct {
    TaskQueue *queue;
} WorkerArg;

static void *worker_main(void *vp) {
    WorkerArg *warg = (WorkerArg*)vp;
    TaskQueue *q = warg->queue;

    // allocate window buffer once per worker (reuse between rows)
    int wsize = g_win * g_win;
    int *vals = malloc(sizeof(int) * wsize);
    if (!vals) {
        fprintf(stderr, "Worker: malloc failed\n");
        return NULL;
    }

    while (1) {
        int row;
        if (!tq_pop(q, &row)) break; // shutdown and no more tasks

        // mark active
        int cur = atomic_fetch_add(&g_active, 1) + 1;
        // update observed max
        int prev;
        do {
            prev = atomic_load(&g_max_active);
            if (cur <= prev) break;
        } while (!atomic_compare_exchange_weak(&g_max_active, &prev, cur));

        // process row `row` reading from g_in and writing to g_out
        int half = g_win / 2;
        for (int col = 0; col < g_C; ++col) {
            int idx = 0;
            for (int dr = -half; dr <= half; ++dr) {
                int rr = row + dr;
                if (rr < 0) rr = 0;
                if (rr >= g_R) rr = g_R - 1;
                for (int dc = -half; dc <= half; ++dc) {
                    int cc = col + dc;
                    if (cc < 0) cc = 0;
                    if (cc >= g_C) cc = g_C - 1;
                    vals[idx++] = g_in[rr * g_C + cc];
                }
            }
            qsort(vals, (size_t)idx, sizeof(int), cmp_int);
            int median = vals[idx/2];
            g_out[row * g_C + col] = median;
        }

        // done with one row: decrement active
        atomic_fetch_sub(&g_active, 1);

        // notify completion (main thread waits on completed == g_R)
        pthread_mutex_lock(&completed_mutex);
        completed++;
        if (completed == g_R) {
            pthread_cond_signal(&completed_cond);
        }
        pthread_mutex_unlock(&completed_mutex);
    }

    free(vals);
    return NULL;
}

/* print usage */
static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [-m max_threads] -k K -w window_size [-i infile] [-o outfile]\n"
        "  -m max_threads   maximum concurrent worker threads (default: number of CPU cores)\n"
        "  -k K             number of times to apply median filter (>=1)\n"
        "  -w window_size   window size (odd positive integer, e.g. 3,5,7)\n"
        "  -i infile        input file (default: stdin)\n"
        "  -o outfile       output file (default: stdout)\n",
        prog);
}

int main(int argc, char **argv) {
    int opt;
    int max_threads = 0;
    int K = -1;
    int win = -1;
    char *infile = NULL, *outfile = NULL;

    while ((opt = getopt(argc, argv, "m:k:w:i:o:")) != -1) {
        switch (opt) {
            case 'm': max_threads = atoi(optarg); break;
            case 'k': K = atoi(optarg); break;
            case 'w': win = atoi(optarg); break;
            case 'i': infile = optarg; break;
            case 'o': outfile = optarg; break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (K < 1 || win < 1 || (win % 2 == 0)) {
        fprintf(stderr, "Error: K must be >=1, window_size must be odd positive integer.\n");
        print_usage(argv[0]);
        return 1;
    }
    g_win = win;

    if (max_threads <= 0) {
        long procs = sysconf(_SC_NPROCESSORS_ONLN);
        max_threads = (procs > 0) ? (int)procs : 4;
    }
    if (max_threads < 1) max_threads = 1;

    FILE *fin = stdin;
    FILE *fout = stdout;
    if (infile) {
        fin = fopen(infile, "r");
        if (!fin) { perror("fopen infile"); return 1; }
    }
    if (outfile) {
        fout = fopen(outfile, "w");
        if (!fout) { perror("fopen outfile"); if (infile) fclose(fin); return 1; }
    }

    if (fscanf(fin, "%d %d", &g_R, &g_C) != 2) {
        fprintf(stderr, "Failed to read R C from input\n");
        if (infile) fclose(fin);
        if (outfile) fclose(fout);
        return 1;
    }
    if (g_R <= 0 || g_C <= 0) {
        fprintf(stderr, "Invalid matrix size\n");
        if (infile) fclose(fin);
        if (outfile) fclose(fout);
        return 1;
    }

    size_t total = (size_t)g_R * (size_t)g_C;
    int *buf1 = malloc(sizeof(int) * total);
    int *buf2 = malloc(sizeof(int) * total);
    if (!buf1 || !buf2) {
        fprintf(stderr, "Memory allocation failed\n");
        free(buf1); free(buf2);
        if (infile) fclose(fin);
        if (outfile) fclose(fout);
        return 1;
    }

    for (size_t i = 0; i < total; ++i) {
        if (fscanf(fin, "%d", &buf1[i]) != 1) {
            fprintf(stderr, "Not enough matrix elements in input\n");
            free(buf1); free(buf2);
            if (infile) fclose(fin);
            if (outfile) fclose(fout);
            return 1;
        }
    }
    if (infile) fclose(fin);

    // prepare task queue and workers
    TaskQueue queue;
    if (tq_init(&queue, g_R) != 0) {
        fprintf(stderr, "Failed to init task queue\n");
        free(buf1); free(buf2);
        return 1;
    }

    pthread_t *workers = malloc(sizeof(pthread_t) * max_threads);
    WorkerArg *wargs = malloc(sizeof(WorkerArg) * max_threads);
    if (!workers || !wargs) {
        fprintf(stderr, "Allocation failed\n");
        free(buf1); free(buf2); free(workers); free(wargs);
        tq_destroy(&queue);
        return 1;
    }

    // spawn worker threads
    for (int i = 0; i < max_threads; ++i) {
        wargs[i].queue = &queue;
        int rc = pthread_create(&workers[i], NULL, worker_main, &wargs[i]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create failed for worker %d: %s\n", i, strerror(rc));
            // we'll still try to continue with fewer workers
            workers[i] = 0;
        }
    }

    // perform K iterations: for each iteration populate queue with row tasks and wait until all done
    int *cur = buf1;
    int *next = buf2;

    for (int iter = 0; iter < K; ++iter) {
        // set global buffer pointers for workers to use in this iteration
        g_in = cur;
        g_out = next;

        // reset completed counter
        pthread_mutex_lock(&completed_mutex);
        completed = 0;
        pthread_mutex_unlock(&completed_mutex);

        // push tasks (rows)
        for (int r = 0; r < g_R; ++r) tq_push(&queue, r);

        // wait until all rows processed
        pthread_mutex_lock(&completed_mutex);
        while (completed < g_R) {
            pthread_cond_wait(&completed_cond, &completed_mutex);
        }
        pthread_mutex_unlock(&completed_mutex);

        // swap buffers
        int *tmp = cur; cur = next; next = tmp;
    }

    // shutdown workers
    tq_shutdown(&queue);
    for (int i = 0; i < max_threads; ++i) {
        if (workers[i]) pthread_join(workers[i], NULL);
    }

    // print concurrency info
    fprintf(stderr, "Configured worker threads: %d\n", max_threads);
    fprintf(stderr, "Observed max simultaneous active workers: %d\n", atomic_load(&g_max_active));

    // write output (cur points to final buffer after last swap)
    fprintf(fout, "%d %d\n", g_R, g_C);
    for (int i = 0; i < g_R; ++i) {
        for (int j = 0; j < g_C; ++j) {
            fprintf(fout, "%d%c", cur[i*g_C + j], (j+1==g_C) ? '\n' : ' ');
        }
    }

    if (outfile) fclose(fout);

    // cleanup
    free(buf1); free(buf2);
    free(workers); free(wargs);
    tq_destroy(&queue);
    pthread_mutex_destroy(&completed_mutex);
    pthread_cond_destroy(&completed_cond);

    return 0;
}