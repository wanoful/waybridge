// A real Win32 named-pipe host for the native worker, driven from Python/Wine.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <string>
#include "windows_pipe_fixture.h"

static DWORD WINAPI readEvents(void *context) {
  auto &pipe = *static_cast<WindowsPipeFixture *>(context);
  char bytes[8192]; DWORD count;
  while (pipe.read(bytes, sizeof(bytes), count))
    std::cout.write(bytes, count).flush();
  return 0;
}

int wmain(int argc, wchar_t **argv) {
  if (argc != 3) return 2;
  WindowsPipeFixture pipe;
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE log = CreateFileW(argv[2], GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
    &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = startup.hStdError = log; startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION child{};
  std::wstring command = L"\"" + std::wstring(argv[1]) + L"\" --control-pipe " + pipe.name;
  if (!CreateProcessW(argv[1], command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &child)) return 4;
  CloseHandle(child.hThread); CloseHandle(log);
  if (!pipe.accept()) { TerminateProcess(child.hProcess, 5); CloseHandle(child.hProcess); return 5; }
  HANDLE reader = CreateThread(nullptr, 0, readEvents, &pipe, 0, nullptr);
  if (!reader) return 6;
  std::string line;
  while (std::getline(std::cin, line)) {
    line.push_back('\n');
    if (!pipe.write(std::move(line))) break;
  }
  DWORD finished = WaitForSingleObject(child.hProcess, 10000);
  if (finished != WAIT_OBJECT_0) TerminateProcess(child.hProcess, 6);
  pipe.cancel(); WaitForSingleObject(reader, INFINITE); CloseHandle(reader);
  DWORD code = 0; GetExitCodeProcess(child.hProcess, &code);
  CloseHandle(child.hProcess);
  return int(code);
}
