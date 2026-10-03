/*
 * simhP4 / RetroP4 integration
 * Copyright (c) 2026 simhP4 contributors
 *
 * Upstream SIMH-derived portions retain their original copyright notices
 * and license terms.  See LICENSES/SIMH_LICENSE.txt.
 */
#define _POSIX_C_SOURCE 200809L
#include "simhp4_st_migration.h"
#include "simhp4_btools_migration.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"

/* SIMHP4_R0A9R16D_SPACE_TRAVEL_83_BACKUP_FALLBACK
 *
 * R14D/R14E proved the current system/st execution matches the historical
 * bad assembly order:
 *
 *   st1 st2 st3 st4 st5 st6 st7 fop
 *
 * The user's recovered working tree contains the upstream fix commit
 * "Space Travel now runs without crashing after reordering input files."
 * and a working build/bin/st assembled as:
 *
 *   st1 st2 st3 st4 fop st5 st6 st7
 *
 * Important: a simple section rotation is NOT sufficient because relocating
 * sections changes embedded absolute/relative references.  R15 therefore
 * performs an exact whole-file replacement only when system/st matches the
 * complete known-bad 2927-word image.  Unknown files are left untouched.
 */

static const char *TAG = "SIMHP4_ST_R16B";

#define R15_NUMBLOCKS          8000u
#define R15_WORDS_PER_BLOCK      64u
#define R15_BYTES_PER_WORD         4u
#define R15_FS_BASE_BYTES \
    (R15_NUMBLOCKS * R15_WORDS_PER_BLOCK * R15_BYTES_PER_WORD)

#define R15_FIRST_INODE_BLOCK      2u
#define R15_INODE_SIZE            12u
#define R15_INODES_PER_BLOCK       5u
#define R15_I_FLAGS                0u
#define R15_I_DISKPS               1u
#define R15_I_SIZE                10u
#define R15_I_NUMBLKS              7u
#define R15_I_USED            0400000u
#define R15_I_LARGE           0200000u
#define R15_I_DIRECTORY       0000020u

#define R15_DIRENT_SIZE             8u
#define R15_D_INUM                  0u
#define R15_D_NAME                  1u

#define R15_SYSTEM_INUM             3u
#define R15_MAX_FILE_BLOCKS       384u

/* Exact recovered Space Travel executable size. */
#define R15_ST_WORDS             05557u /* 2927 words */

/* Known addresses, for diagnostic logs only. */
#define R15_BAD_DISPLIST_OFF     04225u /* memory 014225 */
#define R15_BAD_DSPL_OFF         04301u /* memory 014301 */
#define R15_BAD_FMP_Q_OFF        04302u /* memory 014302 */
#define R15_GOOD_FMP_Q_OFF       02132u /* memory 012132 */
#define R15_GOOD_DISPLIST_OFF    05502u /* memory 015502 */
#define R15_GOOD_DSPL_OFF        05556u /* memory 015556 */

#ifndef R15_BACKUP_PATH
#define R15_BACKUP_PATH "/sdcard/SIMHP4_R15_ST_BAD.BAK"
#endif

/* R16B: the live SD image is the same proven bad layout, but its crash
 * initial value at memory 014006 is -027 instead of the recovered bad
 * reference -028.  R14D/R14E still prove the same destructive layout:
 * dspl=014301 immediately overlaps fmp q=014302.  Recognize ONLY this
 * exact one-word variant; any additional difference remains read-only. */
#ifndef R16B_VARIANT_BACKUP_PATH
#define R16B_VARIANT_BACKUP_PATH "/sdcard/SIMHP4_R16B_ST_VARIANT.BAK"
#endif
#ifndef R16C_VARIANT_BACKUP_PATH
#define R16C_VARIANT_BACKUP_PATH "/sdcard/SIMHP4_R16C_ST_VARIANT.BAK"
#endif
/* R16D: use an 8.3-safe fallback name.  The live device can open/write the
 * short UNIXV0.DSK and BOOT.RIM paths, while both previous long backup names
 * failed before any migration write.  Preserve all older paths untouched. */
#ifndef R16D_VARIANT_BACKUP_PATH
#define R16D_VARIANT_BACKUP_PATH "/sdcard/R16D_ST.BAK"
#endif
#define R16B_OBSERVED_DIFF_OFF  04006u /* memory 014006: crash */
#define R16B_BAD_REF_WORD      0777750u /* -028 */
#define R16B_OBSERVED_WORD     0777751u /* -027 */

#include "simhp4_st_images.inc"

static int r15_seek_word(FILE *fp, uint32_t block, uint32_t word_off)
{
    const uint64_t word_index = (uint64_t)block * R15_WORDS_PER_BLOCK + word_off;
    const uint64_t byte_off = (uint64_t)R15_FS_BASE_BYTES +
                              word_index * R15_BYTES_PER_WORD;
    if (byte_off > 0x7fffffffu)
        return 0;
    return fseek(fp, (long)byte_off, SEEK_SET) == 0;
}

static int r15_read_word(FILE *fp, uint32_t block, uint32_t word_off, uint32_t *out)
{
    uint8_t b[4];
    if (!r15_seek_word(fp, block, word_off))
        return 0;
    if (fread(b, 1, sizeof(b), fp) != sizeof(b))
        return 0;
    *out = ((uint32_t)b[0] |
            ((uint32_t)b[1] << 8) |
            ((uint32_t)b[2] << 16) |
            ((uint32_t)b[3] << 24)) & 0777777u;
    return 1;
}

static int r15_write_word(FILE *fp, uint32_t block, uint32_t word_off, uint32_t v)
{
    v &= 0777777u;
    const uint8_t b[4] = {
        (uint8_t)(v & 0xffu),
        (uint8_t)((v >> 8) & 0xffu),
        (uint8_t)((v >> 16) & 0xffu),
        (uint8_t)((v >> 24) & 0xffu),
    };
    if (!r15_seek_word(fp, block, word_off))
        return 0;
    return fwrite(b, 1, sizeof(b), fp) == sizeof(b);
}

static int r15_inode_pos(uint32_t inum, uint32_t *block, uint32_t *off)
{
    *block = R15_FIRST_INODE_BLOCK + (inum / R15_INODES_PER_BLOCK);
    *off = R15_INODE_SIZE * (inum % R15_INODES_PER_BLOCK);
    return *off + R15_INODE_SIZE <= R15_WORDS_PER_BLOCK;
}

static int r15_get_file_blocks(FILE *fp,
                               uint32_t inum,
                               uint32_t *blocks,
                               size_t cap,
                               uint32_t *size_words,
                               size_t *nblocks,
                               uint32_t *flags_out)
{
    uint32_t flags = 0, size = 0;
    uint32_t iblock, ioff;
    size_t n = 0;

    if (!r15_inode_pos(inum, &iblock, &ioff) ||
        !r15_read_word(fp, iblock, ioff + R15_I_FLAGS, &flags) ||
        !r15_read_word(fp, iblock, ioff + R15_I_SIZE, &size))
        return 0;
    if ((flags & R15_I_USED) == 0)
        return 0;

    if (flags & R15_I_LARGE) {
        for (uint32_t p = 0; p < R15_I_NUMBLKS; ++p) {
            uint32_t indirect = 0;
            if (!r15_read_word(fp, iblock, ioff + R15_I_DISKPS + p, &indirect))
                return 0;
            if (indirect == 0)
                continue;
            if (indirect >= R15_NUMBLOCKS)
                return 0;
            for (uint32_t j = 0; j < R15_WORDS_PER_BLOCK; ++j) {
                uint32_t data = 0;
                if (!r15_read_word(fp, indirect, j, &data))
                    return 0;
                if (data == 0)
                    continue;
                if (data >= R15_NUMBLOCKS || n >= cap)
                    return 0;
                blocks[n++] = data;
            }
        }
    } else {
        for (uint32_t p = 0; p < R15_I_NUMBLKS; ++p) {
            uint32_t data = 0;
            if (!r15_read_word(fp, iblock, ioff + R15_I_DISKPS + p, &data))
                return 0;
            if (data == 0)
                continue;
            if (data >= R15_NUMBLOCKS || n >= cap)
                return 0;
            blocks[n++] = data;
        }
    }

    if (n * R15_WORDS_PER_BLOCK < size)
        return 0;

    *size_words = size;
    *nblocks = n;
    if (flags_out)
        *flags_out = flags;
    return 1;
}

static void r15_decode_name(const uint32_t words[4], char out[9])
{
    size_t n = 0;
    for (size_t i = 0; i < 4; ++i) {
        out[n++] = (char)((words[i] >> 9) & 0177u);
        out[n++] = (char)(words[i] & 0177u);
    }
    out[8] = '\0';
    while (n != 0 && out[n - 1] == ' ')
        out[--n] = '\0';
}

static int r15_find_in_dir(FILE *fp, uint32_t dir_inum,
                           const char *want, uint32_t *found_inum)
{
    uint32_t blocks[R15_MAX_FILE_BLOCKS];
    uint32_t size = 0, flags = 0;
    size_t nblocks = 0;
    uint32_t seen = 0;

    if (!r15_get_file_blocks(fp, dir_inum, blocks,
                             R15_MAX_FILE_BLOCKS, &size, &nblocks, &flags))
        return 0;
    if ((flags & R15_I_DIRECTORY) == 0)
        return 0;

    for (size_t bi = 0; bi < nblocks && seen < size; ++bi) {
        for (uint32_t off = 0; off < R15_WORDS_PER_BLOCK && seen < size;
             off += R15_DIRENT_SIZE) {
            uint32_t inum = 0;
            uint32_t nw[4];
            char name[9];
            if (!r15_read_word(fp, blocks[bi], off + R15_D_INUM, &inum))
                return 0;
            for (uint32_t k = 0; k < 4; ++k) {
                if (!r15_read_word(fp, blocks[bi], off + R15_D_NAME + k, &nw[k]))
                    return 0;
            }
            r15_decode_name(nw, name);
            if (inum != 0 && strcmp(name, want) == 0) {
                *found_inum = inum;
                return 1;
            }
            seen += R15_DIRENT_SIZE;
        }
    }
    return 0;
}

static int r15_read_file_words(FILE *fp, uint32_t inum,
                               uint32_t *dst, uint32_t expected_words,
                               uint32_t *blocks_out, size_t *nblocks_out)
{
    uint32_t blocks[R15_MAX_FILE_BLOCKS];
    uint32_t size = 0;
    size_t nblocks = 0;
    uint32_t pos = 0;

    if (!r15_get_file_blocks(fp, inum, blocks, R15_MAX_FILE_BLOCKS,
                             &size, &nblocks, NULL))
        return 0;
    if (size != expected_words)
        return 0;

    for (size_t bi = 0; bi < nblocks && pos < size; ++bi) {
        for (uint32_t off = 0; off < R15_WORDS_PER_BLOCK && pos < size; ++off) {
            if (!r15_read_word(fp, blocks[bi], off, &dst[pos++]))
                return 0;
        }
    }
    if (pos != size)
        return 0;

    if (blocks_out) {
        for (size_t i = 0; i < nblocks; ++i)
            blocks_out[i] = blocks[i];
    }
    if (nblocks_out)
        *nblocks_out = nblocks;
    return 1;
}

static int r15_write_file_words(FILE *fp, const uint32_t *blocks,
                                size_t nblocks, const uint32_t *src,
                                uint32_t words)
{
    uint32_t pos = 0;
    for (size_t bi = 0; bi < nblocks && pos < words; ++bi) {
        for (uint32_t off = 0; off < R15_WORDS_PER_BLOCK && pos < words; ++off) {
            if (!r15_write_word(fp, blocks[bi], off, src[pos++]))
                return 0;
        }
    }
    return pos == words;
}

static uint32_t r15_first_diff(const uint32_t *a, const uint32_t *b)
{
    for (uint32_t i = 0; i < R15_ST_WORDS; ++i) {
        if ((a[i] & 0777777u) != (b[i] & 0777777u))
            return i;
    }
    return R15_ST_WORDS;
}

static int r16b_backup_matches(const char *path, const uint32_t *words)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
        return 0;

    int ok = 1;
    for (uint32_t i = 0; i < R15_ST_WORDS; ++i) {
        uint8_t b[4];
        if (fread(b, 1, sizeof(b), fp) != sizeof(b)) {
            ok = 0;
            break;
        }
        const uint32_t v = ((uint32_t)b[0] |
                            ((uint32_t)b[1] << 8) |
                            ((uint32_t)b[2] << 16) |
                            ((uint32_t)b[3] << 24)) & 0777777u;
        if (v != (words[i] & 0777777u)) {
            ok = 0;
            break;
        }
    }
    if (ok && fgetc(fp) != EOF)
        ok = 0;
    fclose(fp);
    return ok;
}

static int r16c_backup_to_path(const char *path, const uint32_t *words)
{
    FILE *fp = fopen(path, "rb");
    if (fp != NULL) {
        fclose(fp);
        return r16b_backup_matches(path, words);
    }

    fp = fopen(path, "wb");
    if (fp == NULL)
        return 0;
    for (uint32_t i = 0; i < R15_ST_WORDS; ++i) {
        const uint32_t v = words[i] & 0777777u;
        const uint8_t b[4] = {
            (uint8_t)(v & 0xffu),
            (uint8_t)((v >> 8) & 0xffu),
            (uint8_t)((v >> 16) & 0xffu),
            (uint8_t)((v >> 24) & 0xffu),
        };
        if (fwrite(b, 1, sizeof(b), fp) != sizeof(b)) {
            fclose(fp);
            return 0;
        }
    }
    if (fflush(fp) != 0) {
        fclose(fp);
        return 0;
    }
    {
        const int fd = fileno(fp);
        if (fd >= 0 && fsync(fd) != 0) {
            fclose(fp);
            return 0;
        }
    }
    if (fclose(fp) != 0)
        return 0;
    return r16b_backup_matches(path, words);
}

static int r16b_backup_original(const char *path, const uint32_t *words)
{
    FILE *fp = fopen(path, "rb");
    if (fp != NULL) {
        fclose(fp);
        if (r16b_backup_matches(path, words))
            return 1;

        /* Preserve any pre-existing long-name backup exactly as-is. */
        ESP_LOGW(TAG,
                 "primary backup exists but differs from exact current st; preserving path=%s",
                 path);
    } else {
        /* R16C proved that the long backup pathname could not be created on
         * the live SD volume.  Do not keep retrying or delete anything. */
        ESP_LOGW(TAG,
                 "primary backup path unavailable/absent; preserving SD state and using 8.3 fallback path=%s",
                 R16D_VARIANT_BACKUP_PATH);
    }

    /* R16D: always fall back to one short 8.3-compatible pathname.  Existing
     * content is never overwritten: r16c_backup_to_path() accepts it only if
     * it already matches this exact 2927-word executable. */
    if (r16c_backup_to_path(R16D_VARIANT_BACKUP_PATH, words)) {
        ESP_LOGI(TAG, "PASS exact 8.3 fallback backup path=%s",
                 R16D_VARIANT_BACKUP_PATH);
        return 1;
    }

    ESP_LOGE(TAG,
             "8.3 fallback backup create/verify failed or existing file differs; refusing write path=%s",
             R16D_VARIANT_BACKUP_PATH);
    return 0;
}

static int r16b_matches_observed_bad_variant(const uint32_t *words)
{
    for (uint32_t i = 0; i < R15_ST_WORDS; ++i) {
        uint32_t expected = r15_bad_st_words[i] & 0777777u;
        if (i == R16B_OBSERVED_DIFF_OFF)
            expected = R16B_OBSERVED_WORD;
        if ((words[i] & 0777777u) != expected)
            return 0;
    }
    return 1;
}

static int r15_sync(FILE *fp)
{
    if (fflush(fp) != 0)
        return 0;
    const int fd = fileno(fp);
    return fd < 0 || fsync(fd) == 0;
}

static int r16b_restore_original(FILE *fp,
                                const uint32_t *blocks,
                                size_t nblocks,
                                const uint32_t *original)
{
    if (!r15_write_file_words(fp, blocks, nblocks,
                              original, R15_ST_WORDS))
        return 0;
    return r15_sync(fp);
}

int simhp4_st_migrate_bad_layout(const char *disk_path)
{
    FILE *fp = NULL;
    uint32_t st_inum = 0;
    uint32_t *cur = NULL;
    uint32_t st_blocks[R15_MAX_FILE_BLOCKS];
    size_t st_nblocks = 0;
    int result = -1;

    if (disk_path == NULL)
        return -1;

    /* SIMHP4_RELEASE_FINAL_BTOOLS_HOOK
     * Run the B-tools filesystem audit/migration before opening the disk for
     * the existing Space Travel layout check.  A B-tools refusal never
     * changes Space Travel behavior or prevents the guest from booting. */
    {
        const int btools_state = simhp4_btools_migrate(disk_path);
        if (btools_state < 0)
            ESP_LOGW(TAG, "B-tools migration reported failure; continuing existing st audit state=%d",
                     btools_state);
        else
            ESP_LOGI(TAG, "B-tools state=%d", btools_state);
    }

    fp = fopen(disk_path, "r+b");
    if (fp == NULL) {
        ESP_LOGE(TAG, "open failed path=%s errno=%d (%s)",
                 disk_path, errno, strerror(errno));
        return -1;
    }

    if (!r15_find_in_dir(fp, R15_SYSTEM_INUM, "st", &st_inum)) {
        ESP_LOGW(TAG, "system/st not found; no migration performed");
        result = 0;
        goto done;
    }

    cur = (uint32_t *)malloc((size_t)R15_ST_WORDS * sizeof(uint32_t));
    if (cur == NULL) {
        ESP_LOGE(TAG, "allocation failed words=%u", (unsigned)R15_ST_WORDS);
        result = -1;
        goto done;
    }

    if (!r15_read_file_words(fp, st_inum, cur, R15_ST_WORDS,
                             st_blocks, &st_nblocks)) {
        ESP_LOGW(TAG,
                 "system/st size is not exact recovered size=%u words; no write",
                 (unsigned)R15_ST_WORDS);
        result = 0;
        goto done;
    }

    if (memcmp(cur, r15_good_st_words, sizeof(r15_good_st_words)) == 0) {
        ESP_LOGI(TAG,
                 "PASS already-good exact recovered st inum=%u words=%u fmp_q=012132 displist=015502 dspl=015556",
                 (unsigned)st_inum, (unsigned)R15_ST_WORDS);
        result = 1;
        goto done;
    }

    const int exact_bad =
        memcmp(cur, r15_bad_st_words, sizeof(r15_bad_st_words)) == 0;
    const int observed_variant = r16b_matches_observed_bad_variant(cur);

    if (!exact_bad && !observed_variant) {
        const uint32_t db = r15_first_diff(cur, r15_bad_st_words);
        const uint32_t dg = r15_first_diff(cur, r15_good_st_words);
        ESP_LOGW(TAG,
                 "SKIP system/st is not exact bad, exact observed one-word bad variant, or exact recovered-good; bad_diff=%06o obs=%06o exp_bad=%06o good_diff=%06o",
                 (unsigned)(010000u + db),
                 (unsigned)((db < R15_ST_WORDS ? cur[db] : 0u) & 0777777u),
                 (unsigned)((db < R15_ST_WORDS ? r15_bad_st_words[db] : 0u) & 0777777u),
                 (unsigned)(010000u + dg));
        result = 0;
        goto done;
    }

    if ((r15_bad_st_words[R16B_OBSERVED_DIFF_OFF] & 0777777u) !=
        R16B_BAD_REF_WORD) {
        ESP_LOGE(TAG, "internal lineage guard failed at 014006");
        result = -1;
        goto done;
    }

    const char *backup_path = exact_bad ? R15_BACKUP_PATH : R16B_VARIANT_BACKUP_PATH;
    if (exact_bad) {
        ESP_LOGI(TAG,
                 "MATCH exact known-bad system/st inum=%u words=%u bad{displist=014225 dspl=014301 fmp_q=014302}",
                 (unsigned)st_inum, (unsigned)R15_ST_WORDS);
    } else {
        ESP_LOGI(TAG,
                 "MATCH exact observed bad-layout variant inum=%u words=%u crash@014006=777751 bad{displist=014225 dspl=014301 fmp_q=014302}",
                 (unsigned)st_inum, (unsigned)R15_ST_WORDS);
    }

    if (!r16b_backup_original(backup_path, cur)) {
        ESP_LOGE(TAG, "backup failed/refused path=%s; disk left unchanged",
                 backup_path);
        result = -1;
        goto done;
    }

    uint32_t *original = (uint32_t *)malloc((size_t)R15_ST_WORDS * sizeof(uint32_t));
    if (original == NULL) {
        ESP_LOGE(TAG, "rollback-copy allocation failed; disk left unchanged");
        result = -1;
        goto done;
    }
    memcpy(original, cur, (size_t)R15_ST_WORDS * sizeof(uint32_t));

    if (!r15_write_file_words(fp, st_blocks, st_nblocks,
                              r15_good_st_words, R15_ST_WORDS) ||
        !r15_sync(fp)) {
        ESP_LOGE(TAG, "write/sync failed; attempting exact rollback");
        if (r16b_restore_original(fp, st_blocks, st_nblocks, original))
            ESP_LOGW(TAG, "ROLLBACK PASS restored exact pre-R16B system/st");
        else
            ESP_LOGE(TAG, "ROLLBACK FAILED; inspect SD image before further booting");
        free(original);
        result = -1;
        goto done;
    }

    memset(cur, 0, (size_t)R15_ST_WORDS * sizeof(uint32_t));
    if (!r15_read_file_words(fp, st_inum, cur, R15_ST_WORDS, NULL, NULL) ||
        memcmp(cur, r15_good_st_words, sizeof(r15_good_st_words)) != 0) {
        ESP_LOGE(TAG, "post-write exact verification failed; attempting rollback");
        if (r16b_restore_original(fp, st_blocks, st_nblocks, original))
            ESP_LOGW(TAG, "ROLLBACK PASS restored exact pre-R16B system/st");
        else
            ESP_LOGE(TAG, "ROLLBACK FAILED; inspect SD image before further booting");
        free(original);
        result = -1;
        goto done;
    }
    free(original);

    ESP_LOGI(TAG,
             "PASS replaced proven bad-layout system/st with exact recovered working binary inum=%u words=%u good{fmp_q=012132 displist=015502 dspl=015556}",
             (unsigned)st_inum, (unsigned)R15_ST_WORDS);
    result = 2;

done:
    free(cur);
    if (fp != NULL)
        fclose(fp);
    return result;
}
