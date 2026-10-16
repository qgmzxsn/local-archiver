#include "safe_write.hpp"

#include <algorithm>
#include <cwctype>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
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
  explicit PosixFile(int fd) : fd_(fd) {}
  ~PosixFile() override {
    if (fd_ >= 0) ::close(fd_);
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
    struct stat st;
    if (::fstatat(parent.get(), leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
      if (S_ISDIR(st.st_mode)) {
        error = "io";
        return false;
      }
      if (::unlinkat(parent.get(), leaf.c_str(), 0) != 0) {
        error = "io";
        return false;
      }
    }
    std::string stored;
    stored.reserve(target.size());
    for (char c : target) stored.push_back(c == '\\' ? '/' : c);
    if (::symlinkat(stored.c_str(), parent.get(), leaf.c_str()) != 0) {
      error = "io";
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
                      O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
      error = (errno == ELOOP) ? "symlink-component" : "io";
      return nullptr;
    }
    return std::unique_ptr<File>(new PosixFile(fd));
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

bool under_root(const std::wstring &root, const std::wstring &full) {
  if (full.size() < root.size()) return false;
  for (size_t i = 0; i < root.size(); ++i) {
    wchar_t a = root[i];
    wchar_t b = full[i];
    if (a == L'/') a = L'\\';
    if (b == L'/') b = L'\\';
    if (towlower(a) != towlower(b)) return false;
  }
  if (full.size() == root.size()) return true;
  return full[root.size()] == L'\\' || root.back() == L'\\';
}

class WinFile final : public SafeDest::File {
 public:
  explicit WinFile(HANDLE handle) : handle_(handle) {}
  ~WinFile() override {
    if (handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
  }
  bool write(const void *data, size_t size, std::string &error) override {
    const char *bytes = static_cast<const char *>(data);
    size_t done = 0;
    while (done < size) {
      DWORD chunk = static_cast<DWORD>(std::min(size - done, static_cast<size_t>(1 << 20)));
      DWORD wrote = 0;
      if (!::WriteFile(handle_, bytes + done, chunk, &wrote, nullptr) || wrote == 0) {
        error = "io";
        return false;
      }
      done += wrote;
    }
    return true;
  }

 private:
  HANDLE handle_;
};

class WinDest final : public SafeDest {
 public:
  explicit WinDest(std::wstring root) : root_(std::move(root)) {}

  bool mkdir_entry(const std::string &relative, std::string &error) override {
    std::wstring full;
    if (!resolve(relative, full, true, error)) return false;
    if (!::CreateDirectoryW(full.c_str(), nullptr) &&
        ::GetLastError() != ERROR_ALREADY_EXISTS) {
      error = "io";
      return false;
    }
    return true;
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
    std::wstring full;
    if (!resolve(relative, full, true, error)) return nullptr;
    HANDLE handle = ::CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      error = "io";
      return nullptr;
    }
    return std::unique_ptr<File>(new WinFile(handle));
  }

 private:
  bool resolve(const std::string &relative, std::wstring &full, bool create_parents,
               std::string &error) {
    const std::vector<std::string> parts = split_relative(relative);
    if (parts.empty()) {
      error = "empty-path";
      return false;
    }
    std::wstring built = root_;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
      built.push_back(L'\\');
      built += widen(parts[i]);
      if (create_parents) {
        if (!::CreateDirectoryW(built.c_str(), nullptr) &&
            ::GetLastError() != ERROR_ALREADY_EXISTS) {
          error = "io";
          return false;
        }
      }
    }
    built.push_back(L'\\');
    built += widen(parts.back());
    wchar_t resolved[MAX_PATH * 4];
    DWORD n = ::GetFullPathNameW(built.c_str(),
                                 static_cast<DWORD>(sizeof(resolved) / sizeof(resolved[0])),
                                 resolved, nullptr);
    if (n == 0 || n >= sizeof(resolved) / sizeof(resolved[0])) {
      error = "io";
      return false;
    }
    if (!under_root(root_, resolved)) {
      error = "escapes";
      return false;
    }
    full = resolved;
    return true;
  }

  std::wstring root_;
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
  wchar_t resolved[MAX_PATH * 4];
  DWORD n = ::GetFullPathNameW(wide.c_str(),
                               static_cast<DWORD>(sizeof(resolved) / sizeof(resolved[0])),
                               resolved, nullptr);
  if (n == 0) {
    error = "io";
    return nullptr;
  }
  return std::unique_ptr<SafeDest>(new WinDest(resolved));
#endif
}

}  /* namespace archiver */
