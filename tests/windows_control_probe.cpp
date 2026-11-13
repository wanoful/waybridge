// A real Win32 named-pipe host for the native worker, driven from Python/Wine.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <string>

static DWORD WINAPI readEvents(void *context) {
  HANDLE pipe = static_cast<HANDLE>(context);
  char bytes[8192]; DWORD count;
  while (ReadFile(pipe, bytes, sizeof(bytes), &count, nullptr) && count)
    std::cout.write(bytes, count).flush();
  return 0;
}

int wmain(int argc, wchar_t **argv) {
  if (argc != 3) return 2;
  std::wstring name = L"waybridge-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
  HANDLE pipe = CreateNamedPipeW((L"\\\\.\\pipe\\" + name).c_str(), PIPE_ACCESS_DUPLEX,
    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
    1, 65536, 65536, 10000, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return 3;
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE log = CreateFileW(argv[2], GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
    &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = startup.hStdError = log; startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION child{};
  std::wstring command = L"\"" + std::wstring(argv[1]) + L"\" --control-pipe " + name;
  if (!CreateProcessW(argv[1], command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &child)) return 4;
  CloseHandle(child.hThread); CloseHandle(log);
  if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return 5;
  HANDLE reader = CreateThread(nullptr, 0, readEvents, pipe, 0, nullptr);
  if (!reader) return 6;
  std::string line;
  while (std::getline(std::cin, line)) {
    line.push_back('\n'); size_t position = 0; DWORD count;
    while (position < line.size()) {
      if (!WriteFile(pipe, line.data() + position, DWORD(line.size() - position), &count, nullptr) || !count) break;
      position += count;
    }
    if (position != line.size()) break;
  }
  DWORD finished = WaitForSingleObject(child.hProcess, 10000);
  if (finished != WAIT_OBJECT_0) TerminateProcess(child.hProcess, 6);
  CancelSynchronousIo(reader); WaitForSingleObject(reader, 5000); CloseHandle(reader);
  DWORD code = 0; GetExitCodeProcess(child.hProcess, &code);
  CloseHandle(child.hProcess); CloseHandle(pipe);
  return int(code);
}
