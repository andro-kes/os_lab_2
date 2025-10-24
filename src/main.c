// median filter
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

// Очередь задач
typedef struct {
    int *buf;           // Буфер для хранения задач (номеров строк)
    int capacity;       // Максимальная вместимость очереди
    int head, tail, size; // Указатели начала/конца и текущий размер очереди
    pthread_mutex_t mutex; // Мьютекс для синхронизации доступа
    pthread_cond_t cond_nonempty; // Условная переменная для уведомления о новых задачах
    int shutdown;       // Флаг завершения работы очереди
} TaskQueue;

/* Глобальные переменные */
static int *in = NULL;    // Входной буфер матрицы
static int *out = NULL;   // Выходной буфер матрицы
static int numberOfRows = 0;         // Количество строк матрицы
static int numberOfColumns = 0;         // Количество столбцов матрицы
static int sizeWindow = 3;       // Размер окна фильтра (по умолчанию 3x3) 

// Атомарные переменные для отслеживания активности потоков
static atomic_int currentNumberOfActive = 0;      // Текущее количество активных потоков
static atomic_int maxActive = 0;  // Максимальное количество одновременно активных потоков

// Переменные для синхронизации завершения итераций
static int completed = 0;            // Счетчик завершенных строк
static pthread_mutex_t completed_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t completed_cond = PTHREAD_COND_INITIALIZER;

// Инициализация очереди задач
static int tq_init(TaskQueue *q, int capacity) {
    q->buf = malloc(sizeof(int) * capacity); // Выделение памяти под буфер
    if (!q->buf) return -1;                  // Проверка на ошибку
    q->capacity = capacity;
    q->head = q->tail = q->size = 0;
    q->shutdown = 0;
    pthread_mutex_init(&q->mutex, NULL);     // Инициализация мьютекса
    pthread_cond_init(&q->cond_nonempty, NULL); // Инициализация условной переменной
    return 0;
}

// Освобождение ресурсов очереди
static void tq_destroy(TaskQueue *q) {
    if (!q) return;
    free(q->buf);                            // Освобождение памяти
    pthread_mutex_destroy(&q->mutex);        // Деструкция мьютекса
    pthread_cond_destroy(&q->cond_nonempty); // Деструкция условной переменной
}

// Добавление в очередь задач
static void tq_push(TaskQueue *q, int row) {
    pthread_mutex_lock(&q->mutex);           // Блокировка мьютекса
    q->buf[q->tail] = row;                   // Добавление номера строки
    q->tail = (q->tail + 1) % q->capacity;   // Циклический сдвиг указателя
    q->size++;                               // Увеличение размера очереди
    pthread_cond_signal(&q->cond_nonempty);  // Сигнал о наличии задачи
    pthread_mutex_unlock(&q->mutex);         // Разблокировка мьютекса
}

// Извлечение задачи из очереди
static int tq_pop(TaskQueue *q, int *out_row) {
    pthread_mutex_lock(&q->mutex);           // Блокировка мьютекса
    while (q->size == 0 && !q->shutdown) {   // Ожидание, пока очередь не станет непустой
        pthread_cond_wait(&q->cond_nonempty, &q->mutex);
    }
    if (q->size == 0 && q->shutdown) {       // Проверка на завершение
        pthread_mutex_unlock(&q->mutex);
        return 0; // Нет задач, завершение
    }
    *out_row = q->buf[q->head];              // Извлечение номера строки
    q->head = (q->head + 1) % q->capacity;   // Циклический сдвиг указателя
    q->size--;                               // Уменьшение размера очереди
    pthread_mutex_unlock(&q->mutex);         // Разблокировка мьютекса
    return 1;
}

// Завершение работы очереди
static void tq_shutdown(TaskQueue *q) {
    pthread_mutex_lock(&q->mutex);
    q->shutdown = 1;                           // Установка флага завершения
    pthread_cond_broadcast(&q->cond_nonempty); // Сигнал всем потокам
    pthread_mutex_unlock(&q->mutex);
}

// Компаратор для сортировки значений
static int cmp_int(const void *a, const void *b) {
    int ia = *(const int*)a;
    int ib = *(const int*)b;
    return (ia > ib) - (ia < ib); // Сравнение для qsort
}

// Структура аргументов для потока
typedef struct {
    TaskQueue *queue; // Указатель на очередь задач
} WorkerArg;

// Основная функция потока
static void *worker_main(void *vp) {
    WorkerArg *warg = (WorkerArg*)vp;
    TaskQueue *q = warg->queue;

    // Выделение памяти под временное хранение значений окна
    int wsize = sizeWindow * sizeWindow;
    int *vals = malloc(sizeof(int) * wsize);
    if (!vals) {
        fprintf(stderr, "Worker: malloc failed\n");
        return NULL;
    }

    while (1) {
        int row;
        if (!tq_pop(q, &row)) break; // Завершение, если нет задач

        // Отслеживание активных потоков
        int cur = atomic_fetch_add(&currentNumberOfActive, 1) + 1;
        int prev;
        do {
            prev = atomic_load(&maxActive);
            if (cur <= prev) break;
        } while (!atomic_compare_exchange_weak(&maxActive, &prev, cur));

        // Обработка строки `row`
        int half = sizeWindow / 2;
        for (int col = 0; col < numberOfColumns; ++col) {
            int idx = 0;
            for (int dr = -half; dr <= half; ++dr) {
                int rr = row + dr;
                if (rr < 0) rr = 0;
                if (rr >= numberOfRows) rr = numberOfRows - 1;
                for (int dc = -half; dc <= half; ++dc) {
                    int cc = col + dc;
                    if (cc < 0) cc = 0;
                    if (cc >= numberOfColumns) cc = numberOfColumns - 1;
                    vals[idx++] = in[rr * numberOfColumns + cc]; // Сбор значений окна
                }
            }
            qsort(vals, (size_t)idx, sizeof(int), cmp_int); // Сортировка
            int median = vals[idx/2]; // Нахождение медианы
            out[row * numberOfColumns + col] = median; // Запись результата
        }

        // Уменьшение счетчика активных потоков
        atomic_fetch_sub(&currentNumberOfActive, 1);

        // Уведомление основного потока о завершении строки
        pthread_mutex_lock(&completed_mutex);
        completed++;
        if (completed == numberOfRows) {
            pthread_cond_signal(&completed_cond);
        }
        pthread_mutex_unlock(&completed_mutex);
    }

    free(vals); // Освобождение памяти
    return NULL;
}

// Вывод помощи при использовании
static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [-m max_threads] -k K -w window_size [-i infile] [-o outfile]\n"
        "  -m max_threads   максимальное количество потоков (по умолчанию: количество ядер)\n"
        "  -k K             количество применений фильтра (>=1)\n"
        "  -w window_size   размер окна (нечетное число, например 3,5,7)\n"
        "  -i infile        входной файл (по умолчанию: stdin)\n"
        "  -o outfile       выходной файл (по умолчанию: stdout)\n",
        prog);
}

// Основная функция программы
int main(int argc, char **argv) {
    int opt;
    int max_threads = 0; // Максимальное количество потоков
    int K = -1;          // Количество итераций фильтра
    int win = -1;        // Размер окна
    char *infile = NULL, *outfile = NULL;

    // Парсинг аргументов командной строки
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

    // Проверка корректности аргументов
    if (K < 1 || win < 1 || (win % 2 == 0)) {
        fprintf(stderr, "Ошибка: K должно быть >=1, window_size должно быть нечетным.\n");
        print_usage(argv[0]);
        return 1;
    }
    sizeWindow = win;

    // Определение количества потоков по умолчанию
    if (max_threads <= 0) {
        long procs = sysconf(_SC_NPROCESSORS_ONLN);
        max_threads = (procs > 0) ? (int)procs : 4;
    }
    if (max_threads < 1) max_threads = 1;

    // Открытие входного и выходного файлов
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

    // Чтение размеров матрицы
    if (fscanf(fin, "%d %d", &numberOfRows, &numberOfColumns) != 2) {
        fprintf(stderr, "Не удалось прочитать R C из входного файла\n");
        if (infile) fclose(fin);
        if (outfile) fclose(fout);
        return 1;
    }
    if (numberOfRows <= 0 || numberOfColumns <= 0) {
        fprintf(stderr, "Неверный размер матрицы\n");
        if (infile) fclose(fin);
        if (outfile) fclose(fout);
        return 1;
    }

    // Выделение памяти под буферы
    size_t total = (size_t)numberOfRows * (size_t)numberOfColumns;
    int *buf1 = malloc(sizeof(int) * total);
    int *buf2 = malloc(sizeof(int) * total);
    if (!buf1 || !buf2) {
        fprintf(stderr, "Ошибка выделения памяти\n");
        free(buf1); free(buf2);
        if (infile) fclose(fin);
        if (outfile) fclose(fout);
        return 1;
    }

    // Чтение матрицы из файла
    for (size_t i = 0; i < total; ++i) {
        if (fscanf(fin, "%d", &buf1[i]) != 1) {
            fprintf(stderr, "Недостаточно элементов матрицы в входном файле\n");
            free(buf1); free(buf2);
            if (infile) fclose(fin);
            if (outfile) fclose(fout);
            return 1;
        }
    }
    if (infile) fclose(fin);

    // Инициализация очереди задач
    TaskQueue queue;
    if (tq_init(&queue, numberOfRows) != 0) {
        fprintf(stderr, "Ошибка инициализации очереди задач\n");
        free(buf1); free(buf2);
        return 1;
    }

    // Создание массивов для потоков
    pthread_t *workers = malloc(sizeof(pthread_t) * max_threads);
    WorkerArg *wargs = malloc(sizeof(WorkerArg) * max_threads);
    if (!workers || !wargs) {
        fprintf(stderr, "Ошибка выделения памяти для потоков\n");
        free(buf1); free(buf2); free(workers); free(wargs);
        tq_destroy(&queue);
        return 1;
    }

    // Создание потоков
    for (int i = 0; i < max_threads; ++i) {
        wargs[i].queue = &queue;
        int rc = pthread_create(&workers[i], NULL, worker_main, &wargs[i]);
        if (rc != 0) {
            fprintf(stderr, "Ошибка создания потока %d: %s\n", i, strerror(rc));
            workers[i] = 0;
        }
    }

    // Выполнение K итераций фильтрации
    int *cur = buf1;
    int *next = buf2;

    for (int iter = 0; iter < K; ++iter) {
        // Установка указателей на текущие буферы
        in = cur;
        out = next;

        // Сброс счетчика завершенных строк
        pthread_mutex_lock(&completed_mutex);
        completed = 0;
        pthread_mutex_unlock(&completed_mutex);

        // Добавление всех строк в очередь
        for (int r = 0; r < numberOfRows; ++r) tq_push(&queue, r);

        // Ожидание завершения всех строк
        pthread_mutex_lock(&completed_mutex);
        while (completed < numberOfRows) {
            pthread_cond_wait(&completed_cond, &completed_mutex);
        }
        pthread_mutex_unlock(&completed_mutex);

        // Переключение буферов
        int *tmp = cur; cur = next; next = tmp;
    }

    // Завершение работы потоков
    tq_shutdown(&queue);
    for (int i = 0; i < max_threads; ++i) {
        if (workers[i]) pthread_join(workers[i], NULL);
    }

    // Вывод информации о параллелизме
    fprintf(stderr, "Настроено потоков: %d\n", max_threads);
    fprintf(stderr, "Максимальное одновременное количество активных потоков: %d\n", atomic_load(&maxActive));

    // Запись результата в файл
    fprintf(fout, "%d %d\n", numberOfRows, numberOfColumns);
    for (int i = 0; i < numberOfRows; ++i) {
        for (int j = 0; j < numberOfColumns; ++j) {
            fprintf(fout, "%d%c", cur[i*numberOfColumns + j], (j+1==numberOfColumns) ? '\n' : ' ');
        }
    }

    if (outfile) fclose(fout);

    // Освобождение ресурсов
    free(buf1); free(buf2);
    free(workers); free(wargs);
    tq_destroy(&queue);
    pthread_mutex_destroy(&completed_mutex);
    pthread_cond_destroy(&completed_cond);

    return 0;
}