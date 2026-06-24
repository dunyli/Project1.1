#define _CRT_SECURE_NO_WARNINGS  /* Отключаем предупреждения безопасности в Visual Studio */

#include <stdio.h>               /* Для printf, snprintf */
#include <locale.h>              /* Для setlocale - установка русской локали */
#include <stdlib.h>              /* Для malloc, free, calloc - выделение памяти */
#include <string.h>              /* Для strcmp, memcpy, strlen - работа со строками */
#include <stdbool.h>             /* Для типа bool - true/false */
#include <stdint.h>              /* Для uint32_t - 32-битные беззнаковые целые */
#include <windows.h>             /* Для Windows API: HANDLE, Interlocked... */
#include <process.h>             /* Для _beginthreadex - создание потоков */
#include <time.h>                /* Для clock(), time() - замер времени */

/*
 * Хеш-функция FNV-1a
 * Принимает строку и размер таблицы, возвращает индекс
 */
uint32_t fnv1a_hash_lf(const char* str, uint32_t table_size) {
    uint32_t hash = 2166136261U;                 /* Начальное хеш-значение */
    const unsigned char* ptr = (const unsigned char*)str;  /* Указатель на байты строки */

    while (*ptr) {                               /* Пока не дошли до конца строки */
        hash ^= (uint32_t)*ptr;                  /* XOR с текущим байтом */
        hash *= 16777619U;                       /* Умножение на простое число */
        ptr++;                                   /* Переход к следующему байту */
    }

    return hash % table_size;                    /* Возвращаем индекс в таблице */
}

/*
 * Узел связного списка
 * Хранит ключ, значение и указатель на следующий узел
 */
typedef struct LockFreeNode {
    char* key;                                   /* Ключ (NULL - узел удалён) */
    int value;                                   /* Значение */
    struct LockFreeNode* next;                   /* Указатель на следующий узел */
} LockFreeNode;

/*
 * Неблокирующая хеш-таблица
 * Использует CAS-операции для синхронизации
 */
typedef struct {
    LockFreeNode** buckets;                      /* Массив корзин */
    uint32_t size;                               /* Количество корзин */
    uint32_t mask;                               /* Маска для быстрого индекса (size-1) */
    uint32_t(*hash_func)(const char*, uint32_t); /* Указатель на хеш-функцию */
    volatile LONG count;                         /* Атомарный счётчик элементов */
} LockFreeHashTable;

/*
 * Блокирующая хеш-таблица для сравнения
 * Использует критические секции
 */
typedef struct {
    LockFreeNode** buckets;                      /* Массив корзин */
    uint32_t size;                               /* Количество корзин */
    uint32_t mask;                               /* Маска для индекса */
    uint32_t(*hash_func)(const char*, uint32_t); /* Хеш-функция */
    uint32_t count;                              /* Количество элементов */
    CRITICAL_SECTION mutex;                      /* Критическая секция для синхронизации */
} BlockingHashTable;

/*
 * Создание неблокирующей хеш-таблицы
 */
LockFreeHashTable* lfht_create(uint32_t size, uint32_t(*hash_func)(const char*, uint32_t)) {
    uint32_t actual_size = 1;                    /* Начинаем с 1 */
    while (actual_size < size) actual_size <<= 1; /* Удваиваем до степени двойки */

    LockFreeHashTable* ht = (LockFreeHashTable*)malloc(sizeof(LockFreeHashTable)); /* Выделяем память */
    if (!ht) return NULL;                        /* Проверка выделения */

    ht->size = actual_size;                      /* Сохраняем размер */
    ht->mask = actual_size - 1;                  /* Вычисляем маску */
    ht->hash_func = hash_func ? hash_func : fnv1a_hash_lf; /* Устанавливаем хеш-функцию */
    ht->count = 0;                               /* Обнуляем счётчик */

    ht->buckets = (LockFreeNode**)calloc(actual_size, sizeof(LockFreeNode*)); /* Выделяем корзины */
    if (!ht->buckets) {                          /* Проверка выделения */
        free(ht);                                /* Освобождаем таблицу */
        return NULL;
    }

    for (uint32_t i = 0; i < actual_size; i++) { /* Проходим по всем корзинам */
        ht->buckets[i] = NULL;                   /* Обнуляем каждую корзину */
    }

    return ht;                                   /* Возвращаем указатель на таблицу */
}

/*
 * Поиск ключа в цепочке
 */
LockFreeNode* lfht_find_in_chain(LockFreeNode* head, const char* key) {
    LockFreeNode* curr = head;                   /* Начинаем с головы списка */

    while (curr) {                               /* Пока есть узлы */
        char* curr_key = curr->key;              /* Сохраняем ключ локально */
        if (curr_key != NULL && strcmp(curr_key, key) == 0) { /* Если ключ совпадает */
            return curr;                         /* Возвращаем узел */
        }
        curr = curr->next;                       /* Переходим к следующему узлу */
    }

    return NULL;                                 /* Ключ не найден */
}

/*
 * Поиск значения по ключу
 */
bool lfht_get(LockFreeHashTable* ht, const char* key, int* out_value) {
    uint32_t index = ht->hash_func(key, ht->size) & ht->mask; /* Вычисляем индекс */
    LockFreeNode* head = ht->buckets[index];     /* Получаем голову списка */
    LockFreeNode* node = lfht_find_in_chain(head, key); /* Ищем ключ */

    if (node && node->key != NULL) {             /* Если нашли и узел не удалён */
        if (out_value) *out_value = node->value; /* Сохраняем значение */
        return true;                             /* Успешно */
    }

    return false;                                /* Не найдено */
}

/*
 * Создание нового узла
 */
LockFreeNode* create_lf_node(const char* key, int value) {
    LockFreeNode* node = (LockFreeNode*)malloc(sizeof(LockFreeNode)); /* Выделяем память */
    if (!node) return NULL;                      /* Проверка */

    node->key = (char*)malloc(strlen(key) + 1);  /* Выделяем память под ключ */
    if (!node->key) {                            /* Проверка */
        free(node);                              /* Освобождаем узел */
        return NULL;
    }
    memcpy(node->key, key, strlen(key) + 1);     /* Копируем ключ */

    node->value = value;                         /* Устанавливаем значение */
    node->next = NULL;                           /* Следующий узел пока не известен */

    return node;                                 /* Возвращаем узел */
}

/*
 * Вставка ключа и значения
 */
bool lfht_insert(LockFreeHashTable* ht, const char* key, int value) {
    uint32_t index = ht->hash_func(key, ht->size) & ht->mask; /* Вычисляем индекс */

    while (true) {                               /* Цикл до успешной вставки */
        LockFreeNode* head = ht->buckets[index]; /* Текущая голова списка */

        LockFreeNode* curr = head;               /* Начинаем поиск с головы */
        while (curr) {                           /* Проверяем все узлы */
            char* curr_key = curr->key;          /* Сохраняем ключ локально */
            if (curr_key != NULL && strcmp(curr_key, key) == 0) { /* Если ключ есть */
                if (curr->key != NULL) {         /* Проверяем что не удалён */
                    curr->value = value;         /* Обновляем значение */
                    return true;                 /* Успешно */
                }
                break;                           /* Выходим из цикла */
            }
            curr = curr->next;                   /* Переходим к следующему */
        }

        LockFreeNode* new_node = create_lf_node(key, value); /* Создаём узел */
        if (!new_node) return false;             /* Проверка */

        new_node->next = head;                   /* Вставляем в начало списка */

        if (InterlockedCompareExchangePointer(   /* CAS-операция */
            (PVOID*)&ht->buckets[index],         /* Куда записываем */
            new_node,                            /* Новое значение */
            head) == head) {                     /* Проверяем что было head */
            InterlockedIncrement(&ht->count);    /* Увеличиваем счётчик */
            return true;                         /* Успешно */
        }

        free(new_node->key);                     /* Освобождаем ключ */
        free(new_node);                          /* Освобождаем узел */
        /* Цикл повторится с обновлённым head */
    }
}

/*
 * Удаление ключа
 */
bool lfht_delete(LockFreeHashTable* ht, const char* key) {
    uint32_t index = ht->hash_func(key, ht->size) & ht->mask; /* Вычисляем индекс */

    while (true) {                               /* Цикл до успешного удаления */
        LockFreeNode* head = ht->buckets[index]; /* Голова списка */
        LockFreeNode* prev = NULL;               /* Предыдущий узел */
        LockFreeNode* curr = head;               /* Текущий узел */

        while (curr) {                           /* Ищем узел для удаления */
            char* curr_key = curr->key;          /* Сохраняем ключ локально */
            if (curr_key != NULL && strcmp(curr_key, key) == 0) { /* Нашли */
                break;
            }
            prev = curr;                         /* Запоминаем предыдущий */
            curr = curr->next;                   /* Переходим дальше */
        }

        if (!curr) return false;                 /* Ключ не найден */

        if (curr->key == NULL) {                 /* Если узел уже удалён */
            continue;                            /* Пробуем сначала */
        }

        if (prev == NULL) {                      /* Удаляем голову */
            LockFreeNode* next = curr->next;     /* Следующий узел */
            if (InterlockedCompareExchangePointer( /* CAS-операция */
                (PVOID*)&ht->buckets[index],     /* Куда записываем */
                next,                            /* Новое значение */
                curr) == curr) {                 /* Проверяем что было curr */
                char* old_key = curr->key;       /* Сохраняем ключ */
                curr->key = NULL;                /* Помечаем как удалённый */
                if (old_key != NULL) {           /* Если ключ не NULL */
                    free(old_key);               /* Освобождаем ключ */
                }
                InterlockedDecrement(&ht->count); /* Уменьшаем счётчик */
                return true;                     /* Успешно */
            }
        }
        else {                                   /* Удаляем из середины или конца */
            LockFreeNode* next = curr->next;     /* Следующий узел */
            if (InterlockedCompareExchangePointer( /* CAS-операция */
                (PVOID*)&prev->next,             /* Куда записываем */
                next,                            /* Новое значение */
                curr) == curr) {                 /* Проверяем что было curr */
                char* old_key = curr->key;       /* Сохраняем ключ */
                curr->key = NULL;                /* Помечаем как удалённый */
                if (old_key != NULL) {           /* Если ключ не NULL */
                    free(old_key);               /* Освобождаем ключ */
                }
                InterlockedDecrement(&ht->count); /* Уменьшаем счётчик */
                return true;                     /* Успешно */
            }
        }
        /* CAS не удался - пробуем снова */
    }
}

/*
 * Освобождение памяти таблицы
 */
void lfht_destroy(LockFreeHashTable* ht) {
    if (!ht) return;                             /* Проверка */

    for (uint32_t i = 0; i < ht->size; i++) {    /* Проходим по всем корзинам */
        LockFreeNode* curr = ht->buckets[i];     /* Начинаем с головы */
        while (curr) {                           /* Пока есть узлы */
            LockFreeNode* next = curr->next;     /* Сохраняем следующий */
            if (curr->key) free(curr->key);      /* Освобождаем ключ */
            free(curr);                          /* Освобождаем узел */
            curr = next;                         /* Переходим к следующему */
        }
    }

    free(ht->buckets);                           /* Освобождаем корзины */
    free(ht);                                    /* Освобождаем таблицу */
}

/*
 * Печать таблицы
 */
void lfht_print(LockFreeHashTable* ht) {
    printf("Lock-Free HashTable (size=%u, count=%ld):\n", ht->size, ht->count);

    for (uint32_t i = 0; i < ht->size; i++) {    /* Проходим по корзинам */
        LockFreeNode* curr = ht->buckets[i];     /* Начинаем с головы */
        if (curr) {                              /* Если есть элементы */
            printf("  [%u] ", i);                /* Выводим номер корзины */
            while (curr) {                       /* Проходим по узлам */
                char* curr_key = curr->key;      /* Сохраняем ключ локально */
                if (curr_key != NULL) {          /* Если ключ есть */
                    printf("'%s':%d ", curr_key, curr->value); /* Выводим данные */
                }
                else {
                    printf("[DELETED] ");        /* Удалённый узел */
                }
                curr = curr->next;               /* Переходим к следующему */
            }
            printf("\n");                        /* Конец строки */
        }
    }
}

/*
 * Создание блокирующей хеш-таблицы
 */
BlockingHashTable* bht_create(uint32_t size) {
    BlockingHashTable* ht = (BlockingHashTable*)malloc(sizeof(BlockingHashTable)); /* Выделяем память */
    ht->size = size;                             /* Сохраняем размер */
    ht->mask = size - 1;                         /* Вычисляем маску */
    ht->hash_func = fnv1a_hash_lf;               /* Устанавливаем хеш-функцию */
    ht->count = 0;                               /* Обнуляем счётчик */
    ht->buckets = (LockFreeNode**)calloc(size, sizeof(LockFreeNode*)); /* Выделяем корзины */
    InitializeCriticalSection(&ht->mutex);       /* Инициализируем критическую секцию */
    return ht;                                   /* Возвращаем таблицу */
}


/*
 * Вставка в блокирующую таблицу
 */
void bht_insert(BlockingHashTable* ht, const char* key, int value) {
    EnterCriticalSection(&ht->mutex);            /* Входим в критическую секцию */

    uint32_t index = ht->hash_func(key, ht->size) & ht->mask; /* Вычисляем индекс */
    LockFreeNode* curr = ht->buckets[index];     /* Получаем голову */

    while (curr) {                               /* Ищем ключ */
        if (strcmp(curr->key, key) == 0) {       /* Если нашли */
            curr->value = value;                 /* Обновляем значение */
            LeaveCriticalSection(&ht->mutex);    /* Выходим из секции */
            return;                              /* Завершаем */
        }
        curr = curr->next;                       /* Переходим дальше */
    }

    LockFreeNode* node = create_lf_node(key, value); /* Создаём узел */
    node->next = ht->buckets[index];             /* Вставляем в начало */
    ht->buckets[index] = node;                   /* Обновляем голову */
    ht->count++;                                 /* Увеличиваем счётчик */

    LeaveCriticalSection(&ht->mutex);            /* Выходим из секции */
}

/*
 * Поиск в блокирующей таблице
 */
bool bht_get(BlockingHashTable* ht, const char* key, int* out_value) {
    EnterCriticalSection(&ht->mutex);            /* Входим в секцию */

    uint32_t index = ht->hash_func(key, ht->size) & ht->mask; /* Индекс */
    LockFreeNode* curr = ht->buckets[index];     /* Голова списка */

    while (curr) {                               /* Ищем ключ */
        if (strcmp(curr->key, key) == 0) {       /* Если нашли */
            if (out_value) *out_value = curr->value; /* Сохраняем значение */
            LeaveCriticalSection(&ht->mutex);    /* Выходим из секции */
            return true;                         /* Успешно */
        }
        curr = curr->next;                       /* Переходим дальше */
    }

    LeaveCriticalSection(&ht->mutex);            /* Выходим из секции */
    return false;                                /* Не найдено */
}

/*
 * Удаление в блокирующей таблице
 */
void bht_delete(BlockingHashTable* ht, const char* key) {
    EnterCriticalSection(&ht->mutex);            /* Входим в секцию */

    uint32_t index = ht->hash_func(key, ht->size) & ht->mask; /* Индекс */
    LockFreeNode* curr = ht->buckets[index];     /* Голова списка */
    LockFreeNode* prev = NULL;                   /* Предыдущий узел */

    while (curr) {                               /* Ищем ключ */
        if (strcmp(curr->key, key) == 0) {       /* Если нашли */
            if (prev) {                          /* Если не голова */
                prev->next = curr->next;         /* Перекидываем указатель */
            }
            else {                               /* Если голова */
                ht->buckets[index] = curr->next; /* Обновляем голову */
            }
            free(curr->key);                     /* Освобождаем ключ */
            free(curr);                          /* Освобождаем узел */
            ht->count--;                         /* Уменьшаем счётчик */
            break;                               /* Выходим из цикла */
        }
        prev = curr;                             /* Запоминаем предыдущий */
        curr = curr->next;                       /* Переходим дальше */
    }

    LeaveCriticalSection(&ht->mutex);            /* Выходим из секции */
}

