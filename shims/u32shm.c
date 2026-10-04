/* Proxy for user32.dll: implements exports that Wine lacks and forwards the rest (see u32shm.def). */
#include <stdint.h>
typedef void* HWND; typedef void* HANDLE; typedef int BOOL; typedef uint32_t UINT;
__declspec(dllimport) HWND __stdcall GetAncestor(HWND, UINT);
__declspec(dllimport) HWND __stdcall GetDesktopWindow(void);
__declspec(dllimport) void __stdcall OutputDebugStringA(const char*);
__declspec(dllimport) UINT __stdcall GetDpiForWindow(HWND);
__declspec(dllimport) UINT __stdcall GetDpiForSystem(void);
__declspec(dllimport) HANDLE __stdcall MonitorFromWindow(HWND, UINT);
typedef struct { UINT cbSize; int rcMonitor[4]; int rcWork[4]; UINT dwFlags; } MONITORINFO_;
__declspec(dllimport) BOOL __stdcall GetMonitorInfoW(HANDLE, MONITORINFO_*);
static void logcall(const char* name, intptr_t a, intptr_t b, intptr_t c, intptr_t d) { char buf[160]; const char hex[]="0123456789abcdef"; int i=0; const char* n=name; while(*n) buf[i++]=*n++; intptr_t v[4]={a,b,c,d}; for(int k=0;k<4;k++){ buf[i++]=' '; for(int s=60;s>=0;s-=4) buf[i++]=hex[(v[k]>>s)&15]; } buf[i++]='\n'; buf[i]=0; OutputDebugStringA(buf); }
__declspec(dllexport) BOOL __stdcall IsTopLevelWindow(HWND hwnd) { logcall("IsTopLevelWindow",(intptr_t)hwnd,0,0,0); HWND p = GetAncestor(hwnd, 1 /* GA_PARENT */); return p == 0 || p == GetDesktopWindow(); }
__declspec(dllexport) BOOL __stdcall IsWindowInDestroy(HWND hwnd) { logcall("IsWindowInDestroy",(intptr_t)hwnd,0,0,0); return 0; }
/* Ordinal 2559: NtUserGetOwnerTransformedMonitorRect(hwnd, flag, flag, RECT* out). Returns the monitor rectangle of hwnd. */
__declspec(dllexport) intptr_t __stdcall u32_ord2559(intptr_t a, intptr_t b, intptr_t c, intptr_t d) { (void)c;(void)d; logcall("u32_ord2559", a, b, c, d); MONITORINFO_ mi; mi.cbSize = sizeof mi; HANDLE mon = MonitorFromWindow((HWND)a, 2 /* MONITOR_DEFAULTTONEAREST */); if (!d || !mon || !GetMonitorInfoW(mon, &mi)) return 0; int* r = (int*)d; r[0]=mi.rcMonitor[0]; r[1]=mi.rcMonitor[1]; r[2]=mi.rcMonitor[2]; r[3]=mi.rcMonitor[3]; return 1; }
__declspec(dllexport) intptr_t __stdcall u32_ord2704(intptr_t a, intptr_t b, intptr_t c, intptr_t d) { logcall("u32_ord2704", a, b, c, d); return 0; }
__declspec(dllexport) intptr_t __stdcall u32_ord2705(intptr_t a, intptr_t b, intptr_t c, intptr_t d) { logcall("u32_ord2705", a, b, c, d); return 0; }
__declspec(dllexport) intptr_t __stdcall u32_ord2706(intptr_t a, intptr_t b, intptr_t c, intptr_t d) { logcall("u32_ord2706", a, b, c, d); return 0; }
/* Ordinal 2707: window DPI query. Returns the DPI of hwnd, or the system DPI. */
__declspec(dllexport) intptr_t __stdcall u32_ord2707(intptr_t a, intptr_t b, intptr_t c, intptr_t d) { (void)b;(void)c;(void)d; UINT dpi = a ? GetDpiForWindow((HWND)a) : 0; if (!dpi) dpi = GetDpiForSystem(); if (!dpi) dpi = 96; return dpi; }
BOOL __stdcall _DllMainCRTStartup(void* h, UINT reason, void* r) { (void)h; (void)reason; (void)r; return 1; }
