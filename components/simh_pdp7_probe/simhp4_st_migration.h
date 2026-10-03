#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Returns:
 *  2: exact known-bad system/st replaced and verified
 *  1: exact recovered-good system/st already present
 *  0: no write (missing/unrecognized layout)
 * -1: migration/backup/write error
 */
int simhp4_st_migrate_bad_layout(const char *disk_path);

#ifdef __cplusplus
}
#endif
