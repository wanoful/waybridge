#pragma once
// Completed delivery is read-only at its cache path. Do not set the DOS
// READONLY attribute: Explorer copies must acquire normal destination rights.
#include <windows.h>
#include <aclapi.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace ReadOnlyFiles {
inline void access(const std::wstring &path, bool protect) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    throw std::runtime_error("Cannot read delivery owner token");
  DWORD bytes = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
  std::vector<BYTE> info(bytes);
  bool ok = bytes && GetTokenInformation(token, TokenUser, info.data(), bytes, &bytes);
  CloseHandle(token);
  if (!ok)
    throw std::runtime_error("Cannot read delivery owner SID");
  auto sid = reinterpret_cast<TOKEN_USER *>(info.data())->User.Sid;
  DWORD aclSize = sizeof(ACL) + 2 * (sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + GetLengthSid(sid));
  std::vector<BYTE> storage(aclSize);
  auto acl = reinterpret_cast<ACL *>(storage.data());
  // DELETE_CHILD on each directory also blocks temp-file + replace saves.
  constexpr DWORD Write = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
                          FILE_WRITE_ATTRIBUTES | DELETE | FILE_DELETE_CHILD;
  if (!InitializeAcl(acl, aclSize, ACL_REVISION) ||
      (protect && !AddAccessDeniedAce(acl, ACL_REVISION, Write, sid)) ||
      !AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, sid) ||
      SetNamedSecurityInfoW(const_cast<wchar_t *>(path.c_str()), SE_FILE_OBJECT,
                            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                            nullptr, nullptr, acl, nullptr) != ERROR_SUCCESS)
    throw std::runtime_error("Cannot protect read-only file delivery (requires Windows ACL support)");
}
inline void protectTree(const std::wstring &path) {
  DWORD attrs = GetFileAttributesW(path.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
    throw std::runtime_error("Invalid read-only delivery object");
  if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
    WIN32_FIND_DATAW data{};
    HANDLE found = FindFirstFileW((path + L"\\*").c_str(), &data);
    if (found == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND)
      throw std::runtime_error("Cannot inspect read-only delivery directory");
    if (found != INVALID_HANDLE_VALUE) {
      try {
        do {
          std::wstring name = data.cFileName;
          if (name != L"." && name != L"..") protectTree(path + L"\\" + name);
        } while (FindNextFileW(found, &data));
        DWORD error = GetLastError();
        if (error != ERROR_NO_MORE_FILES) throw std::runtime_error("Cannot enumerate delivery directory");
      } catch (...) { FindClose(found); throw; }
      FindClose(found);
    }
  }
  access(path, true);
  // Some compatibility filesystems (including Wine's Unix ACL translation)
  // synthesize READONLY from the DACL and would propagate it into user copies.
  // Refuse to publish that cache rather than silently violate Copy semantics.
  if (!(attrs & FILE_ATTRIBUTE_DIRECTORY) &&
      (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_READONLY))
    throw std::runtime_error("Read-only delivery unsupported: filesystem copies would inherit READONLY");
}
// Only called by removal of this transfer's unpublished staging, never saved
// user copies. The same-UID model does not claim to resist deliberate ACL edits.
inline void allowCleanup(const std::wstring &path) noexcept {
  try { access(path, false); } catch (...) { }
}
} // namespace ReadOnlyFiles
