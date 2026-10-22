/* File access for the game. Desktop: plain stdio. Android: the original bundle is read in
 * place from the APK through AAssetManager (paths "@app/..."), saves use stdio. */
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __ANDROID__
#include <android/asset_manager.h>
AAssetManager *g_assets;
#endif

struct VFile { FILE *f;
#ifdef __ANDROID__
    AAsset *a;
#endif
};
VFile *vfs_open(const char *path, const char *mode) {
#ifdef __ANDROID__
    if (!strncmp(path, "@app/", 5)) {
        if (strchr(mode, 'w') || strchr(mode, 'a')) return NULL;          /* the bundle is read-only, as on iOS */
        AAsset *a = AAssetManager_open(g_assets, path + 5, AASSET_MODE_RANDOM);
        if (!a) return NULL;
        VFile *v = calloc(1, sizeof *v); v->a = a; return v;
    }
#endif
    FILE *f = fopen(path, mode); if (!f) return NULL;
    VFile *v = calloc(1, sizeof *v); v->f = f; return v;
}
size_t vfs_read(VFile *v, void *d, size_t n) {
#ifdef __ANDROID__
    if (v->a) { int r = AAsset_read(v->a, d, n); return r < 0 ? 0 : (size_t)r; }
#endif
    return fread(d, 1, n, v->f);
}
size_t vfs_write(VFile *v, const void *s, size_t n) { return v->f ? fwrite(s, 1, n, v->f) : 0; }
int vfs_seek(VFile *v, long off, int wh) {
#ifdef __ANDROID__
    if (v->a) return AAsset_seek(v->a, off, wh) < 0 ? -1 : 0;
#endif
    return fseek(v->f, off, wh);
}
long vfs_tell(VFile *v) {
#ifdef __ANDROID__
    if (v->a) return (long)(AAsset_getLength(v->a) - AAsset_getRemainingLength(v->a));
#endif
    return ftell(v->f);
}
int vfs_close(VFile *v) {
    int r = 0;
#ifdef __ANDROID__
    if (v->a) AAsset_close(v->a); else
#endif
    r = fclose(v->f);
    free(v); return r;
}
long vfs_size(const char *path) {
#ifdef __ANDROID__
    if (!strncmp(path, "@app/", 5)) {
        AAsset *a = AAssetManager_open(g_assets, path + 5, AASSET_MODE_UNKNOWN);
        if (!a) return -1;
        long n = (long)AAsset_getLength(a); AAsset_close(a); return n;
    }
#endif
    struct stat st; if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return (long)st.st_size;
}
uint8_t *vfs_read_all(const char *path, long *size) {
    VFile *v = vfs_open(path, "rb"); if (!v) return NULL;
    vfs_seek(v, 0, SEEK_END); long n = vfs_tell(v); vfs_seek(v, 0, SEEK_SET);
    uint8_t *b = malloc(n + 1); size_t got = vfs_read(v, b, n); vfs_close(v); b[got] = 0;
    if (size) *size = (long)got;
    return b;
}
