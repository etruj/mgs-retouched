/* Tables extracted from the original binary by tools/mk_image.py. Addresses are the
 * original vm addresses; the image is loaded at those same addresses in wasm memory. */
#ifndef IMAGE_META_H
#define IMAGE_META_H
typedef struct { unsigned addr; const char *sym; unsigned addend; } ImgBind;
typedef struct { const char *sel, *types; unsigned imp; } ImgMethod;
/* a pointer field is either a local image address or a bound external symbol */
typedef struct {
    const char *name; unsigned addr, meta;
    unsigned sup; const char *sup_sym;
    unsigned meta_isa; const char *meta_isa_sym;
    unsigned meta_sup; const char *meta_sup_sym;
    unsigned istart, isize;
    const ImgMethod *imethods, *cmethods;
} ImgClass;
typedef struct { const char *name; unsigned cls; const char *cls_sym; const ImgMethod *imethods, *cmethods; } ImgCategory;
extern const ImgBind img_binds[]; extern const int img_nbinds;
extern const ImgClass img_classes[]; extern const int img_nclasses;
extern const ImgCategory img_categories[]; extern const int img_ncategories;
#endif
