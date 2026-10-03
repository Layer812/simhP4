#include "simhp4_rb09_persist.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * SIMHP4 R0A9R12
 *
 * RB09 is 1,024,000 18-bit words stored by Open SIMH as 32-bit words:
 *   1,024,000 * 4 = 4,096,000 bytes
 *   4,096,000 / 512 = 8,000 persistence blocks
 *
 * CPU1 only performs:
 *   RAM word store + dirty-bit mark
 *
 * CPU0 performs:
 *   copy one dirty 512-byte block under a short cross-core lock
 *   release lock
 *   fseek/fwrite/fflush/fsync to SD
 *
 * No SD call occurs in the guest RB09 service path.
 */

#define TAG "SIMHP4_RB09_PERSIST"

#define SIMHP4_RB09_BLOCK_BYTES          512u
#define SIMHP4_RB09_WORD_BYTES           ((size_t)sizeof(int32_t))
#define SIMHP4_RB09_WORDS_PER_BLOCK      (SIMHP4_RB09_BLOCK_BYTES / SIMHP4_RB09_WORD_BYTES)
#define SIMHP4_RB09_MAX_BLOCKS           8000u
#define SIMHP4_RB09_DIRTY_WORDS          ((SIMHP4_RB09_MAX_BLOCKS + 31u) / 32u)
#define SIMHP4_RB09_FLUSH_PERIOD_MS      750u
#define SIMHP4_RB09_TASK_STACK           4096u
#define SIMHP4_RB09_TASK_PRIO            2u
#define SIMHP4_RB09_TASK_CORE            0

static int32_t *s_filebuf;
static size_t s_word_count;
static size_t s_block_count;
static char s_path[128];

static uint32_t s_dirty[SIMHP4_RB09_DIRTY_WORDS];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static TaskHandle_t s_task;
static volatile uint32_t s_flush_count;
static volatile uint32_t s_error_count;
static volatile uint32_t s_dirty_generation;

static inline void dirty_set_locked(size_t block)
{
    s_dirty[block >> 5] |= (1u << (block & 31u));
}

static inline int dirty_test_locked(size_t block)
{
    return (s_dirty[block >> 5] & (1u << (block & 31u))) != 0;
}

static inline void dirty_clear_locked(size_t block)
{
    s_dirty[block >> 5] &= ~(1u << (block & 31u));
}

static void dirty_restore(size_t block)
{
    portENTER_CRITICAL(&s_lock);
    dirty_set_locked(block);
    ++s_dirty_generation;
    portEXIT_CRITICAL(&s_lock);
}

static int copy_dirty_block(size_t block,
                            uint8_t out[SIMHP4_RB09_BLOCK_BYTES],
                            size_t *bytes_out)
{
    const size_t first_word = block * SIMHP4_RB09_WORDS_PER_BLOCK;
    size_t words;
    size_t bytes;

    if (bytes_out)
        *bytes_out = 0;

    if (s_filebuf == NULL || first_word >= s_word_count)
        return 0;

    portENTER_CRITICAL(&s_lock);

    if (!dirty_test_locked(block)) {
        portEXIT_CRITICAL(&s_lock);
        return 0;
    }

    words = s_word_count - first_word;
    if (words > SIMHP4_RB09_WORDS_PER_BLOCK)
        words = SIMHP4_RB09_WORDS_PER_BLOCK;
    bytes = words * SIMHP4_RB09_WORD_BYTES;

    memcpy(out, &s_filebuf[first_word], bytes);
    dirty_clear_locked(block);

    portEXIT_CRITICAL(&s_lock);

    if (bytes_out)
        *bytes_out = bytes;
    return 1;
}

static FILE *open_backing_image(void)
{
    FILE *fp = fopen(s_path, "r+b");
    if (fp == NULL) {
        ++s_error_count;
        printf("%s: open failed path=%s errno=%d\n", TAG, s_path, errno);
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        ++s_error_count;
        printf("%s: seek-end failed errno=%d\n", TAG, errno);
        fclose(fp);
        return NULL;
    }

    const long size = ftell(fp);
    const long expected = (long)(s_word_count * SIMHP4_RB09_WORD_BYTES);

    if (size != expected) {
        ++s_error_count;
        printf("%s: size mismatch got=%ld expected=%ld\n",
               TAG, size, expected);
        fclose(fp);
        return NULL;
    }

    rewind(fp);
    setvbuf(fp, NULL, _IONBF, 0);
    return fp;
}

static int flush_pass(FILE *fp)
{
    uint8_t staging[SIMHP4_RB09_BLOCK_BYTES];
    uint32_t blocks_written = 0;

    for (size_t block = 0; block < s_block_count; ++block) {
        size_t bytes = 0;

        if (!copy_dirty_block(block, staging, &bytes))
            continue;

        const long offset = (long)(block * SIMHP4_RB09_BLOCK_BYTES);

        if (fseek(fp, offset, SEEK_SET) != 0 ||
            fwrite(staging, 1, bytes, fp) != bytes) {
            ++s_error_count;
            printf("%s: write failed block=%u errno=%d\n",
                   TAG, (unsigned)block, errno);
            clearerr(fp);
            dirty_restore(block);
            return -1;
        }

        ++blocks_written;
    }

    if (blocks_written == 0)
        return 0;

    if (fflush(fp) != 0) {
        ++s_error_count;
        printf("%s: fflush failed errno=%d\n", TAG, errno);
        return -1;
    }

    const int fd = fileno(fp);
    if (fd >= 0 && fsync(fd) != 0) {
        ++s_error_count;
        printf("%s: fsync failed errno=%d\n", TAG, errno);
        return -1;
    }

    const uint32_t seq = __atomic_add_fetch(&s_flush_count, 1u, __ATOMIC_RELAXED);
    printf("%s: PASS flush=%u dirty_blocks=%u generation=%u\n",
           TAG,
           (unsigned)seq,
           (unsigned)blocks_written,
           (unsigned)__atomic_load_n(&s_dirty_generation, __ATOMIC_RELAXED));
    return 1;
}

static void persist_task(void *arg)
{
    (void)arg;
    FILE *fp = NULL;

    printf("%s: task entered CPU%d prio=%u blocks=%u period=%ums path=%s\n",
           TAG,
           xPortGetCoreID(),
           (unsigned)SIMHP4_RB09_TASK_PRIO,
           (unsigned)s_block_count,
           (unsigned)SIMHP4_RB09_FLUSH_PERIOD_MS,
           s_path);

    for (;;) {
        if (fp == NULL) {
            fp = open_backing_image();
            if (fp != NULL) {
                printf("%s: backing image READY size=%u bytes\n",
                       TAG,
                       (unsigned)(s_word_count * SIMHP4_RB09_WORD_BYTES));
            } else {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        const int rc = flush_pass(fp);
        if (rc < 0) {
            fclose(fp);
            fp = NULL;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(SIMHP4_RB09_FLUSH_PERIOD_MS));
    }
}

int simhp4_rb09_persist_start(const char *path,
                              int32_t *filebuf,
                              size_t word_count)
{
    if (path == NULL || filebuf == NULL || word_count == 0)
        return -1;

    if ((word_count * SIMHP4_RB09_WORD_BYTES) %
            SIMHP4_RB09_BLOCK_BYTES != 0)
        return -2;

    const size_t block_count =
        (word_count * SIMHP4_RB09_WORD_BYTES) / SIMHP4_RB09_BLOCK_BYTES;

    if (block_count == 0 || block_count > SIMHP4_RB09_MAX_BLOCKS)
        return -3;

    const size_t path_len = strlen(path);
    if (path_len == 0 || path_len >= sizeof(s_path))
        return -4;

    if (s_task != NULL)
        return (s_filebuf == filebuf && s_word_count == word_count) ? 0 : -5;

    portENTER_CRITICAL(&s_lock);
    s_filebuf = filebuf;
    s_word_count = word_count;
    s_block_count = block_count;
    memcpy(s_path, path, path_len + 1);
    memset(s_dirty, 0, sizeof(s_dirty));
    s_flush_count = 0;
    s_error_count = 0;
    s_dirty_generation = 0;
    portEXIT_CRITICAL(&s_lock);

    BaseType_t ok = xTaskCreatePinnedToCore(persist_task,
                                            "rb09_persist",
                                            SIMHP4_RB09_TASK_STACK,
                                            NULL,
                                            SIMHP4_RB09_TASK_PRIO,
                                            &s_task,
                                            SIMHP4_RB09_TASK_CORE);
    if (ok != pdPASS) {
        s_task = NULL;
        return -6;
    }

    printf("%s: START CPU0 async dirty-block persistence words=%u blocks=%u\n",
           TAG, (unsigned)word_count, (unsigned)block_count);
    return 0;
}

void simhp4_rb09_persist_write_word(int32_t *filebuf,
                                    uint32_t word_index,
                                    int32_t value)
{
    if (filebuf == NULL)
        return;

    /* Preserve original RB09 behavior even if persistence has not started. */
    if (s_filebuf != filebuf || word_index >= s_word_count) {
        filebuf[word_index] = value;
        return;
    }

    const size_t block = (size_t)word_index / SIMHP4_RB09_WORDS_PER_BLOCK;

    portENTER_CRITICAL(&s_lock);
    filebuf[word_index] = value;
    dirty_set_locked(block);
    ++s_dirty_generation;
    portEXIT_CRITICAL(&s_lock);
}

uint32_t simhp4_rb09_persist_flush_count(void)
{
    return __atomic_load_n(&s_flush_count, __ATOMIC_ACQUIRE);
}

uint32_t simhp4_rb09_persist_error_count(void)
{
    return __atomic_load_n(&s_error_count, __ATOMIC_ACQUIRE);
}
