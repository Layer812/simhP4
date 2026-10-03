#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns:
 *   2 = migration committed (ops.s added and/or b_readme corrected)
 *   1 = already exact / no write required
 *   0 = read-only audit mismatch or unsupported layout; no write performed
 *  -1 = I/O or transactional failure; original disk preserved when possible
 */
int simhp4_btools_migrate(const char *disk_path);

#ifdef __cplusplus
}
#endif
