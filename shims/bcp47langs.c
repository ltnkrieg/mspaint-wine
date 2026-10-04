/* Stub for Bcp47Langs.dll (absent in Wine 9.0). The native fms.dll imports only IsTransientLcid. */
typedef int BOOL; typedef unsigned long LCID;
/* Transient LCIDs are the 0x2000-0x2400 (and 0x1000) placeholders Windows gives to custom locales. None exist under Wine. */
__declspec(dllexport) BOOL __stdcall IsTransientLcid(LCID lcid) { (void)lcid; return 0; }
BOOL __stdcall _DllMainCRTStartup(void* h, unsigned reason, void* r) { (void)h; (void)reason; (void)r; return 1; }
