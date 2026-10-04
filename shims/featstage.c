/* Stub implementation of the Windows feature-staging API set for Wine.
   mspaint.exe loads this DLL at startup, so it also installs the PNG clipboard bridge (pclip.c). */
#include <stddef.h>
#include <stdint.h>
#define EXPORT __declspec(dllexport)
typedef uint32_t UINT32;
typedef int BOOL;
typedef void* HANDLE;
EXPORT UINT32 __stdcall GetFeatureEnabledState(UINT32 featureId, UINT32 changeTime) { (void)featureId; (void)changeTime; return 0; }
EXPORT void __stdcall RecordFeatureUsage(UINT32 featureId, UINT32 kind, UINT32 addend, const char* originName) { (void)featureId; (void)kind; (void)addend; (void)originName; }
EXPORT void __stdcall RecordFeatureError(UINT32 featureId, const void* error) { (void)featureId; (void)error; }
EXPORT void __stdcall SubscribeFeatureStateChangeNotification(void** subscription, void* callback, void* context) { (void)callback; (void)context; if (subscription) *subscription = NULL; }
EXPORT void __stdcall UnsubscribeFeatureStateChangeNotification(void* subscription) { (void)subscription; }
EXPORT void* __stdcall GetFeatureVariant(UINT32 featureId, UINT32 changeTime, UINT32* payloadId, BOOL* hasNotification) { (void)featureId; (void)changeTime; if (payloadId) *payloadId = 0; if (hasNotification) *hasNotification = 0; return NULL; }
void pclip_init(void);
BOOL __stdcall DllMain(HANDLE h, UINT32 reason, void* r) { (void)h; (void)r; if (reason == 1 /* DLL_PROCESS_ATTACH */) pclip_init(); return 1; }
