// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "process.hpp"

#include <cstdio>
#include <cstring>
#include <sstream>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <signal.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace hgmt {
namespace {

#if defined(_WIN32)
std::wstring widen(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

std::string quote(const std::string& argument) {
  std::string out = "\"";
  for (const char character : argument) {
    if (character == '"') out.push_back('\\');
    out.push_back(character);
  }
  out.push_back('"');
  return out;
}
#endif

}  // namespace

bool ChildProcess::spawn(const std::string& executable, const std::vector<std::string>& args,
                         const std::filesystem::path& stdout_path) {
  if (spawned_) return false;
#if defined(_WIN32)
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE output = ::CreateFileW(stdout_path.wstring().c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output == INVALID_HANDLE_VALUE) return false;

  std::string command = quote(executable);
  for (const std::string& argument : args) {
    command.push_back(' ');
    command.append(quote(argument));
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = output;
  startup.hStdError = output;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION info{};
  std::wstring wide_command = widen(command);
  const BOOL created = ::CreateProcessW(nullptr, wide_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
  ::CloseHandle(output);
  if (created == 0) return false;
  ::CloseHandle(info.hThread);
  process_ = info.hProcess;
  spawned_ = true;
  reaped_ = false;
  return true;
#else
  const std::string command = executable;
  std::vector<std::string> storage;
  storage.push_back(executable);
  for (const std::string& argument : args) storage.push_back(argument);
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (std::string& entry : storage) argv.push_back(entry.data());
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) return false;
  if (pid == 0) {
    const int fd = ::open(stdout_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
      ::dup2(fd, STDOUT_FILENO);
      ::dup2(fd, STDERR_FILENO);
      ::close(fd);
    }
    ::execv(command.c_str(), argv.data());
    ::_exit(127);
  }
  pid_ = pid;
  spawned_ = true;
  reaped_ = false;
  return true;
#endif
}

bool ChildProcess::running() const {
  if (!spawned_ || reaped_) return false;
#if defined(_WIN32)
  return ::WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
#else
  int status = 0;
  const pid_t result = ::waitpid(pid_, &status, WNOHANG);
  return result == 0;
#endif
}

int ChildProcess::wait() {
  if (!spawned_) return -1;
  if (reaped_) return exit_code_;
#if defined(_WIN32)
  ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  DWORD code = 0;
  ::GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
  exit_code_ = static_cast<int>(code);
#else
  int status = 0;
  ::waitpid(pid_, &status, 0);
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  reaped_ = true;
  return exit_code_;
}

void ChildProcess::kill() {
  if (!spawned_ || reaped_) return;
#if defined(_WIN32)
  ::TerminateProcess(static_cast<HANDLE>(process_), 137);
  ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  ::CloseHandle(static_cast<HANDLE>(process_));
  process_ = nullptr;
#else
  ::kill(pid_, SIGKILL);
  int status = 0;
  ::waitpid(pid_, &status, 0);
  pid_ = -1;
#endif
  reaped_ = true;
  exit_code_ = 137;
  spawned_ = false;
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return std::string();
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

bool file_contains(const std::filesystem::path& path, const std::string& needle) {
  return read_text(path).find(needle) != std::string::npos;
}

bool wait_for_line(const std::filesystem::path& path, const std::string& needle, unsigned attempts,
                   unsigned sleep_ms) {
  for (unsigned attempt = 0; attempt < attempts; ++attempt) {
    if (file_contains(path, needle)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
  return file_contains(path, needle);
}

std::string extract_field(const std::filesystem::path& path, const std::string& prefix,
                          const std::string& key) {
  const std::string text = read_text(path);
  std::istringstream lines(text);
  std::string line;
  const std::string needle = key + "=";
  std::string found;
  while (std::getline(lines, line)) {
    // The latest matching line wins, so callers observe the newest state.
    if (line.find(prefix) == std::string::npos) continue;
    const std::size_t position = line.find(needle);
    if (position == std::string::npos) continue;
    const std::size_t start = position + needle.size();
    std::size_t end = start;
    while (end < line.size() && line[end] != ' ' && line[end] != '\r' && line[end] != '\n') ++end;
    found = line.substr(start, end - start);
  }
  return found;
}

}  // namespace hgmt
