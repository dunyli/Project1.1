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