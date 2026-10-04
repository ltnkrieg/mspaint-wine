/* Clipboard and text-tool fixes for Paint under Wine. Built into featstage.dll.
 *
 * 1. PNG clipboard bridge.
 *
 * Wine maps the X clipboard target image/png to the registered clipboard
 * format "PNG" only. It does not convert between PNG and CF_DIB. Paint
 * reads and writes CF_BITMAP/CF_DIB only, and many X programs (Chromium,
 * Electron, Firefox, screenshot tools) read and write image/png only.
 *
 * This file patches the import tables of mspaint.exe and mfc42u.dll:
 * - OleSetClipboard: Paint's data object is wrapped, so the clipboard also
 *   offers "PNG" (encoded from Paint's bitmap on request).
 * - OleGetClipboard / IsClipboardFormatAvailable: when the clipboard has
 *   PNG but no bitmap, Paint sees CF_BITMAP/CF_DIB decoded from the PNG.
 * - OleIsCurrentClipboard: maps Paint's own data object to the wrapper, so
 *   MFC still knows that it owns the clipboard (flush on exit).
 * - OleFlushClipboard: at exit, the image is handed to xclip so that it stays
 *   on the X clipboard after Wine shuts down.
 * PNG encode and decode use GDI+.
 *
 * 2. EM_FORMATRANGE for the text tool (see format_range below).
 */
#define COBJMACROS
#include <windows.h>
#include <ole2.h>

void pclip_init(void);

static UINT cf_png;

/* ---- GDI+ flat API, loaded on first use ---- */
typedef struct { UINT32 GdiplusVersion; void *DebugEventCallback; BOOL SuppressBackgroundThread; BOOL SuppressExternalCodecs; } GdiplusStartupInput;
typedef struct { UINT Width, Height; INT Stride; INT PixelFormat; void *Scan0; UINT_PTR Reserved; } GpBitmapData;
typedef struct { INT X, Y, Width, Height; } GpRect;
#define PixelFormat32bppARGB 0x26200A
#define PixelFormat32bppRGB 0x22009
#define ImageLockModeRead 1
static int (WINAPI *pGdiplusStartup)(ULONG_PTR *, const GdiplusStartupInput *, void *);
static int (WINAPI *pGdipCreateBitmapFromStream)(IStream *, void **);
static int (WINAPI *pGdipCreateBitmapFromScan0)(INT, INT, INT, INT, BYTE *, void **);
static int (WINAPI *pGdipGetImageWidth)(void *, UINT *);
static int (WINAPI *pGdipGetImageHeight)(void *, UINT *);
static int (WINAPI *pGdipBitmapLockBits)(void *, const GpRect *, UINT, INT, GpBitmapData *);
static int (WINAPI *pGdipBitmapUnlockBits)(void *, GpBitmapData *);
static int (WINAPI *pGdipSaveImageToStream)(void *, IStream *, const CLSID *, const void *);
static int (WINAPI *pGdipDisposeImage)(void *);
static const CLSID CLSID_PngEncoder = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };

static BOOL gdiplus_ready(void)
{
    static int state; /* 0 = not tried, 1 = ready, 2 = failed */
    if (state) return state == 1;
    state = 2;
    HMODULE m = LoadLibraryW(L"gdiplus.dll");
    if (!m) return FALSE;
#define LOAD(name) if (!(p##name = (void *)GetProcAddress(m, #name))) return FALSE
    LOAD(GdiplusStartup); LOAD(GdipCreateBitmapFromStream); LOAD(GdipCreateBitmapFromScan0);
    LOAD(GdipGetImageWidth); LOAD(GdipGetImageHeight); LOAD(GdipBitmapLockBits);
    LOAD(GdipBitmapUnlockBits); LOAD(GdipSaveImageToStream); LOAD(GdipDisposeImage);
#undef LOAD
    GdiplusStartupInput in = { 1, NULL, FALSE, FALSE };
    ULONG_PTR token;
    if (pGdiplusStartup(&token, &in, NULL)) return FALSE;
    state = 1;
    return TRUE;
}

/* ---- pixel conversion ---- */

/* 32bpp top-down BGRA pixels, alpha 255. */
typedef struct { int w, h; DWORD *px; } Pixels;

static BOOL pixels_from_dib(const BITMAPINFO *bmi, SIZE_T size, Pixels *out)
{
    const BITMAPINFOHEADER *bh = &bmi->bmiHeader;
    if (size < sizeof(*bh) || bh->biSize < sizeof(*bh) || bh->biWidth <= 0 || bh->biHeight == 0) return FALSE;
    int w = bh->biWidth, h = bh->biHeight < 0 ? -bh->biHeight : bh->biHeight;
    SIZE_T colors = bh->biClrUsed ? bh->biClrUsed : (bh->biBitCount <= 8 ? (SIZE_T)1 << bh->biBitCount : 0);
    SIZE_T off = bh->biSize + colors * sizeof(RGBQUAD);
    if (bh->biCompression == BI_BITFIELDS && bh->biSize == sizeof(BITMAPINFOHEADER)) off += 3 * sizeof(DWORD);
    if (off >= size) return FALSE;
    BITMAPINFO dst = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB } };
    void *bits;
    HDC dc = GetDC(NULL);
    HBITMAP sec = CreateDIBSection(dc, &dst, DIB_RGB_COLORS, &bits, NULL, 0);
    BOOL ok = sec && SetDIBits(dc, sec, 0, h, (const BYTE *)bmi + off, bmi, DIB_RGB_COLORS) == h;
    ReleaseDC(NULL, dc);
    if (ok) {
        out->w = w; out->h = h;
        out->px = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)w * h * 4);
        ok = out->px != NULL;
        if (ok) for (SIZE_T i = 0; i < (SIZE_T)w * h; i++) out->px[i] = ((DWORD *)bits)[i] | 0xff000000;
    }
    if (sec) DeleteObject(sec);
    return ok;
}

static BOOL pixels_from_hbitmap(HBITMAP hbm, Pixels *out)
{
    BITMAP bm;
    if (!GetObjectW(hbm, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return FALSE;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), bm.bmWidth, -bm.bmHeight, 1, 32, BI_RGB } };
    out->w = bm.bmWidth; out->h = bm.bmHeight;
    out->px = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)out->w * out->h * 4);
    if (!out->px) return FALSE;
    HDC dc = GetDC(NULL);
    BOOL ok = GetDIBits(dc, hbm, 0, out->h, out->px, &bi, DIB_RGB_COLORS) == out->h;
    ReleaseDC(NULL, dc);
    if (ok) for (SIZE_T i = 0; i < (SIZE_T)out->w * out->h; i++) out->px[i] |= 0xff000000;
    else HeapFree(GetProcessHeap(), 0, out->px);
    return ok;
}

static HGLOBAL png_from_pixels(const Pixels *p)
{
    HGLOBAL result = NULL;
    void *img = NULL;
    IStream *stm = NULL;
    if (!gdiplus_ready()) return NULL;
    if (pGdipCreateBitmapFromScan0(p->w, p->h, p->w * 4, PixelFormat32bppRGB, (BYTE *)p->px, &img)) return NULL;
    if (SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &stm)) && !pGdipSaveImageToStream(img, stm, &CLSID_PngEncoder, NULL)) {
        STATSTG st;
        HGLOBAL src;
        if (SUCCEEDED(IStream_Stat(stm, &st, STATFLAG_NONAME)) && SUCCEEDED(GetHGlobalFromStream(stm, &src))) {
            SIZE_T n = (SIZE_T)st.cbSize.QuadPart;
            if ((result = GlobalAlloc(GMEM_MOVEABLE, n))) {
                memcpy(GlobalLock(result), GlobalLock(src), n);
                GlobalUnlock(src); GlobalUnlock(result);
            }
        }
    }
    if (stm) IStream_Release(stm);
    pGdipDisposeImage(img);
    return result;
}

/* Decode PNG bytes. Transparent pixels are blended over white, because a Paint canvas has no alpha. */
static BOOL pixels_from_png(HGLOBAL png, Pixels *out)
{
    BOOL ok = FALSE;
    IStream *stm = NULL;
    void *img = NULL;
    SIZE_T n = GlobalSize(png);
    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!gdiplus_ready() || !copy) { if (copy) GlobalFree(copy); return FALSE; }
    memcpy(GlobalLock(copy), GlobalLock(png), n);
    GlobalUnlock(png); GlobalUnlock(copy);
    if (FAILED(CreateStreamOnHGlobal(copy, TRUE, &stm))) { GlobalFree(copy); return FALSE; }
    if (!pGdipCreateBitmapFromStream(stm, &img)) {
        UINT w = 0, h = 0;
        pGdipGetImageWidth(img, &w); pGdipGetImageHeight(img, &h);
        GpRect r = { 0, 0, (INT)w, (INT)h };
        GpBitmapData d;
        if (w && h && !pGdipBitmapLockBits(img, &r, ImageLockModeRead, PixelFormat32bppARGB, &d)) {
            out->w = w; out->h = h;
            out->px = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)w * h * 4);
            if (out->px) {
                for (UINT y = 0; y < h; y++) {
                    const DWORD *row = (const DWORD *)((const BYTE *)d.Scan0 + (SSIZE_T)y * d.Stride);
                    for (UINT x = 0; x < w; x++) {
                        DWORD c = row[x], a = c >> 24, res = 0xff000000;
                        for (int s = 0; s < 24; s += 8) res |= (((c >> s & 0xff) * a + 255 * (255 - a) + 127) / 255) << s;
                        out->px[(SIZE_T)y * w + x] = res;
                    }
                }
                ok = TRUE;
            }
            pGdipBitmapUnlockBits(img, &d);
        }
        pGdipDisposeImage(img);
    }
    IStream_Release(stm);
    return ok;
}

static HGLOBAL dib_from_pixels(const Pixels *p)
{
    SIZE_T stride = ((SIZE_T)p->w * 3 + 3) & ~(SIZE_T)3;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + stride * p->h);
    if (!g) return NULL;
    BITMAPINFOHEADER *bh = GlobalLock(g);
    ZeroMemory(bh, sizeof(*bh));
    bh->biSize = sizeof(*bh); bh->biWidth = p->w; bh->biHeight = p->h; bh->biPlanes = 1;
    bh->biBitCount = 24; bh->biCompression = BI_RGB; bh->biSizeImage = (DWORD)(stride * p->h);
    BYTE *bits = (BYTE *)(bh + 1);
    for (int y = 0; y < p->h; y++) { /* bottom-up rows */
        BYTE *d = bits + stride * (p->h - 1 - y);
        const DWORD *s = p->px + (SIZE_T)y * p->w;
        for (int x = 0; x < p->w; x++) { d[3 * x] = s[x] & 0xff; d[3 * x + 1] = s[x] >> 8 & 0xff; d[3 * x + 2] = s[x] >> 16 & 0xff; }
        for (SIZE_T x = (SIZE_T)p->w * 3; x < stride; x++) d[x] = 0;
    }
    GlobalUnlock(g);
    return g;
}

static HBITMAP hbitmap_from_pixels(const Pixels *p)
{
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), p->w, -p->h, 1, 32, BI_RGB } };
    HDC dc = GetDC(NULL);
    HBITMAP hbm = CreateDIBitmap(dc, &bi.bmiHeader, CBM_INIT, p->px, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    return hbm;
}

/* ---- IEnumFORMATETC over a fixed array ---- */
typedef struct { IEnumFORMATETC iface; LONG ref; ULONG pos, count; FORMATETC fmt[]; } FmtEnum;
static const IEnumFORMATETCVtbl fmtenum_vtbl;

static FmtEnum *fmtenum_new(const FORMATETC *fmt, ULONG count, ULONG pos)
{
    FmtEnum *e = HeapAlloc(GetProcessHeap(), 0, sizeof(*e) + count * sizeof(FORMATETC));
    if (!e) return NULL;
    e->iface.lpVtbl = (IEnumFORMATETCVtbl *)&fmtenum_vtbl;
    e->ref = 1; e->pos = pos; e->count = count;
    memcpy(e->fmt, fmt, count * sizeof(FORMATETC));
    return e;
}
static HRESULT WINAPI fe_QueryInterface(IEnumFORMATETC *This, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumFORMATETC)) { *out = This; IEnumFORMATETC_AddRef(This); return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI fe_AddRef(IEnumFORMATETC *This) { return InterlockedIncrement(&((FmtEnum *)This)->ref); }
static ULONG WINAPI fe_Release(IEnumFORMATETC *This)
{
    ULONG r = InterlockedDecrement(&((FmtEnum *)This)->ref);
    if (!r) HeapFree(GetProcessHeap(), 0, This);
    return r;
}
static HRESULT WINAPI fe_Next(IEnumFORMATETC *This, ULONG n, FORMATETC *out, ULONG *fetched)
{
    FmtEnum *e = (FmtEnum *)This;
    ULONG i = 0;
    for (; i < n && e->pos < e->count; i++) out[i] = e->fmt[e->pos++];
    if (fetched) *fetched = i;
    return i == n ? S_OK : S_FALSE;
}
static HRESULT WINAPI fe_Skip(IEnumFORMATETC *This, ULONG n)
{
    FmtEnum *e = (FmtEnum *)This;
    e->pos = e->pos + n > e->count ? e->count : e->pos + n;
    return e->pos < e->count ? S_OK : S_FALSE;
}
static HRESULT WINAPI fe_Reset(IEnumFORMATETC *This) { ((FmtEnum *)This)->pos = 0; return S_OK; }
static HRESULT WINAPI fe_Clone(IEnumFORMATETC *This, IEnumFORMATETC **out)
{
    FmtEnum *e = (FmtEnum *)This, *c = fmtenum_new(e->fmt, e->count, e->pos);
    *out = c ? &c->iface : NULL;
    return c ? S_OK : E_OUTOFMEMORY;
}
static const IEnumFORMATETCVtbl fmtenum_vtbl = { fe_QueryInterface, fe_AddRef, fe_Release, fe_Next, fe_Skip, fe_Reset, fe_Clone };

/* ---- IDataObject wrapper that adds PNG <-> bitmap conversions ---- */
typedef struct { IDataObject iface; LONG ref; IDataObject *inner; } Wrap;
static const IDataObjectVtbl wrap_vtbl;

static BOOL inner_has(IDataObject *inner, CLIPFORMAT cf, DWORD tymed)
{
    FORMATETC f = { cf, NULL, DVASPECT_CONTENT, -1, tymed };
    return IDataObject_QueryGetData(inner, &f) == S_OK;
}
static BOOL inner_has_bitmap(IDataObject *inner) { return inner_has(inner, CF_DIB, TYMED_HGLOBAL) || inner_has(inner, CF_BITMAP, TYMED_GDI); }

static BOOL inner_pixels(IDataObject *inner, Pixels *p)
{
    FORMATETC f = { CF_DIB, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM m;
    BOOL ok = FALSE;
    if (SUCCEEDED(IDataObject_GetData(inner, &f, &m))) {
        if (m.tymed == TYMED_HGLOBAL) { ok = pixels_from_dib(GlobalLock(m.hGlobal), GlobalSize(m.hGlobal), p); GlobalUnlock(m.hGlobal); }
        ReleaseStgMedium(&m);
        if (ok) return TRUE;
    }
    f.cfFormat = CF_BITMAP; f.tymed = TYMED_GDI;
    if (SUCCEEDED(IDataObject_GetData(inner, &f, &m))) {
        if (m.tymed == TYMED_GDI) ok = pixels_from_hbitmap(m.hBitmap, p);
        ReleaseStgMedium(&m);
    }
    return ok;
}

static BOOL inner_png_pixels(IDataObject *inner, Pixels *p)
{
    FORMATETC f = { (CLIPFORMAT)cf_png, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM m;
    BOOL ok = FALSE;
    if (SUCCEEDED(IDataObject_GetData(inner, &f, &m))) {
        if (m.tymed == TYMED_HGLOBAL) ok = pixels_from_png(m.hGlobal, p);
        ReleaseStgMedium(&m);
    }
    return ok;
}

/* Which formats this wrapper can make that the inner object lacks. */
static BOOL can_make(IDataObject *inner, CLIPFORMAT cf, DWORD tymed)
{
    if (cf == cf_png && (tymed & TYMED_HGLOBAL)) return inner_has_bitmap(inner);
    if ((cf == CF_DIB && (tymed & TYMED_HGLOBAL)) || (cf == CF_BITMAP && (tymed & TYMED_GDI)))
        return !inner_has_bitmap(inner) && inner_has(inner, (CLIPFORMAT)cf_png, TYMED_HGLOBAL);
    return FALSE;
}

static IDataObject *wrap_new(IDataObject *inner)
{
    Wrap *w = HeapAlloc(GetProcessHeap(), 0, sizeof(*w));
    if (!w) return NULL;
    w->iface.lpVtbl = (IDataObjectVtbl *)&wrap_vtbl;
    w->ref = 1;
    w->inner = inner;
    IDataObject_AddRef(inner);
    return &w->iface;
}
#define INNER(This) (((Wrap *)(This))->inner)

static HRESULT WINAPI w_QueryInterface(IDataObject *This, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDataObject)) { *out = This; IDataObject_AddRef(This); return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI w_AddRef(IDataObject *This) { return InterlockedIncrement(&((Wrap *)This)->ref); }
static ULONG WINAPI w_Release(IDataObject *This)
{
    ULONG r = InterlockedDecrement(&((Wrap *)This)->ref);
    if (!r) { IDataObject_Release(INNER(This)); HeapFree(GetProcessHeap(), 0, This); }
    return r;
}
static HRESULT WINAPI w_GetData(IDataObject *This, FORMATETC *f, STGMEDIUM *m)
{
    HRESULT hr = IDataObject_GetData(INNER(This), f, m);
    if (SUCCEEDED(hr) || !can_make(INNER(This), f->cfFormat, f->tymed)) return hr;
    Pixels p;
    if (f->cfFormat == cf_png) { if (!inner_pixels(INNER(This), &p)) return hr; }
    else if (!inner_png_pixels(INNER(This), &p)) return hr;
    m->pUnkForRelease = NULL;
    if (f->cfFormat == cf_png) { m->tymed = TYMED_HGLOBAL; m->hGlobal = png_from_pixels(&p); }
    else if (f->cfFormat == CF_DIB) { m->tymed = TYMED_HGLOBAL; m->hGlobal = dib_from_pixels(&p); }
    else { m->tymed = TYMED_GDI; m->hBitmap = hbitmap_from_pixels(&p); }
    HeapFree(GetProcessHeap(), 0, p.px);
    return m->hGlobal ? S_OK : hr;
}
static HRESULT WINAPI w_GetDataHere(IDataObject *This, FORMATETC *f, STGMEDIUM *m) { return IDataObject_GetDataHere(INNER(This), f, m); }
static HRESULT WINAPI w_QueryGetData(IDataObject *This, FORMATETC *f)
{
    HRESULT hr = IDataObject_QueryGetData(INNER(This), f);
    if (hr != S_OK && (!f->ptd && f->dwAspect == DVASPECT_CONTENT) && can_make(INNER(This), f->cfFormat, f->tymed)) hr = S_OK;
    return hr;
}
static HRESULT WINAPI w_GetCanonicalFormatEtc(IDataObject *This, FORMATETC *in, FORMATETC *out) { return IDataObject_GetCanonicalFormatEtc(INNER(This), in, out); }
static HRESULT WINAPI w_SetData(IDataObject *This, FORMATETC *f, STGMEDIUM *m, BOOL release) { return IDataObject_SetData(INNER(This), f, m, release); }
static HRESULT WINAPI w_EnumFormatEtc(IDataObject *This, DWORD dir, IEnumFORMATETC **out)
{
    IEnumFORMATETC *ie;
    HRESULT hr = IDataObject_EnumFormatEtc(INNER(This), dir, &ie);
    if (dir != DATADIR_GET || FAILED(hr)) return hr;
    FORMATETC fmt[64];
    ULONG n = 0, got;
    BOOL have_png = FALSE, have_dib = FALSE, have_bmp = FALSE;
    while (n < 61 && IEnumFORMATETC_Next(ie, 1, &fmt[n], &got) == S_OK && got == 1) {
        if (fmt[n].ptd) { CoTaskMemFree(fmt[n].ptd); fmt[n].ptd = NULL; }
        have_png |= fmt[n].cfFormat == cf_png;
        have_dib |= fmt[n].cfFormat == CF_DIB;
        have_bmp |= fmt[n].cfFormat == CF_BITMAP;
        n++;
    }
    IEnumFORMATETC_Release(ie);
    FORMATETC png = { (CLIPFORMAT)cf_png, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    FORMATETC dib = { CF_DIB, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    FORMATETC bmp = { CF_BITMAP, NULL, DVASPECT_CONTENT, -1, TYMED_GDI };
    if (!have_png && can_make(INNER(This), png.cfFormat, png.tymed)) fmt[n++] = png;
    if (!have_dib && can_make(INNER(This), dib.cfFormat, dib.tymed)) fmt[n++] = dib;
    if (!have_bmp && can_make(INNER(This), bmp.cfFormat, bmp.tymed)) fmt[n++] = bmp;
    FmtEnum *e = fmtenum_new(fmt, n, 0);
    *out = e ? &e->iface : NULL;
    return e ? S_OK : E_OUTOFMEMORY;
}
static HRESULT WINAPI w_DAdvise(IDataObject *This, FORMATETC *f, DWORD adv, IAdviseSink *sink, DWORD *conn) { return IDataObject_DAdvise(INNER(This), f, adv, sink, conn); }
static HRESULT WINAPI w_DUnadvise(IDataObject *This, DWORD conn) { return IDataObject_DUnadvise(INNER(This), conn); }
static HRESULT WINAPI w_EnumDAdvise(IDataObject *This, IEnumSTATDATA **out) { return IDataObject_EnumDAdvise(INNER(This), out); }
static const IDataObjectVtbl wrap_vtbl = {
    w_QueryInterface, w_AddRef, w_Release, w_GetData, w_GetDataHere, w_QueryGetData,
    w_GetCanonicalFormatEtc, w_SetData, w_EnumFormatEtc, w_DAdvise, w_DUnadvise, w_EnumDAdvise,
};

/* ---- hooks ---- */
static HRESULT (WINAPI *real_OleSetClipboard)(IDataObject *);
static HRESULT (WINAPI *real_OleGetClipboard)(IDataObject **);
static HRESULT (WINAPI *real_OleIsCurrentClipboard)(IDataObject *);
static BOOL (WINAPI *real_IsClipboardFormatAvailable)(UINT);
static IDataObject *set_inner, *set_wrap; /* Paint's last data object and its wrapper (no references held) */

static HRESULT WINAPI hook_OleSetClipboard(IDataObject *obj)
{
    if (!obj) { set_inner = set_wrap = NULL; return real_OleSetClipboard(NULL); }
    IDataObject *w = wrap_new(obj);
    if (!w) return real_OleSetClipboard(obj);
    HRESULT hr = real_OleSetClipboard(w);
    if (SUCCEEDED(hr)) { set_inner = obj; set_wrap = w; }
    IDataObject_Release(w);
    return hr;
}

static HRESULT WINAPI hook_OleIsCurrentClipboard(IDataObject *obj)
{
    if (obj && obj == set_inner) {
        HRESULT hr = real_OleIsCurrentClipboard(set_wrap);
        if (hr != S_OK) set_inner = set_wrap = NULL;
        return hr;
    }
    return real_OleIsCurrentClipboard(obj);
}

static HRESULT WINAPI hook_OleGetClipboard(IDataObject **out)
{
    HRESULT hr = real_OleGetClipboard(out);
    if (SUCCEEDED(hr) && *out && !real_IsClipboardFormatAvailable(CF_DIB) && real_IsClipboardFormatAvailable(cf_png)) {
        IDataObject *w = wrap_new(*out);
        if (w) { IDataObject_Release(*out); *out = w; }
    }
    return hr;
}

/* When Paint exits, Wine's wineserver exits a moment later and the X clipboard
 * owner goes away with it. Hand the flushed PNG to /usr/bin/xclip,
 * which keeps serving it to X programs until something else is copied. */
static BOOL CALLBACK find_visible_window(HWND hwnd, LPARAM found)
{
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) { *(BOOL *)found = TRUE; return FALSE; }
    return TRUE;
}

static void hand_off_clipboard(void)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    WCHAR *(CDECL *to_dos)(const char *) = (void *)GetProcAddress(k32, "wine_get_dos_file_name");
    char *(CDECL *to_unix)(const WCHAR *) = (void *)GetProcAddress(k32, "wine_get_unix_file_name");
    WCHAR *exe = to_dos ? to_dos("/usr/bin/xclip") : NULL, path[MAX_PATH + 32], cmd[MAX_PATH * 2];
    char *unix_path = NULL;
    BOOL written = FALSE;
    if (!exe || !to_unix || GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) goto done;
    /* A pipe to xclip's stdin arrives empty under Wine 9.0, so pass a file.
       The file has a fixed name and is overwritten on each hand-off. */
    DWORD n = GetTempPathW(MAX_PATH, path);
    if (!n || n >= MAX_PATH) goto done;
    lstrcatW(path, L"mspaint-clipboard.png");
    if (!OpenClipboard(NULL)) goto done;
    HANDLE h = GetClipboardData(cf_png);
    if (h) {
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD size = (DWORD)GlobalSize(h), done;
            written = size && WriteFile(f, GlobalLock(h), size, &done, NULL) && done == size;
            GlobalUnlock(h);
            CloseHandle(f);
        }
    }
    CloseClipboard();
    if (!written || !(unix_path = to_unix(path))) goto done;
    WCHAR wunix[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_UTF8, 0, unix_path, -1, wunix, MAX_PATH * 2)) goto done;
    wsprintfW(cmd, L"xclip -selection clipboard -t image/png -i \"%s\"", wunix);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        if (pi.hProcess) { WaitForSingleObject(pi.hProcess, 3000); CloseHandle(pi.hProcess); }
        if (pi.hThread) CloseHandle(pi.hThread);
    }
done:
    if (unix_path) HeapFree(GetProcessHeap(), 0, unix_path);
    if (exe) HeapFree(GetProcessHeap(), 0, exe);
}

static HRESULT (WINAPI *real_OleFlushClipboard)(void);
static HRESULT WINAPI hook_OleFlushClipboard(void)
{
    BOOL ours = set_wrap && real_OleIsCurrentClipboard(set_wrap) == S_OK, visible = FALSE;
    HRESULT hr = real_OleFlushClipboard();
    EnumWindows(find_visible_window, (LPARAM)&visible);
    if (SUCCEEDED(hr) && ours && !visible) hand_off_clipboard(); /* no visible window left: Paint is exiting */
    return hr;
}

static BOOL WINAPI hook_IsClipboardFormatAvailable(UINT cf)
{
    BOOL r = real_IsClipboardFormatAvailable(cf);
    if (!r && (cf == CF_DIB || cf == CF_BITMAP || cf == CF_DIBV5)) r = real_IsClipboardFormatAvailable(cf_png);
    return r;
}

/* ---- EM_FORMATRANGE for the text tool ----
 * Paint commits the text box by sending EM_FORMATRANGE to the rich edit
 * control with the canvas DC. Wine 9.0's riched20 does not implement
 * EM_FORMATRANGE (it is an "UNSUPPORTED_MSG"), so committed text vanished.
 * This copies the content (RTF) into a hidden rich edit control at canvas
 * scale (Paint zooms the box with EM_SETZOOM, EM_FORMATRANGE must not),
 * takes the position of each character from it (EM_POSFROMCHAR) and draws
 * each run of equal formatting with ExtTextOutW directly on Paint's DC, so
 * anti-aliasing blends with the real canvas pixels, as on Windows. */
#include <richedit.h>

typedef struct { BYTE *data; SIZE_T size, cap, pos; } RtfBuf;

static DWORD CALLBACK rtf_out(DWORD_PTR cookie, BYTE *buf, LONG n, LONG *done)
{
    RtfBuf *b = (RtfBuf *)cookie;
    if (b->size + n > b->cap) {
        SIZE_T cap = (b->size + n) * 2;
        BYTE *d = b->data ? HeapReAlloc(GetProcessHeap(), 0, b->data, cap) : HeapAlloc(GetProcessHeap(), 0, cap);
        if (!d) return 1;
        b->data = d; b->cap = cap;
    }
    memcpy(b->data + b->size, buf, n);
    b->size += n;
    *done = n;
    return 0;
}

static DWORD CALLBACK rtf_in(DWORD_PTR cookie, BYTE *buf, LONG n, LONG *done)
{
    RtfBuf *b = (RtfBuf *)cookie;
    SIZE_T left = b->size - b->pos;
    *done = (LONG)(left < (SIZE_T)n ? left : (SIZE_T)n);
    memcpy(buf, b->data + b->pos, *done);
    b->pos += *done;
    return 0;
}

static LRESULT (WINAPI *real_SendMessageW)(HWND, UINT, WPARAM, LPARAM);

static BOOL is_break(WCHAR c) { return c == '\r' || c == '\n' || c == '\t' || c == 0x0b; }

static LRESULT format_range(HWND edit, BOOL render, FORMATRANGE *fr)
{
    LRESULT len = real_SendMessageW(edit, WM_GETTEXTLENGTH, 0, 0);
    if (!fr || !render || !fr->hdc) return fr ? len : 0;
    int dx = GetDeviceCaps(fr->hdc, LOGPIXELSX), dy = GetDeviceCaps(fr->hdc, LOGPIXELSY);
    int x = MulDiv(fr->rc.left, dx, 1440), y = MulDiv(fr->rc.top, dy, 1440);
    int w = MulDiv(fr->rc.right - fr->rc.left, dx, 1440), h = MulDiv(fr->rc.bottom - fr->rc.top, dy, 1440);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || !len) return len;

    WCHAR cls[64];
    if (!GetClassNameW(edit, cls, 64)) return 0;
    DWORD style = (GetWindowLongW(edit, GWL_STYLE) & 0xffff & ~(ES_AUTOHSCROLL | ES_NOHIDESEL)) | ES_MULTILINE | WS_POPUP;
    HWND tmp = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, cls, NULL, style, -32000, -32000, w, h,
                               NULL, NULL, (HINSTANCE)GetWindowLongPtrW(edit, GWLP_HINSTANCE), NULL);
    if (!tmp) return 0;
    RECT cr;
    GetClientRect(tmp, &cr); /* the rich edit class may add a border: make the client area w x h */
    if (cr.right != w || cr.bottom != h)
        SetWindowPos(tmp, NULL, 0, 0, 2 * w - cr.right, 2 * h - cr.bottom, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    /* Wine's RTF writer records effects as changes from plain text, and the
       default format of a new control is the bold system font: start plain. */
    CHARFORMAT2W def = { sizeof(def) };
    real_SendMessageW(edit, EM_GETCHARFORMAT, SCF_DEFAULT, (LPARAM)&def);
    def.dwMask = CFM_ALL2;
    def.dwEffects &= ~(CFE_BOLD | CFE_ITALIC | CFE_UNDERLINE | CFE_STRIKEOUT | CFE_SUBSCRIPT | CFE_SUPERSCRIPT | CFE_HIDDEN);
    def.wWeight = FW_NORMAL;
    real_SendMessageW(tmp, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&def);
    real_SendMessageW(tmp, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&def);
    RtfBuf rtf = { 0 };
    EDITSTREAM es = { (DWORD_PTR)&rtf, 0, rtf_out };
    real_SendMessageW(edit, EM_STREAMOUT, SF_RTF, (LPARAM)&es);
    if (rtf.size) {
        es.pfnCallback = rtf_in;
        real_SendMessageW(tmp, EM_STREAMIN, SF_RTF, (LPARAM)&es);
    }
    HeapFree(GetProcessHeap(), 0, rtf.data);

    /* Draw each run of equal formatting at the position the hidden control gives it. */
    GETTEXTLENGTHEX gl = { GTL_NUMCHARS | GTL_PRECISE, 1200 };
    LONG n = (LONG)real_SendMessageW(tmp, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
    WCHAR *text = n > 0 ? HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(WCHAR)) : NULL;
    POINTL *pos = n > 0 ? HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(POINTL)) : NULL;
    INT *adv = n > 0 ? HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(INT)) : NULL;
    GETTEXTEX gt = { (n + 1) * sizeof(WCHAR), GT_DEFAULT, 1200, NULL, NULL };
    if (text && pos && adv && real_SendMessageW(tmp, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)text)) {
        for (LONG i = 0; i < n; i++) real_SendMessageW(tmp, EM_POSFROMCHAR, (WPARAM)&pos[i], i);
        int save = SaveDC(fr->hdc);
        SetBkMode(fr->hdc, TRANSPARENT);
        SetTextAlign(fr->hdc, TA_TOP | TA_LEFT | TA_NOUPDATECP);
        CHARFORMAT2W cf = { sizeof(cf) }, next = { sizeof(next) };
        for (LONG i = 0; i < n;) {
            CHARRANGE cr1 = { i, i + 1 };
            real_SendMessageW(tmp, EM_EXSETSEL, 0, (LPARAM)&cr1);
            cf.dwMask = CFM_ALL2;
            real_SendMessageW(tmp, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
            /* the run: same format, same line, no break, paragraph end or tab (tabs are not drawn) */
            LONG j = i + 1;
            while (j < n && !is_break(text[j - 1]) && !is_break(text[j]) &&
                   pos[j].y == pos[i].y && pos[j].x > pos[j - 1].x) {
                CHARRANGE cr2 = { j, j + 1 };
                real_SendMessageW(tmp, EM_EXSETSEL, 0, (LPARAM)&cr2);
                next.dwMask = CFM_ALL2;
                real_SendMessageW(tmp, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&next);
                if (next.dwEffects != cf.dwEffects || next.yHeight != cf.yHeight || next.wWeight != cf.wWeight ||
                    next.crTextColor != cf.crTextColor || lstrcmpW(next.szFaceName, cf.szFaceName)) break;
                j++;
            }
            LONG k = i;
            while (k < j && is_break(text[k])) k++;
            LONG end = j;
            while (end > k && is_break(text[end - 1])) end--;
            if (end > k && !(cf.dwEffects & CFE_HIDDEN)) {
                LOGFONTW lf = { 0 };
                lf.lfHeight = -MulDiv(cf.yHeight, dy, 1440);
                lf.lfWeight = cf.wWeight ? cf.wWeight : (cf.dwEffects & CFE_BOLD) ? FW_BOLD : FW_NORMAL;
                lf.lfItalic = !!(cf.dwEffects & CFE_ITALIC);
                lf.lfUnderline = !!(cf.dwEffects & CFE_UNDERLINE);
                lf.lfStrikeOut = !!(cf.dwEffects & CFE_STRIKEOUT);
                lf.lfCharSet = cf.bCharSet;
                lf.lfPitchAndFamily = cf.bPitchAndFamily;
                lstrcpynW(lf.lfFaceName, cf.szFaceName, LF_FACESIZE);
                HFONT font = CreateFontIndirectW(&lf), oldf = SelectObject(fr->hdc, font);
                SetTextColor(fr->hdc, (cf.dwEffects & CFE_AUTOCOLOR) ? GetSysColor(COLOR_WINDOWTEXT) : cf.crTextColor);
                for (LONG m = k; m < end; m++) {
                    if (m + 1 < end) adv[m - k] = pos[m + 1].x - pos[m].x;
                    else { SIZE sz; adv[m - k] = GetTextExtentPoint32W(fr->hdc, &text[m], 1, &sz) ? sz.cx : 0; }
                }
                ExtTextOutW(fr->hdc, x + pos[k].x, y + pos[k].y, 0, NULL, &text[k], end - k, adv);
                SelectObject(fr->hdc, oldf);
                DeleteObject(font);
            }
            i = j;
        }
        RestoreDC(fr->hdc, save);
    }
    if (text) HeapFree(GetProcessHeap(), 0, text);
    if (pos) HeapFree(GetProcessHeap(), 0, pos);
    if (adv) HeapFree(GetProcessHeap(), 0, adv);
    DestroyWindow(tmp);
    return len;
}

static LRESULT WINAPI hook_SendMessageW(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == EM_FORMATRANGE) {
        WCHAR cls[16];
        if (GetClassNameW(hwnd, cls, 16) && !_wcsnicmp(cls, L"RichEdit", 8)) return format_range(hwnd, (BOOL)wp, (FORMATRANGE *)lp);
    }
    return real_SendMessageW(hwnd, msg, wp, lp);
}

/* Replace one import of module mod. Returns the old target, or NULL. */
static void *patch_import(HMODULE mod, const char *dll, const char *func, void *hook)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dir.VirtualAddress); d->Name; d++) {
        if (lstrcmpiA((char *)base + d->Name, dll)) continue;
        IMAGE_THUNK_DATA *names = (void *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *iat = (void *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp(((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, func)) continue;
            DWORD old;
            void *prev = (void *)iat->u1.Function;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) return NULL;
            iat->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
            return prev;
        }
    }
    return NULL;
}

void pclip_init(void)
{
    HMODULE exe = GetModuleHandleW(NULL), mfc = GetModuleHandleW(L"mfc42u.dll");
    HMODULE ole = GetModuleHandleW(L"ole32.dll"), user = GetModuleHandleW(L"user32.dll");
    if (!ole || !user) return;
    cf_png = RegisterClipboardFormatW(L"PNG");
    real_OleSetClipboard = (void *)GetProcAddress(ole, "OleSetClipboard");
    real_OleGetClipboard = (void *)GetProcAddress(ole, "OleGetClipboard");
    real_OleIsCurrentClipboard = (void *)GetProcAddress(ole, "OleIsCurrentClipboard");
    real_OleFlushClipboard = (void *)GetProcAddress(ole, "OleFlushClipboard");
    real_IsClipboardFormatAvailable = (void *)GetProcAddress(user, "IsClipboardFormatAvailable");
    if (!cf_png || !real_OleSetClipboard || !real_OleGetClipboard || !real_OleIsCurrentClipboard || !real_OleFlushClipboard || !real_IsClipboardFormatAvailable) return;
    real_SendMessageW = (void *)GetProcAddress(user, "SendMessageW");
    if (real_SendMessageW) patch_import(exe, "user32.dll", "SendMessageW", hook_SendMessageW);
    HMODULE mods[2] = { exe, mfc };
    for (int i = 0; i < 2; i++) {
        if (!mods[i]) continue;
        patch_import(mods[i], "ole32.dll", "OleSetClipboard", hook_OleSetClipboard);
        patch_import(mods[i], "ole32.dll", "OleGetClipboard", hook_OleGetClipboard);
        patch_import(mods[i], "ole32.dll", "OleIsCurrentClipboard", hook_OleIsCurrentClipboard);
        patch_import(mods[i], "ole32.dll", "OleFlushClipboard", hook_OleFlushClipboard);
        patch_import(mods[i], "user32.dll", "IsClipboardFormatAvailable", hook_IsClipboardFormatAvailable);
    }
}
