#include <windows.h>
#include <shellapi.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include "readonly_files.h"

void require(bool ok, const char *what) { if (!ok) throw std::runtime_error(what); }
void write(const std::wstring &path, const char *bytes) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  require(file != INVALID_HANDLE_VALUE, "cannot write independent copy");
  DWORD n = 0;
  require(WriteFile(file, bytes, static_cast<DWORD>(strlen(bytes)), &n, nullptr), "write failed");
  CloseHandle(file);
}
void cleanup(const std::wstring &path) {
  ReadOnlyFiles::allowCleanup(path);
  DWORD attrs = GetFileAttributesW(path.c_str());
  if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
    WIN32_FIND_DATAW data{};
    HANDLE found = FindFirstFileW((path + L"\\*").c_str(), &data);
    if (found != INVALID_HANDLE_VALUE) {
      do { std::wstring name = data.cFileName; if (name != L"." && name != L"..") cleanup(path + L"\\" + name); }
      while (FindNextFileW(found, &data));
      FindClose(found);
    }
    RemoveDirectoryW(path.c_str());
  } else DeleteFileW(path.c_str());
}
int main(int argc, char **argv) {
  bool expectUnsupported = argc == 2 && std::string(argv[1]) == "--expect-unsupported";
  // OpenSSH can hand test executables an administrative token with backup/
  // restore privileges enabled. Ordinary desktop applications do not get those
  // ACL-bypass privileges; remove them from this probe's token only.
  HANDLE privileges = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &privileges)) return 2;
  if (!AdjustTokenPrivileges(privileges, TRUE, nullptr, 0, nullptr, nullptr)) { CloseHandle(privileges); return 2; }
  CloseHandle(privileges);
  wchar_t temp[MAX_PATH]{};
  GetTempPathW(MAX_PATH, temp);
  std::wstring root = std::wstring(temp) + L"Waybridge-readonly-probe-" + std::to_wstring(GetCurrentProcessId());
  try {
    require(CreateDirectoryW(root.c_str(), nullptr), "create fixture");
    auto batch = root + L"\\delivery", nested = batch + L"\\nested", empty = nested + L"\\empty";
    require(CreateDirectoryW(batch.c_str(), nullptr), "create delivery");
    require(CreateDirectoryW(nested.c_str(), nullptr), "create nested");
    require(CreateDirectoryW(empty.c_str(), nullptr), "create empty");
    auto project = nested + L"\\project.blend";
    write(project, "original project");
    try { ReadOnlyFiles::protectTree(batch); }
    catch (const std::exception &error) {
      if (!expectUnsupported || std::string(error.what()).find("Read-only delivery unsupported") == std::string::npos) throw;
      cleanup(root);
      require(GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES, "unsupported staging leaked");
      std::cout << "PASS: unsupported read-only filesystem explicitly rejected and staging removed\n";
      return 0;
    }
    require(!expectUnsupported, "expected unsupported filesystem was accepted");
    HANDLE editable = CreateFileW(project.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    require(editable == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED, "direct write was not rejected");
    require(!(GetFileAttributesW(project.c_str()) & FILE_ATTRIBUTE_READONLY), "readonly attribute would leak into copies");
    auto replacement = root + L"\\replace";
    write(replacement, "replacement");
    require(!MoveFileExW(replacement.c_str(), project.c_str(), MOVEFILE_REPLACE_EXISTING), "atomic replacement was not rejected");
    require(!DeleteFileW(project.c_str()), "delete was not rejected");
    require(!CreateDirectoryW((nested + L"\\new").c_str(), nullptr), "directory mutation was not rejected");
    auto saved = root + L"\\saved";
    require(CreateDirectoryW(saved.c_str(), nullptr), "create saved location");
    auto single = saved + L"\\copy.blend";
    require(CopyFileW(project.c_str(), single.c_str(), TRUE), "copy to user location failed");
    write(single, "saved edit");
    // Exercise the same Shell copy API as file-manager delivery, including directories.
    std::wstring from = batch; from.push_back(0); from.push_back(0);
    std::wstring to = saved; to.push_back(0); to.push_back(0);
    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_COPY; operation.pFrom = from.c_str(); operation.pTo = to.c_str();
    operation.fFlags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI;
    require(SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted, "shell directory copy failed");
    write(saved + L"\\delivery\\nested\\project.blend", "shell saved edit");
    DWORD emptyAttributes = GetFileAttributesW((saved + L"\\delivery\\nested\\empty").c_str());
    require(emptyAttributes != INVALID_FILE_ATTRIBUTES && (emptyAttributes & FILE_ATTRIBUTE_DIRECTORY), "empty directory missing");
    HANDLE reader = CreateFileW(project.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    require(reader != INVALID_HANDLE_VALUE, "delivered source no longer readable");
    char bytes[32]{}; DWORD n = 0;
    require(ReadFile(reader, bytes, sizeof(bytes), &n, nullptr) && std::string(bytes, n) == "original project", "delivery content changed");
    CloseHandle(reader);
    cleanup(root);
    std::cout << "PASS: protected delivery rejects write/replace/delete, Shell and plain copies are writable, directory contents preserved\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << " (Win32 " << GetLastError() << ")\n";
    cleanup(root);
    return 1;
  }
}
