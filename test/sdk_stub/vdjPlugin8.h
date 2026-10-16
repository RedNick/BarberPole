// MINIMAL STAND-IN for compile-testing on Linux only. NOT the real SDK - do not ship.
// Layout follows the documented public declarations of vdjPlugin8.h.
#pragma once
#include <stddef.h>
#define VDJ_EXPORT __attribute__((visibility("default")))
#define VDJ_API
typedef int HRESULT; typedef unsigned int ULONG; typedef unsigned int DWORD;
typedef char *VDJ_BITMAP; typedef void *VDJ_HINSTANCE; typedef void *VDJ_WINDOW;
#define S_OK 0
#define E_NOTIMPL ((HRESULT)0x80004001)
#define NO_ERROR 0
#define CLASS_E_CLASSNOTAVAILABLE -1
struct GUID { unsigned long Data1; unsigned short Data2, Data3; unsigned char Data4[8]; };
struct TVdjPluginInfo8 { const char *PluginName, *Author, *Description, *Version; VDJ_BITMAP Bitmap; DWORD Flags; };
struct TVdjPluginInterface8 { DWORD Type; const char *Xml; void *ImageBuffer; int ImageSize; VDJ_WINDOW hWnd; };
enum { VDJPARAM_BUTTON, VDJPARAM_SLIDER, VDJPARAM_SWITCH, VDJPARAM_COLORFX = 7 };
class IVdjCallbacks8 { public:
  virtual HRESULT DeclareParameter(void *p, int type, int id, const char *n, const char *s, float d) = 0; };
class IVdjPlugin8 { public:
  virtual HRESULT VDJ_API OnLoad() { return S_OK; }
  virtual HRESULT VDJ_API OnGetPluginInfo(TVdjPluginInfo8 *) { return E_NOTIMPL; }
  virtual ULONG VDJ_API Release() { delete this; return S_OK; }
  virtual ~IVdjPlugin8() {}
  virtual HRESULT VDJ_API OnParameter(int) { return S_OK; }
  virtual HRESULT VDJ_API OnGetParameterString(int, char *, int) { return E_NOTIMPL; }
  VDJ_HINSTANCE hInstance; IVdjCallbacks8 *cb;
  HRESULT DeclareParameterColorFX(float *p, int id, const char *n, const char *s) { return cb->DeclareParameter(p, VDJPARAM_COLORFX, id, n, s, 0.5f); }
  HRESULT DeclareParameterSlider(float *p, int id, const char *n, const char *s, float d) { return cb->DeclareParameter(p, VDJPARAM_SLIDER, id, n, s, d); }
};
static const GUID CLSID_VdjPlugin8 = {0xED8A8D87, 0xF4F9, 0x4DCD, {0xBD,0x24,0x29,0x14,0x12,0xE9,0x3B,0x60}};
extern "C" VDJ_EXPORT HRESULT VDJ_API DllGetClassObject(const GUID &rclsid, const GUID &riid, void **ppObject);
