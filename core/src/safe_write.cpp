#include "safe_write.hpp"
#include "path_safety.hpp"

#include <algorithm>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <winternl.h>
#  include <cwchar>
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace archiver {
namespace {

std::vector<std::string> split_relative(const std::string &relative) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : relative) {
    if (c == '/') {
      if (!cur.empty()) parts.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) parts.push_back(cur);
  return parts;
}

#if !defined(_WIN32)

struct Fd {
  int fd = -1;
  explicit Fd(int value = -1) : fd(value) {}
  ~Fd() {
    if (fd >= 0) ::close(fd);
  }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  Fd(Fd &&other) noexcept : fd(other.fd) { other.fd = -1; }
  Fd &operator=(Fd &&other) noexcept {
    if (this != &other) {
      if (fd >= 0) ::close(fd);
      fd = other.fd;
      other.fd = -1;
    }
    return *this;
  }
  int get() const { return fd; }
  int release() {
    int value = fd;
    fd = -1;
    return value;
  }
};

bool walk_parent(int root, const std::string &relative, bool create, Fd &parent,
                 std::string &leaf, std::string &error) {
  const std::vector<std::string> parts = split_relative(relative);
  if (parts.empty()) {
    error = "empty-path";
    return false;
  }
  Fd dir(::dup(root));
  if (dir.get() < 0) {
    error = "io";
    return false;
  }
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    if (create) {
      if (::mkdirat(dir.get(), parts[i].c_str(), 0755) != 0 && errno != EEXIST) {
        error = "io";
        return false;
      }
    }
    int next = ::openat(dir.get(), parts[i].c_str(),
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (next < 0) {
      error = (errno == ELOOP) ? "symlink-component" : "io";
      return false;
    }
    ::close(dir.release());
    dir = Fd(next);
  }
  parent = std::move(dir);
  leaf = parts.back();
  return true;
}

class PosixFile final : public SafeDest::File {
 public:
  PosixFile(int fd, Fd parent, std::string leaf)
      : fd_(fd), parent_(std::move(parent)), leaf_(std::move(leaf)) {}
  ~PosixFile() override {
    if (!committed_) ::unlinkat(parent_.get(), leaf_.c_str(), 0);
    if (fd_ >= 0) ::close(fd_);
  }
  bool commit(std::string & /*error*/) override {
    committed_ = true;
    return true;
  }
  bool write(const void *data, size_t size, std::string &error) override {
    const char *bytes = static_cast<const char *>(data);
    size_t done = 0;
    while (done < size) {
      ssize_t n = ::write(fd_, bytes + done, size - done);
      if (n < 0) {
        if (errno == EINTR) continue;
        error = "io";
        return false;
      }
      if (n == 0) {
        error = "io";
        return false;
      }
      done += static_cast<size_t>(n);
    }
    return true;
  }

 private:
  int fd_;
  Fd parent_;
  std::string leaf_;
  bool committed_ = false;
};

class PosixDest final : public SafeDest {
 public:
  explicit PosixDest(int fd) : fd_(fd) {}
  ~PosixDest() override {
    if (fd_ >= 0) ::close(fd_);
  }

  bool mkdir_entry(const std::string &relative, std::string &error) override {
    Fd parent;
    std::string leaf;
    if (!walk_parent(fd_, relative, true, parent, leaf, error)) return false;
    if (::mkdirat(parent.get(), leaf.c_str(), 0755) != 0 && errno != EEXIST) {
      error = "io";
      return false;
    }
    struct stat st;
    if (::fstatat(parent.get(), leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
      error = "io";
      return false;
    }
    if (!S_ISDIR(st.st_mode)) {
      error = "symlink-component";
      return false;
    }
    return true;
  }

  bool make_symlink(const std::string &relative, const std::string &target,
                    std::string &error) override {
    Fd parent;
    std::string leaf;
    if (!walk_parent(fd_, relative, true, parent, leaf, error)) return false;
    std::string stored;
    stored.reserve(target.size());
    for (char c : target) stored.push_back(c == '\\' ? '/' : c);
    if (::symlinkat(stored.c_str(), parent.get(), leaf.c_str()) != 0) {
      error = errno == EEXIST ? "destination-exists" : "io";
      return false;
    }
    return true;
  }

  std::unique_ptr<File> create_file(const std::string &relative,
                                     std::string &error) override {
    Fd parent;
    std::string leaf;
    if (!walk_parent(fd_, relative, true, parent, leaf, error)) return nullptr;
    int fd = ::openat(parent.get(), leaf.c_str(),
                      O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
      error = errno == EEXIST ? "destination-exists" :
              errno == ELOOP ? "symlink-component" : "io";
      return nullptr;
    }
    return std::unique_ptr<File>(new PosixFile(fd, std::move(parent), leaf));
  }

 private:
  int fd_;
};

#else  /* _WIN32 */

std::wstring widen(const std::string &utf8) {
  if (utf8.empty()) return std::wstring();
  int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                static_cast<int>(utf8.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                        static_cast<int>(utf8.size()), &out[0], n);
  return out;
}

class WinHandle {
 public:
  explicit WinHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
  ~WinHandle() { if (valid()) ::CloseHandle(value_); }
  WinHandle(const WinHandle &) = delete;
  WinHandle &operator=(const WinHandle &) = delete;
  WinHandle(WinHandle &&other) noexcept : value_(other.value_) {
    other.value_ = INVALID_HANDLE_VALUE;
  }
  WinHandle &operator=(WinHandle &&other) noexcept {
    if (this != &other) {
      if (valid()) ::CloseHandle(value_);
      value_ = other.value_;
      other.value_ = INVALID_HANDLE_VALUE;
    }
    return *this;
  }
  HANDLE get() const { return value_; }
  bool valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
 private:
  HANDLE value_;
};

bool plain_directory(HANDLE handle, std::string &error) {
  BY_HANDLE_FILE_INFORMATION info;
  if (!::GetFileInformationByHandle(handle, &info)) {
    error = "io";
    return false;
  }
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
      !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
    error = "symlink-component";
    return false;
  }
  return true;
}

WinHandle open_child(HANDLE parent, const std::string &part, bool directory,
                     std::string &error) {
  std::wstring name = widen(part);
  if (name.empty() || name.size() > 32767) {
    error = "invalid-name";
    return WinHandle();
  }
  UNICODE_STRING unicode;
  unicode.Buffer = &name[0];
  unicode.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
  unicode.MaximumLength = unicode.Length;
  OBJECT_ATTRIBUTES attrs = {};
  attrs.Length = sizeof(attrs);
  attrs.RootDirectory = parent;
  attrs.ObjectName = &unicode;
  attrs.Attributes = OBJ_CASE_INSENSITIVE;
  IO_STATUS_BLOCK status_block = {};
  HANDLE raw = INVALID_HANDLE_VALUE;
  // Open one component relative to the verified parent, never a string path
  // that could be redirected between checking and opening it.
  NTSTATUS status = ::NtCreateFile(
      &raw, directory ? FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE
                      : FILE_GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE,
      &attrs, &status_block, nullptr, FILE_ATTRIBUTE_NORMAL,
      directory ? FILE_SHARE_READ : 0, directory ? FILE_OPEN_IF : FILE_CREATE,
      FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT |
          (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE),
      nullptr, 0);
  if (status < 0) {
    error = static_cast<ULONG>(status) == 0xC0000035UL ? "destination-exists" : "io";
    return WinHandle();
  }
  WinHandle handle(raw);
  if (directory && !plain_directory(handle.get(), error)) return WinHandle();
  return handle;
}

class WinFile final : public SafeDest::File {
 public:
  WinFile(WinHandle handle, std::vector<WinHandle> parents)
      : handle_(std::move(handle)), parents_(std::move(parents)) {}
  ~WinFile() override {
    if (!committed_) {
      FILE_DISPOSITION_INFO info = {TRUE};
      ::SetFileInformationByHandle(handle_.get(), FileDispositionInfo, &info, sizeof(info));
    }
  }
  bool commit(std::string & /*error*/) override {
    committed_ = true;
    return true;
  }
  bool write(const void *data, size_t size, std::string &error) override {
    const char *bytes = static_cast<const char *>(data);
    size_t done = 0;
    while (done < size) {
      DWORD chunk = static_cast<DWORD>(std::min(size - done, static_cast<size_t>(1 << 20)));
      DWORD wrote = 0;
      if (!::WriteFile(handle_.get(), bytes + done, chunk, &wrote, nullptr) || wrote == 0) {
        error = "io";
        return false;
      }
      done += wrote;
    }
    return true;
  }

 private:
  WinHandle handle_;
  std::vector<WinHandle> parents_;
  bool committed_ = false;
};

class WinDest final : public SafeDest {
 public:
  explicit WinDest(WinHandle root) : root_(std::move(root)) {}

  bool mkdir_entry(const std::string &relative, std::string &error) override {
    std::vector<WinHandle> parents;
    std::string leaf;
    if (!walk(relative, parents, leaf, error)) return false;
    WinHandle dir = open_child(parents.empty() ? root_.get() : parents.back().get(),
                               leaf, true, error);
    return dir.valid();
  }

  bool make_symlink(const std::string &relative, const std::string &target,
                    std::string &error) override {
    /* Not run in the phase 1 environment. Refuse rather than guess. */
    (void)relative;
    (void)target;
    error = "windows-symlink-untested";
    return false;
  }

  std::unique_ptr<File> create_file(const std::string &relative,
                                     std::string &error) override {
    std::vector<WinHandle> parents;
    std::string leaf;
    if (!walk(relative, parents, leaf, error)) return nullptr;
    WinHandle file = open_child(parents.empty() ? root_.get() : parents.back().get(),
                                leaf, false, error);
    if (!file.valid()) return nullptr;
    return std::unique_ptr<File>(new WinFile(std::move(file), std::move(parents)));
  }

 private:
  bool walk(const std::string &relative, std::vector<WinHandle> &parents,
            std::string &leaf, std::string &error) {
    CheckedPath checked = check_entry_path(relative);
    if (checked.reject != PathReject::Ok) {
      error = "invalid-name";
      return false;
    }
    const std::vector<std::string> parts = split_relative(checked.relative);
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
      WinHandle dir = open_child(parents.empty() ? root_.get() : parents.back().get(),
                                 parts[i], true, error);
      if (!dir.valid()) return false;
      parents.push_back(std::move(dir));
    }
    leaf = parts.back();
    return true;
  }

  WinHandle root_;
};

#endif

}  /* namespace */

std::unique_ptr<SafeDest> SafeDest::open(const std::string &path, std::string &error) {
#if !defined(_WIN32)
  if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
    error = "io";
    return nullptr;
  }
  int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    error = "io";
    return nullptr;
  }
  return std::unique_ptr<SafeDest>(new PosixDest(fd));
#else
  std::wstring wide = widen(path);
  if (wide.empty()) {
    error = "io";
    return nullptr;
  }
  if (!::CreateDirectoryW(wide.c_str(), nullptr) &&
      ::GetLastError() != ERROR_ALREADY_EXISTS) {
    error = "io";
    return nullptr;
  }
  WinHandle root(::CreateFileW(wide.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                               nullptr));
  if (!root.valid()) {
    error = "io";
    return nullptr;
  }
  if (!plain_directory(root.get(), error)) return nullptr;
  return std::unique_ptr<SafeDest>(new WinDest(std::move(root)));
#endif
}

}  /* namespace archiver */
