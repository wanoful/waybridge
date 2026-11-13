// Resolve the exact embedded WinUI theme blobs using the shipped MRT Core DLL.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <string>
#include <cstring>

struct ResourceData { UINT32 size; void *data; };
int wmain(int argc, wchar_t **argv) {
  if (argc != 3) return 2;
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  auto library = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!library) { std::cerr << "Load MRM.dll failed: " << GetLastError() << '\n'; return 3; }
  auto create = reinterpret_cast<HRESULT(WINAPI *)(PCWSTR, void **)>(GetProcAddress(library, "MrmCreateResourceManager"));
  auto load = reinterpret_cast<HRESULT(WINAPI *)(void *,void *,void *,PCWSTR,ResourceData *)>(GetProcAddress(library, "MrmLoadEmbeddedResource"));
  auto release = reinterpret_cast<void(WINAPI *)(void *)>(GetProcAddress(library, "MrmFreeResource"));
  auto destroy = reinterpret_cast<void(WINAPI *)(void *)>(GetProcAddress(library, "MrmDestroyResourceManager"));
  if (!create || !load || !release || !destroy) return 4;
  void *manager = nullptr;
  auto result = create(argv[2], &manager);
  if (FAILED(result)) { std::cerr << "Open PRI HRESULT 0x" << std::hex << ULONG(result) << '\n'; return 5; }
  for (auto name : {L"Files/Microsoft.UI.Xaml/Themes/themeresources.xbf", L"Files/Microsoft.UI.Xaml/Themes/generic.xbf"}) {
    ResourceData blob{}; result = load(manager,nullptr,nullptr,name,&blob);
    if (FAILED(result) || !blob.data || blob.size < 16 || memcmp(blob.data,"XBF\0",4)) {
      std::cerr << "Resolve theme HRESULT 0x" << std::hex << ULONG(result) << '\n'; destroy(manager); return 6;
    }
    std::cout << "PASS: embedded XBF theme " << blob.size << " bytes\n";
    release(blob.data);
  }
  destroy(manager); FreeLibrary(library);
  return 0;
}
