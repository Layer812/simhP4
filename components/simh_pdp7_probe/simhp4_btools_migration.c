#define _POSIX_C_SOURCE 200809L
#include "simhp4_btools_migration.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"

/* SIMHP4_RELEASE_FINAL_BTOOLS_FIX
 *
 * Repair only the PDP-7 UNIX V0 B development environment packaging gap:
 *   - audit final self-hosted system/b, dmr/bi.s and dmr/bl.s read-only;
 *   - add exact upstream dmr/ops.s when absent;
 *   - correct b_readme from "bc" to current "b" command;
 *   - preserve dmr/op.s and all unrelated guest files.
 *
 * Upstream payload provenance:
 *   DoctorWkt/pdp7-unix commit 555eb30fc76b8fa29095d32eca9a43e9b1638288
 *
 * Expected guest sequence after migration:
 *   @ b hello.b hello.s
 *   @ as ops.s bl.s hello.s bi.s
 *   @ a.out
 *   Hello, World!
 *
 * The live UNIXV0.DSK is never edited in place.  A complete 8.3-safe temp
 * copy is patched and verified first; the original is then renamed to
 * BTOOLBAK.DSK before the verified temp is promoted to UNIXV0.DSK.
 */

static const char *TAG = "SIMHP4_BTOOLS";

#define BT_NUMBLOCKS           8000u
#define BT_WORDS_PER_BLOCK       64u
#define BT_BYTES_PER_WORD         4u
#define BT_FS_BASE_BYTES \
    (BT_NUMBLOCKS * BT_WORDS_PER_BLOCK * BT_BYTES_PER_WORD)
#define BT_IMAGE_BYTES       4096000u

#define BT_FIRST_INODE_BLOCK      2u
#define BT_NUM_INODE_BLOCKS     710u
#define BT_INODES_PER_BLOCK       5u
#define BT_INODE_SIZE            12u
#define BT_MAX_INUM \
    (BT_NUM_INODE_BLOCKS * BT_INODES_PER_BLOCK - 1u)

#define BT_I_FLAGS                0u
#define BT_I_DISKPS               1u
#define BT_I_UID                  8u
#define BT_I_NLKS                 9u
#define BT_I_SIZE                10u
#define BT_I_UNIQ                11u
#define BT_I_NUMBLKS              7u

#define BT_I_USED            0400000u
#define BT_I_LARGE           0200000u
#define BT_I_DIRECTORY       0000020u
#define BT_I_OWNERREAD       0000010u
#define BT_I_OWNERWRITE      0000004u
#define BT_I_WORLDREAD       0000002u
#define BT_FILE_PERMS \
    (BT_I_OWNERREAD | BT_I_OWNERWRITE | BT_I_WORLDREAD)

#define BT_DIRENT_SIZE             8u
#define BT_D_INUM                  0u
#define BT_D_NAME                  1u
#define BT_D_UNIQ                  5u

#define BT_SYS_NXFBLK              0u
#define BT_SYS_NFBLKS              1u
#define BT_SYS_FBLKS               2u
#define BT_SYS_FBLKS_COUNT        10u
#define BT_SYS_UNIQ               12u

#define BT_ROOT_INUM               4u
#define BT_FIRST_CREATABLE_INUM   17u
#define BT_MAX_FILE_BLOCKS       384u

#ifndef BT_TEMP_PATH
#define BT_TEMP_PATH    "/sdcard/BTOOLNEW.DSK"
#endif
#ifndef BT_BACKUP_PATH
#define BT_BACKUP_PATH  "/sdcard/BTOOLBAK.DSK"
#endif

#include "simhp4_btools_images.inc"

typedef struct {
    uint32_t inum;
    uint32_t uniq;
    uint32_t block;
    uint32_t off;
} bt_dirent_t;

typedef struct {
    uint32_t block;
    uint32_t off;
    int append;
} bt_dirslot_t;

static int bt_seek_word(FILE *fp, uint32_t block, uint32_t word_off)
{
    const uint64_t word_index = (uint64_t)block * BT_WORDS_PER_BLOCK + word_off;
    const uint64_t byte_off = (uint64_t)BT_FS_BASE_BYTES +
                              word_index * BT_BYTES_PER_WORD;
    if (block >= BT_NUMBLOCKS || word_off >= BT_WORDS_PER_BLOCK ||
        byte_off > 0x7fffffffu)
        return 0;
    return fseek(fp, (long)byte_off, SEEK_SET) == 0;
}

static int bt_read_word(FILE *fp, uint32_t block, uint32_t word_off, uint32_t *out)
{
    uint8_t b[4];
    if (!bt_seek_word(fp, block, word_off))
        return 0;
    if (fread(b, 1, sizeof(b), fp) != sizeof(b))
        return 0;
    *out = ((uint32_t)b[0] |
            ((uint32_t)b[1] << 8) |
            ((uint32_t)b[2] << 16) |
            ((uint32_t)b[3] << 24)) & 0777777u;
    return 1;
}

static int bt_write_word(FILE *fp, uint32_t block, uint32_t word_off, uint32_t v)
{
    const uint8_t b[4] = {
        (uint8_t)(v & 0xffu),
        (uint8_t)((v >> 8) & 0xffu),
        (uint8_t)((v >> 16) & 0xffu),
        (uint8_t)((v >> 24) & 0xffu),
    };
    if (!bt_seek_word(fp, block, word_off))
        return 0;
    return fwrite(b, 1, sizeof(b), fp) == sizeof(b);
}

static int bt_sync(FILE *fp)
{
    if (fflush(fp) != 0)
        return 0;
    const int fd = fileno(fp);
    return fd < 0 || fsync(fd) == 0;
}

static int bt_inode_pos(uint32_t inum, uint32_t *block, uint32_t *off)
{
    if (inum > BT_MAX_INUM)
        return 0;
    *block = BT_FIRST_INODE_BLOCK + inum / BT_INODES_PER_BLOCK;
    *off = BT_INODE_SIZE * (inum % BT_INODES_PER_BLOCK);
    return *off + BT_INODE_SIZE <= BT_WORDS_PER_BLOCK;
}

static int bt_read_inode_word(FILE *fp, uint32_t inum, uint32_t field, uint32_t *out)
{
    uint32_t block, off;
    if (field >= BT_INODE_SIZE || !bt_inode_pos(inum, &block, &off))
        return 0;
    return bt_read_word(fp, block, off + field, out);
}

static int bt_write_inode_word(FILE *fp, uint32_t inum, uint32_t field, uint32_t v)
{
    uint32_t block, off;
    if (field >= BT_INODE_SIZE || !bt_inode_pos(inum, &block, &off))
        return 0;
    return bt_write_word(fp, block, off + field, v);
}

static int bt_get_file_blocks(FILE *fp,
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

    if (!bt_inode_pos(inum, &iblock, &ioff) ||
        !bt_read_word(fp, iblock, ioff + BT_I_FLAGS, &flags) ||
        !bt_read_word(fp, iblock, ioff + BT_I_SIZE, &size))
        return 0;
    if ((flags & BT_I_USED) == 0)
        return 0;

    if (flags & BT_I_LARGE) {
        for (uint32_t p = 0; p < BT_I_NUMBLKS; ++p) {
            uint32_t indirect = 0;
            if (!bt_read_word(fp, iblock, ioff + BT_I_DISKPS + p, &indirect))
                return 0;
            if (indirect == 0)
                continue;
            if (indirect >= BT_NUMBLOCKS)
                return 0;
            for (uint32_t j = 0; j < BT_WORDS_PER_BLOCK; ++j) {
                uint32_t data = 0;
                if (!bt_read_word(fp, indirect, j, &data))
                    return 0;
                if (data == 0)
                    continue;
                if (data >= BT_NUMBLOCKS || n >= cap)
                    return 0;
                blocks[n++] = data;
            }
        }
    } else {
        for (uint32_t p = 0; p < BT_I_NUMBLKS; ++p) {
            uint32_t data = 0;
            if (!bt_read_word(fp, iblock, ioff + BT_I_DISKPS + p, &data))
                return 0;
            if (data == 0)
                continue;
            if (data >= BT_NUMBLOCKS || n >= cap)
                return 0;
            blocks[n++] = data;
        }
    }

    if (n * BT_WORDS_PER_BLOCK < size)
        return 0;

    *size_words = size;
    *nblocks = n;
    if (flags_out)
        *flags_out = flags;
    return 1;
}

static void bt_decode_name(const uint32_t words[4], char out[9])
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

static void bt_encode_name(const char *name, uint32_t out[4])
{
    char tmp[8];
    memset(tmp, ' ', sizeof(tmp));
    const size_t n = strlen(name) < sizeof(tmp) ? strlen(name) : sizeof(tmp);
    memcpy(tmp, name, n);
    for (size_t i = 0; i < 4; ++i) {
        out[i] = ((uint32_t)(uint8_t)tmp[i * 2] << 9) |
                 (uint32_t)(uint8_t)tmp[i * 2 + 1];
    }
}

static int bt_find_in_dir(FILE *fp, uint32_t dir_inum,
                          const char *want, bt_dirent_t *found)
{
    uint32_t blocks[BT_MAX_FILE_BLOCKS];
    uint32_t size = 0, flags = 0;
    size_t nblocks = 0;
    uint32_t seen = 0;

    if (!bt_get_file_blocks(fp, dir_inum, blocks, BT_MAX_FILE_BLOCKS,
                            &size, &nblocks, &flags) ||
        (flags & BT_I_DIRECTORY) == 0 || (size % BT_DIRENT_SIZE) != 0)
        return 0;

    for (size_t bi = 0; bi < nblocks && seen < size; ++bi) {
        for (uint32_t off = 0; off < BT_WORDS_PER_BLOCK && seen < size;
             off += BT_DIRENT_SIZE) {
            uint32_t inum = 0, uniq = 0, nw[4];
            char name[9];
            if (!bt_read_word(fp, blocks[bi], off + BT_D_INUM, &inum) ||
                !bt_read_word(fp, blocks[bi], off + BT_D_UNIQ, &uniq))
                return 0;
            for (uint32_t k = 0; k < 4; ++k) {
                if (!bt_read_word(fp, blocks[bi], off + BT_D_NAME + k, &nw[k]))
                    return 0;
            }
            bt_decode_name(nw, name);
            if (inum != 0 && strcmp(name, want) == 0) {
                if (found) {
                    found->inum = inum;
                    found->uniq = uniq;
                    found->block = blocks[bi];
                    found->off = off;
                }
                return 1;
            }
            seen += BT_DIRENT_SIZE;
        }
    }
    return 0;
}

static int bt_find_dir_slot(FILE *fp, uint32_t dir_inum, bt_dirslot_t *slot)
{
    uint32_t blocks[BT_MAX_FILE_BLOCKS];
    uint32_t size = 0, flags = 0;
    size_t nblocks = 0;
    uint32_t seen = 0;

    if (!slot || !bt_get_file_blocks(fp, dir_inum, blocks, BT_MAX_FILE_BLOCKS,
                                     &size, &nblocks, &flags) ||
        (flags & BT_I_DIRECTORY) == 0 || (size % BT_DIRENT_SIZE) != 0)
        return 0;

    /* Reuse a deleted directory entry first, matching guest dslot semantics. */
    for (size_t bi = 0; bi < nblocks && seen < size; ++bi) {
        for (uint32_t off = 0; off < BT_WORDS_PER_BLOCK && seen < size;
             off += BT_DIRENT_SIZE) {
            uint32_t inum = 0;
            if (!bt_read_word(fp, blocks[bi], off, &inum))
                return 0;
            if (inum == 0) {
                slot->block = blocks[bi];
                slot->off = off;
                slot->append = 0;
                return 1;
            }
            seen += BT_DIRENT_SIZE;
        }
    }

    /* Append only when the directory already owns capacity for another slot.
     * Do not allocate a new directory block in this release fix. */
    const uint32_t capacity = (uint32_t)(nblocks * BT_WORDS_PER_BLOCK);
    if (size + BT_DIRENT_SIZE > capacity)
        return 0;
    const uint32_t bi = size / BT_WORDS_PER_BLOCK;
    const uint32_t off = size % BT_WORDS_PER_BLOCK;
    if (bi >= nblocks || off + BT_DIRENT_SIZE > BT_WORDS_PER_BLOCK)
        return 0;
    slot->block = blocks[bi];
    slot->off = off;
    slot->append = 1;
    return 1;
}

static int bt_file_matches(FILE *fp, uint32_t inum,
                           const uint32_t *expected, uint32_t expected_words,
                           uint32_t *first_diff, uint32_t *observed)
{
    uint32_t blocks[BT_MAX_FILE_BLOCKS];
    uint32_t size = 0;
    size_t nblocks = 0;
    uint32_t pos = 0;

    if (first_diff)
        *first_diff = 0777777u;
    if (observed)
        *observed = 0;

    if (!bt_get_file_blocks(fp, inum, blocks, BT_MAX_FILE_BLOCKS,
                            &size, &nblocks, NULL) || size != expected_words) {
        if (first_diff)
            *first_diff = size;
        return 0;
    }

    for (size_t bi = 0; bi < nblocks && pos < size; ++bi) {
        for (uint32_t off = 0; off < BT_WORDS_PER_BLOCK && pos < size; ++off) {
            uint32_t v = 0;
            if (!bt_read_word(fp, blocks[bi], off, &v))
                return 0;
            if (v != (expected[pos] & 0777777u)) {
                if (first_diff)
                    *first_diff = pos;
                if (observed)
                    *observed = v;
                return 0;
            }
            ++pos;
        }
    }
    return pos == size;
}

static int bt_write_existing_file(FILE *fp, uint32_t inum,
                                  const uint32_t *src, uint32_t words)
{
    uint32_t blocks[BT_MAX_FILE_BLOCKS];
    uint32_t size = 0;
    size_t nblocks = 0;
    uint32_t pos = 0;
    if (!bt_get_file_blocks(fp, inum, blocks, BT_MAX_FILE_BLOCKS,
                            &size, &nblocks, NULL) || size != words)
        return 0;
    for (size_t bi = 0; bi < nblocks && pos < words; ++bi) {
        for (uint32_t off = 0; off < BT_WORDS_PER_BLOCK; ++off) {
            const uint32_t v = pos < words ? src[pos++] : 0u;
            if (!bt_write_word(fp, blocks[bi], off, v))
                return 0;
        }
    }
    return pos == words;
}

static int bt_find_free_inode(FILE *fp, uint32_t *inum_out)
{
    for (uint32_t inum = BT_FIRST_CREATABLE_INUM; inum <= BT_MAX_INUM; ++inum) {
        uint32_t flags = 0;
        if (!bt_read_inode_word(fp, inum, BT_I_FLAGS, &flags))
            return 0;
        if ((flags & BT_I_USED) == 0) {
            *inum_out = inum;
            return 1;
        }
    }
    return 0;
}

/* Mirror UNIX V0 s4.s alloc exactly against the on-disk system block.
 * s.fblks is a 10-entry cache in block 0; when empty, s.nxfblk points to a
 * free-list block whose word 0 is the next link and words 1..9 are free. */
static int bt_alloc_block(FILE *fp, uint32_t *block_out)
{
    uint32_t nf = 0;
    if (!bt_read_word(fp, 0, BT_SYS_NFBLKS, &nf) || nf > BT_SYS_FBLKS_COUNT)
        return 0;

    if (nf == 0) {
        uint32_t head = 0, next = 0;
        if (!bt_read_word(fp, 0, BT_SYS_NXFBLK, &head) ||
            head == 0 || head >= BT_NUMBLOCKS ||
            !bt_read_word(fp, head, 0, &next))
            return 0;
        if (!bt_write_word(fp, 0, BT_SYS_FBLKS, head) ||
            !bt_write_word(fp, 0, BT_SYS_NXFBLK, next))
            return 0;
        for (uint32_t i = 1; i < BT_SYS_FBLKS_COUNT; ++i) {
            uint32_t v = 0;
            if (!bt_read_word(fp, head, i, &v) ||
                !bt_write_word(fp, 0, BT_SYS_FBLKS + i, v))
                return 0;
        }
        if (!bt_write_word(fp, 0, BT_SYS_NFBLKS, BT_SYS_FBLKS_COUNT))
            return 0;
        nf = BT_SYS_FBLKS_COUNT;
    }

    --nf;
    uint32_t v = 0;
    if (!bt_read_word(fp, 0, BT_SYS_FBLKS + nf, &v) ||
        v == 0 || v >= BT_NUMBLOCKS ||
        !bt_write_word(fp, 0, BT_SYS_NFBLKS, nf))
        return 0;
    *block_out = v;
    return 1;
}

static int bt_zero_block(FILE *fp, uint32_t block)
{
    for (uint32_t i = 0; i < BT_WORDS_PER_BLOCK; ++i) {
        if (!bt_write_word(fp, block, i, 0))
            return 0;
    }
    return 1;
}

static int bt_add_ops_file(FILE *fp, uint32_t dmr_inum)
{
    bt_dirslot_t slot;
    uint32_t new_inum = 0, dmr_uid = 0, dmr_size = 0;
    uint32_t uniq = 0;
    const uint32_t words = BT_OPS_WORDS_COUNT;
    const uint32_t data_count = (words + BT_WORDS_PER_BLOCK - 1u) / BT_WORDS_PER_BLOCK;
    const uint32_t ind_count = (data_count + BT_WORDS_PER_BLOCK - 1u) / BT_WORDS_PER_BLOCK;
    uint32_t data_blocks[BT_MAX_FILE_BLOCKS];
    uint32_t ind_blocks[BT_I_NUMBLKS];

    if (ind_count == 0 || ind_count > BT_I_NUMBLKS || data_count > BT_MAX_FILE_BLOCKS ||
        !bt_find_dir_slot(fp, dmr_inum, &slot) ||
        !bt_find_free_inode(fp, &new_inum) ||
        !bt_read_inode_word(fp, dmr_inum, BT_I_UID, &dmr_uid) ||
        !bt_read_inode_word(fp, dmr_inum, BT_I_SIZE, &dmr_size) ||
        !bt_read_word(fp, 0, BT_SYS_UNIQ, &uniq))
        return 0;

    uniq = (uniq + 1u) & 0777777u;

    for (uint32_t i = 0; i < data_count; ++i) {
        if (!bt_alloc_block(fp, &data_blocks[i]) || !bt_zero_block(fp, data_blocks[i]))
            return 0;
    }
    for (uint32_t i = 0; i < ind_count; ++i) {
        if (!bt_alloc_block(fp, &ind_blocks[i]) || !bt_zero_block(fp, ind_blocks[i]))
            return 0;
    }

    uint32_t pos = 0;
    for (uint32_t bi = 0; bi < data_count; ++bi) {
        for (uint32_t off = 0; off < BT_WORDS_PER_BLOCK; ++off) {
            const uint32_t v = pos < words ? bt_ops_words[pos++] : 0u;
            if (!bt_write_word(fp, data_blocks[bi], off, v))
                return 0;
        }
    }
    if (pos != words)
        return 0;

    for (uint32_t i = 0; i < data_count; ++i) {
        const uint32_t ib = i / BT_WORDS_PER_BLOCK;
        const uint32_t io = i % BT_WORDS_PER_BLOCK;
        if (!bt_write_word(fp, ind_blocks[ib], io, data_blocks[i]))
            return 0;
    }

    /* Fill the new inode exactly as guest icreat + first write would leave it. */
    for (uint32_t f = 0; f < BT_INODE_SIZE; ++f) {
        if (!bt_write_inode_word(fp, new_inum, f, 0))
            return 0;
    }
    if (!bt_write_inode_word(fp, new_inum, BT_I_FLAGS,
                             BT_I_USED | BT_I_LARGE | BT_FILE_PERMS) ||
        !bt_write_inode_word(fp, new_inum, BT_I_UID, dmr_uid) ||
        !bt_write_inode_word(fp, new_inum, BT_I_NLKS, 0777777u) ||
        !bt_write_inode_word(fp, new_inum, BT_I_SIZE, words) ||
        !bt_write_inode_word(fp, new_inum, BT_I_UNIQ, uniq))
        return 0;
    for (uint32_t i = 0; i < ind_count; ++i) {
        if (!bt_write_inode_word(fp, new_inum, BT_I_DISKPS + i, ind_blocks[i]))
            return 0;
    }

    uint32_t name_words[4];
    bt_encode_name("ops.s", name_words);
    for (uint32_t i = 0; i < BT_DIRENT_SIZE; ++i) {
        if (!bt_write_word(fp, slot.block, slot.off + i, 0))
            return 0;
    }
    if (!bt_write_word(fp, slot.block, slot.off + BT_D_INUM, new_inum))
        return 0;
    for (uint32_t i = 0; i < 4; ++i) {
        if (!bt_write_word(fp, slot.block, slot.off + BT_D_NAME + i, name_words[i]))
            return 0;
    }
    if (!bt_write_word(fp, slot.block, slot.off + BT_D_UNIQ, uniq) ||
        !bt_write_word(fp, 0, BT_SYS_UNIQ, uniq))
        return 0;

    if (slot.append) {
        if (!bt_write_inode_word(fp, dmr_inum, BT_I_SIZE,
                                 dmr_size + BT_DIRENT_SIZE))
            return 0;
    }

    ESP_LOGI(TAG,
             "prepared exact dmr/ops.s inum=%u words=%u data_blocks=%u indirect_blocks=%u dir_slot=%u/%u append=%d",
             (unsigned)new_inum, (unsigned)words, (unsigned)data_count,
             (unsigned)ind_count, (unsigned)slot.block, (unsigned)slot.off,
             slot.append);
    return 1;
}

static int bt_stat_regular_size(const char *path, off_t *size_out)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    if (size_out)
        *size_out = st.st_size;
    return 1;
}

static int bt_copy_disk(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in)
        return 0;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }
    uint8_t *buf = (uint8_t *)malloc(16384u);
    if (!buf) {
        fclose(in);
        fclose(out);
        return 0;
    }
    size_t total = 0;
    int ok = 1;
    for (;;) {
        const size_t n = fread(buf, 1, 16384u, in);
        if (n != 0) {
            if (fwrite(buf, 1, n, out) != n) {
                ok = 0;
                break;
            }
            total += n;
        }
        if (n < 16384u) {
            if (ferror(in))
                ok = 0;
            break;
        }
    }
    free(buf);
    if (ok)
        ok = bt_sync(out);
    if (fclose(out) != 0)
        ok = 0;
    fclose(in);
    return ok && total == BT_IMAGE_BYTES;
}

typedef struct {
    int b_ok;
    int bi_ok;
    int bl_ok;
    int readme_old;
    int readme_new;
    int ops_absent;
    int ops_exact;
    uint32_t system_inum;
    uint32_t dmr_inum;
    uint32_t b_inum;
    uint32_t bi_inum;
    uint32_t bl_inum;
    uint32_t readme_inum;
    uint32_t ops_inum;
} bt_audit_t;

static int bt_audit(FILE *fp, bt_audit_t *a, int log_detail)
{
    bt_dirent_t de;
    uint32_t diff = 0, obs = 0;
    memset(a, 0, sizeof(*a));

    if (!bt_find_in_dir(fp, BT_ROOT_INUM, "system", &de)) {
        if (log_detail) ESP_LOGW(TAG, "AUDIT root/system missing");
        return 0;
    }
    a->system_inum = de.inum;
    if (!bt_find_in_dir(fp, BT_ROOT_INUM, "dmr", &de)) {
        if (log_detail) ESP_LOGW(TAG, "AUDIT root/dmr missing");
        return 0;
    }
    a->dmr_inum = de.inum;

    if (bt_find_in_dir(fp, a->system_inum, "b", &de)) {
        a->b_inum = de.inum;
        a->b_ok = bt_file_matches(fp, de.inum, bt_expected_b_words,
                                  BT_EXPECTED_B_WORDS_COUNT, &diff, &obs);
        if (log_detail && !a->b_ok)
            ESP_LOGW(TAG, "AUDIT MISMATCH system/b inum=%u first_diff=%u obs=%06o",
                     (unsigned)de.inum, (unsigned)diff, (unsigned)obs);
    } else if (log_detail) {
        ESP_LOGW(TAG, "AUDIT system/b missing");
    }

    if (bt_find_in_dir(fp, a->dmr_inum, "bi.s", &de)) {
        a->bi_inum = de.inum;
        a->bi_ok = bt_file_matches(fp, de.inum, bt_expected_bi_words,
                                   BT_EXPECTED_BI_WORDS_COUNT, &diff, &obs);
        if (log_detail && !a->bi_ok)
            ESP_LOGW(TAG, "AUDIT MISMATCH dmr/bi.s inum=%u first_diff=%u obs=%06o",
                     (unsigned)de.inum, (unsigned)diff, (unsigned)obs);
    } else if (log_detail) {
        ESP_LOGW(TAG, "AUDIT dmr/bi.s missing");
    }

    if (bt_find_in_dir(fp, a->dmr_inum, "bl.s", &de)) {
        a->bl_inum = de.inum;
        a->bl_ok = bt_file_matches(fp, de.inum, bt_expected_bl_words,
                                   BT_EXPECTED_BL_WORDS_COUNT, &diff, &obs);
        if (log_detail && !a->bl_ok)
            ESP_LOGW(TAG, "AUDIT MISMATCH dmr/bl.s inum=%u first_diff=%u obs=%06o",
                     (unsigned)de.inum, (unsigned)diff, (unsigned)obs);
    } else if (log_detail) {
        ESP_LOGW(TAG, "AUDIT dmr/bl.s missing");
    }

    if (bt_find_in_dir(fp, a->dmr_inum, "b_readme", &de)) {
        a->readme_inum = de.inum;
        a->readme_old = bt_file_matches(fp, de.inum, bt_old_readme_words,
                                        BT_OLD_README_WORDS_COUNT, NULL, NULL);
        a->readme_new = bt_file_matches(fp, de.inum, bt_new_readme_words,
                                        BT_NEW_README_WORDS_COUNT, NULL, NULL);
        if (log_detail && !a->readme_old && !a->readme_new)
            ESP_LOGW(TAG, "AUDIT MISMATCH dmr/b_readme inum=%u is neither exact old nor exact corrected text",
                     (unsigned)de.inum);
    } else if (log_detail) {
        ESP_LOGW(TAG, "AUDIT dmr/b_readme missing");
    }

    if (bt_find_in_dir(fp, a->dmr_inum, "ops.s", &de)) {
        a->ops_inum = de.inum;
        a->ops_exact = bt_file_matches(fp, de.inum, bt_ops_words,
                                       BT_OPS_WORDS_COUNT, &diff, &obs);
        if (log_detail && !a->ops_exact)
            ESP_LOGW(TAG, "AUDIT MISMATCH dmr/ops.s inum=%u first_diff=%u obs=%06o; preserving unknown file",
                     (unsigned)de.inum, (unsigned)diff, (unsigned)obs);
    } else {
        a->ops_absent = 1;
    }

    if (log_detail) {
        ESP_LOGI(TAG,
                 "AUDIT system/b=%s dmr/bi.s=%s dmr/bl.s=%s b_readme=%s ops.s=%s",
                 a->b_ok ? "FINAL" : "MISMATCH",
                 a->bi_ok ? "FINAL" : "MISMATCH",
                 a->bl_ok ? "FINAL" : "MISMATCH",
                 a->readme_new ? "FINAL" : (a->readme_old ? "OLD_BC" : "MISMATCH"),
                 a->ops_exact ? "FINAL" : (a->ops_absent ? "ABSENT" : "MISMATCH"));
    }
    return 1;
}

static int bt_audit_allows_write(const bt_audit_t *a)
{
    return a->b_ok && a->bi_ok && a->bl_ok &&
           (a->readme_old || a->readme_new) &&
           (a->ops_absent || a->ops_exact);
}

static int bt_audit_is_final(const bt_audit_t *a)
{
    return a->b_ok && a->bi_ok && a->bl_ok && a->readme_new && a->ops_exact;
}

static int bt_apply_to_temp(const char *path)
{
    FILE *fp = fopen(path, "r+b");
    if (!fp)
        return 0;
    bt_audit_t a;
    int ok = bt_audit(fp, &a, 0) && bt_audit_allows_write(&a);
    if (!ok) {
        fclose(fp);
        return 0;
    }

    if (a.readme_old) {
        ok = bt_write_existing_file(fp, a.readme_inum,
                                    bt_new_readme_words,
                                    BT_NEW_README_WORDS_COUNT);
        if (ok)
            ESP_LOGI(TAG, "prepared corrected dmr/b_readme: bc -> b");
    }
    if (ok && a.ops_absent)
        ok = bt_add_ops_file(fp, a.dmr_inum);
    if (ok)
        ok = bt_sync(fp);
    if (fclose(fp) != 0)
        ok = 0;
    return ok;
}

int simhp4_btools_migrate(const char *disk_path)
{
    if (!disk_path)
        return -1;

    off_t live_size = 0;
    if (!bt_stat_regular_size(disk_path, &live_size) || live_size != BT_IMAGE_BYTES) {
        ESP_LOGW(TAG, "SKIP disk path/size mismatch path=%s size=%ld expected=%u",
                 disk_path, (long)live_size, (unsigned)BT_IMAGE_BYTES);
        return 0;
    }

    FILE *live = fopen(disk_path, "rb");
    if (!live) {
        ESP_LOGE(TAG, "open live disk failed path=%s errno=%d (%s)",
                 disk_path, errno, strerror(errno));
        return -1;
    }
    bt_audit_t audit;
    const int audit_ok = bt_audit(live, &audit, 1);
    fclose(live);
    if (!audit_ok || !bt_audit_allows_write(&audit)) {
        ESP_LOGW(TAG, "NO B-TOOLS WRITE: read-only audit did not match exact final B lineage");
        return 0;
    }
    if (bt_audit_is_final(&audit)) {
        ESP_LOGI(TAG, "PASS B-tools already final: b + bi.s + bl.s + ops.s + corrected b_readme");
        return 1;
    }

    off_t backup_size = 0;
    if (bt_stat_regular_size(BT_BACKUP_PATH, &backup_size)) {
        ESP_LOGE(TAG,
                 "backup already exists while live disk still needs migration; refusing overwrite path=%s size=%ld",
                 BT_BACKUP_PATH, (long)backup_size);
        return -1;
    }

    off_t temp_size = 0;
    if (bt_stat_regular_size(BT_TEMP_PATH, &temp_size)) {
        FILE *tmp = fopen(BT_TEMP_PATH, "rb");
        bt_audit_t ta;
        const int temp_ready = tmp && bt_audit(tmp, &ta, 1) && bt_audit_is_final(&ta);
        if (tmp) fclose(tmp);
        if (!temp_ready) {
            ESP_LOGE(TAG,
                     "existing temp is not exact verified final; preserving for evidence and refusing overwrite path=%s",
                     BT_TEMP_PATH);
            return -1;
        }
        ESP_LOGI(TAG, "PASS reusing already-verified prepared temp path=%s", BT_TEMP_PATH);
    } else {
        ESP_LOGI(TAG, "creating full transactional temp copy path=%s", BT_TEMP_PATH);
        if (!bt_copy_disk(disk_path, BT_TEMP_PATH)) {
            ESP_LOGE(TAG, "temp copy failed; live disk unchanged path=%s", disk_path);
            return -1;
        }
        if (!bt_apply_to_temp(BT_TEMP_PATH)) {
            ESP_LOGE(TAG, "temp patch failed; live disk unchanged; preserving temp path=%s", BT_TEMP_PATH);
            return -1;
        }
        FILE *tmp = fopen(BT_TEMP_PATH, "rb");
        bt_audit_t ta;
        const int verified = tmp && bt_audit(tmp, &ta, 1) && bt_audit_is_final(&ta);
        if (tmp) fclose(tmp);
        if (!verified) {
            ESP_LOGE(TAG, "post-patch exact verification failed; live disk unchanged; preserving temp");
            return -1;
        }
        ESP_LOGI(TAG, "PASS transactional temp exact verification");
    }

    if (rename(disk_path, BT_BACKUP_PATH) != 0) {
        ESP_LOGE(TAG, "backup rename failed; live disk remains original errno=%d (%s)",
                 errno, strerror(errno));
        return -1;
    }
    ESP_LOGI(TAG, "PASS original disk preserved path=%s", BT_BACKUP_PATH);

    if (rename(BT_TEMP_PATH, disk_path) != 0) {
        const int e = errno;
        ESP_LOGE(TAG, "promote temp failed errno=%d (%s); attempting rollback", e, strerror(e));
        if (rename(BT_BACKUP_PATH, disk_path) == 0)
            ESP_LOGW(TAG, "ROLLBACK PASS restored original UNIXV0.DSK");
        else
            ESP_LOGE(TAG, "ROLLBACK FAILED; original remains at %s", BT_BACKUP_PATH);
        return -1;
    }

    live = fopen(disk_path, "rb");
    bt_audit_t final_audit;
    const int final_ok = live && bt_audit(live, &final_audit, 1) &&
                         bt_audit_is_final(&final_audit);
    if (live) fclose(live);
    if (!final_ok) {
        ESP_LOGE(TAG,
                 "CRITICAL post-promote verification failed; preserved original backup path=%s",
                 BT_BACKUP_PATH);
        return -1;
    }

    ESP_LOGI(TAG,
             "PASS B-tools migration committed; original preserved at %s; command: b hello.b hello.s; as ops.s bl.s hello.s bi.s; a.out",
             BT_BACKUP_PATH);
    return 2;
}
