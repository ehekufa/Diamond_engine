/* Diamond Engine — asset and file helpers (internal). */
#ifndef DIAMOND_DFILES_H
#define DIAMOND_DFILES_H

#ifdef __cplusplus
extern "C" {
#endif

int   dfiles_install_assets(void *asset_manager);
const char *dfiles_last_error(void);
void *dfiles_read_asset(const char *path, int *out_size);   /* malloc'ed, NUL-terminated */
void *dfiles_read_file(const char *path, int *out_size);    /* malloc'ed, NUL-terminated */
int   dfiles_write_file(const char *path, const void *data, int size);

#ifdef __cplusplus
}
#endif
#endif
