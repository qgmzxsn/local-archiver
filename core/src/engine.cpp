/* Loads 7-Zip at runtime (dlopen / LoadLibrary). This file does not compile
   7-Zip into the binary. 7-Zip stays LGPL in its own shared library.
   RAR and RAR5 are opened for extraction only. Nothing here creates RAR. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <stdlib.h>

#if defined(_WIN32)
/* Windows headers already define GUID. Do not include 7-Zip's MyGuidDef.h first. */
#  include "Common/MyWindows.h"
#  define INITGUID
#  include <initguid.h>
#else
#  define INITGUID
#  include "Common/MyGuidDef.h"
#  include "Common/MyWindows.h"
#endif
#include "Common/Common0.h"
#include "7zip/IStream.h"
#include "7zip/IProgress.h"
#include "7zip/IPassword.h"
#include "7zip/Archive/IArchive.h"
#include "Common/MyCom.h"

#include "archiver.h"
#include "path_safety.hpp"
#include "safe_write.hpp"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

constexpr uint64_t kWriteCap = 64ull << 30;
constexpr uint32_t kMaxEntries = 1000000;
constexpr size_t kMaxSymlink = 4096;

void wipe_bytes(void *data, size_t size) {
  volatile unsigned char *p = static_cast<volatile unsigned char *>(data);
  while (size--) *p++ = 0;
}

void wipe_string(std::string &s) {
  if (!s.empty()) wipe_bytes(&s[0], s.size());
  s.clear();
}

void wipe_wide(std::wstring &s) {
  if (!s.empty()) wipe_bytes(&s[0], s.size() * sizeof(wchar_t));
  s.clear();
}

struct PasswordState {
  std::wstring text;
  bool provided = false;
  bool asked = false;
  ~PasswordState() { wipe_wide(text); }
};

void utf8_append(std::string &out, uint32_t cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::string wide_to_utf8(const wchar_t *w, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; ++i) {
    uint32_t cp = static_cast<uint32_t>(w[i]);
    if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n) {
      uint32_t low = static_cast<uint32_t>(w[i + 1]);
      if (low >= 0xDC00 && low <= 0xDFFF) {
        cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
        ++i;
      }
    }
    utf8_append(out, cp);
  }
  return out;
}

std::wstring utf8_to_wide(const char *s) {
  std::wstring out;
  if (!s) return out;
  const unsigned char *p = reinterpret_cast<const unsigned char *>(s);
  while (*p) {
    uint32_t cp = 0;
    if (*p < 0x80) {
      cp = *p++;
    } else if ((*p & 0xE0) == 0xC0 && p[1]) {
      cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
      p += 2;
    } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
      cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
      p += 3;
    } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
      cp = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
      p += 4;
    } else {
      cp = *p++;
    }
    if (sizeof(wchar_t) == 2 && cp > 0xFFFF) {
      cp -= 0x10000;
      out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<wchar_t>(cp));
    }
  }
  return out;
}

uint32_t bstr_bytes(BSTR b) {
  if (!b) return 0;
#if defined(_WIN32)
  return ::SysStringByteLen(b);
#else
  return *(reinterpret_cast<uint32_t *>(b) - 1);
#endif
}

BSTR alloc_wide(const wchar_t *s, size_t n) {
  if (n > 0x0fffffffu / sizeof(wchar_t)) return nullptr;
#if defined(_WIN32)
  return ::SysAllocStringLen(s, static_cast<UINT>(n));
#else
  uint32_t bytes = static_cast<uint32_t>(n * sizeof(wchar_t));
  void *mem = malloc(bytes + sizeof(uint32_t) + sizeof(wchar_t));
  if (!mem) return nullptr;
  *static_cast<uint32_t *>(mem) = bytes;
  wchar_t *b = reinterpret_cast<wchar_t *>(static_cast<uint32_t *>(mem) + 1);
  if (n) memcpy(b, s, bytes);
  b[n] = 0;
  return b;
#endif
}

void free_bstr(BSTR b) {
  if (!b) return;
#if defined(_WIN32)
  ::SysFreeString(b);
#else
  free(reinterpret_cast<uint32_t *>(b) - 1);
#endif
}

void prop_clear(PROPVARIANT *v) {
  if (v->vt == VT_BSTR) free_bstr(v->bstrVal);
  memset(v, 0, sizeof(*v));
}

bool bstr_utf8(BSTR b, std::string &out, bool &embedded_nul) {
  embedded_nul = false;
  out.clear();
  if (!b) return true;
  uint32_t chars = bstr_bytes(b) / static_cast<uint32_t>(sizeof(wchar_t));
  size_t slen = 0;
  while (slen < chars && b[slen] != 0) ++slen;
  if (slen != chars) embedded_nul = true;
  out = wide_to_utf8(b, slen);
  return true;
}

enum class Kind {
  Zip,
  SevenZ,
  Tar,
  TarGz,
  TarBz2,
  TarXz,
  TarZst,
  Rar,
  Split,
  Unsupported
};

std::string basename_lower(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
  for (char &c : base) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return base;
}

bool ends_with(const std::string &s, const char *suf) {
  size_t n = strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

bool all_digits(const std::string &s) {
  if (s.empty()) return false;
  for (char c : s)
    if (c < '0' || c > '9') return false;
  return true;
}

bool is_split_name(const std::string &base) {
  size_t dot = base.rfind('.');
  if (dot == std::string::npos) return false;
  std::string ext = base.substr(dot + 1);
  if (ext.size() == 3 && all_digits(ext)) return true;
  if (ext.size() == 3 && (ext[0] == 'z' || ext[0] == 'r') && ext[1] >= '0' && ext[1] <= '9' &&
      ext[2] >= '0' && ext[2] <= '9') {
    return true;
  }
  if (ends_with(base, ".rar")) {
    std::string stem = base.substr(0, base.size() - 4);
    size_t part = stem.rfind(".part");
    if (part != std::string::npos && all_digits(stem.substr(part + 5))) return true;
  }
  return false;
}

Kind classify(const std::string &path) {
  std::string base = basename_lower(path);
  if (is_split_name(base)) return Kind::Split;
  if (ends_with(base, ".tar.gz") || ends_with(base, ".tgz")) return Kind::TarGz;
  if (ends_with(base, ".tar.bz2") || ends_with(base, ".tbz2") || ends_with(base, ".tbz"))
    return Kind::TarBz2;
  if (ends_with(base, ".tar.xz") || ends_with(base, ".txz")) return Kind::TarXz;
  if (ends_with(base, ".tar.zst") || ends_with(base, ".tzst")) return Kind::TarZst;
  if (ends_with(base, ".tar")) return Kind::Tar;
  if (ends_with(base, ".7z")) return Kind::SevenZ;
  if (ends_with(base, ".zip")) return Kind::Zip;
  if (ends_with(base, ".rar")) return Kind::Rar;
  return Kind::Unsupported;
}

/* .mp4 rename, and a video or image with an archive glued on the end.
   Not a general "any extension" scan, and not pixel steganography. */
bool is_disguise_carrier(const std::string &path) {
  std::string base = basename_lower(path);
  return ends_with(base, ".mp4") || ends_with(base, ".m4v") || ends_with(base, ".mov") ||
         ends_with(base, ".jpg") || ends_with(base, ".jpeg") || ends_with(base, ".png") ||
         ends_with(base, ".gif") || ends_with(base, ".bmp") || ends_with(base, ".webp");
}

using CreateObjectFn = HRESULT(STDAPICALLTYPE *)(const GUID *, const GUID *, void **);
using GetNumberOfFormatsFn = HRESULT(STDAPICALLTYPE *)(UInt32 *);
using GetHandlerProperty2Fn = HRESULT(STDAPICALLTYPE *)(UInt32, PROPID, PROPVARIANT *);

struct EngineLib {
  void *handle = nullptr;
  CreateObjectFn CreateObject = nullptr;
  GetNumberOfFormatsFn GetNumberOfFormats = nullptr;
  GetHandlerProperty2Fn GetHandlerProperty2 = nullptr;

  ~EngineLib() { close(); }
  EngineLib() = default;
  EngineLib(const EngineLib &) = delete;
  EngineLib &operator=(const EngineLib &) = delete;

  void close() {
    if (!handle) return;
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
    handle = nullptr;
    CreateObject = nullptr;
    GetNumberOfFormats = nullptr;
    GetHandlerProperty2 = nullptr;
  }
};

struct ListedEntry {
  std::string path;
  bool is_dir = false;
  bool is_symlink = false;
  uint64_t size = 0;
};

struct ArchiverArchive {
  EngineLib engine;
  CMyComPtr<IInStream> stream;
  CMyComPtr<IInArchive> archive;
  std::vector<ListedEntry> entries;
  std::string message;
  std::string temp_dir;

  ~ArchiverArchive() {
    archive.Release();
    stream.Release();
    engine.close();
    if (!temp_dir.empty()) {
      std::string inner = temp_dir + "/inner.bin";
#if defined(_WIN32)
      std::wstring wide_inner = utf8_to_wide(inner.c_str());
      std::wstring wide_dir = utf8_to_wide(temp_dir.c_str());
      ::DeleteFileW(wide_inner.c_str());
      ::RemoveDirectoryW(wide_dir.c_str());
#else
      ::unlink(inner.c_str());
      ::rmdir(temp_dir.c_str());
#endif
      temp_dir.clear();
    }
  }
};

std::string library_dir() {
#if defined(_WIN32)
  HMODULE module = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&archiver_open), &module)) {
    return std::string();
  }
  wchar_t buf[MAX_PATH * 4];
  DWORD n = ::GetModuleFileNameW(module, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
  if (n == 0) return std::string();
  std::string path = wide_to_utf8(buf, n);
  size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) return std::string();
  return path.substr(0, slash);
#else
  Dl_info info;
  memset(&info, 0, sizeof(info));
  if (::dladdr(reinterpret_cast<void *>(&archiver_open), &info) == 0 || !info.dli_fname) {
    return std::string();
  }
  std::string path = info.dli_fname;
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return std::string();
  return path.substr(0, slash);
#endif
}

bool load_engine(EngineLib &lib, const char *engine_path, std::string &message) {
  std::string path;
  if (engine_path && engine_path[0]) {
    path = engine_path;
  } else if (const char *env = getenv("ARCHIVER_7ZIP")) {
    if (env[0]) path = env;
  }
  std::vector<std::string> tries;
  if (!path.empty()) tries.push_back(path);
  std::string dir = library_dir();
  if (!dir.empty()) {
    tries.push_back(dir + "/7z.so");
    tries.push_back(dir + "/7z.dll");
    tries.push_back(dir + "/lib7z.dylib");
  }
  for (const std::string &candidate : tries) {
#if defined(_WIN32)
    std::wstring wide = utf8_to_wide(candidate.c_str());
    HMODULE module = ::LoadLibraryW(wide.c_str());
    if (!module) continue;
    lib.handle = module;
    lib.CreateObject = reinterpret_cast<CreateObjectFn>(::GetProcAddress(module, "CreateObject"));
    lib.GetNumberOfFormats =
        reinterpret_cast<GetNumberOfFormatsFn>(::GetProcAddress(module, "GetNumberOfFormats"));
    lib.GetHandlerProperty2 =
        reinterpret_cast<GetHandlerProperty2Fn>(::GetProcAddress(module, "GetHandlerProperty2"));
#else
    void *module = ::dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module) continue;
    lib.handle = module;
    lib.CreateObject = reinterpret_cast<CreateObjectFn>(::dlsym(module, "CreateObject"));
    lib.GetNumberOfFormats =
        reinterpret_cast<GetNumberOfFormatsFn>(::dlsym(module, "GetNumberOfFormats"));
    lib.GetHandlerProperty2 =
        reinterpret_cast<GetHandlerProperty2Fn>(::dlsym(module, "GetHandlerProperty2"));
#endif
    if (lib.CreateObject && lib.GetNumberOfFormats && lib.GetHandlerProperty2) return true;
    lib.close();
  }
  message = "7-Zip shared library was not found. Pass the path to 7z.so, 7z.dll, or lib7z.dylib.";
  return false;
}

bool iequals(const std::string &a, const char *b) {
  size_t n = strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; ++i) {
    char c = a[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    char d = b[i];
    if (d >= 'A' && d <= 'Z') d = static_cast<char>(d - 'A' + 'a');
    if (c != d) return false;
  }
  return true;
}

bool guid_from_prop(PROPVARIANT &prop, GUID &out) {
  if (prop.vt != VT_BSTR || !prop.bstrVal) return false;
  if (bstr_bytes(prop.bstrVal) < sizeof(GUID)) return false;
  memcpy(&out, prop.bstrVal, sizeof(GUID));
  return true;
}

bool find_handler(EngineLib &lib, const char *name, GUID &out) {
  UInt32 count = 0;
  if (lib.GetNumberOfFormats(&count) != S_OK) return false;
  for (UInt32 i = 0; i < count; ++i) {
    PROPVARIANT prop;
    memset(&prop, 0, sizeof(prop));
    if (lib.GetHandlerProperty2(i, NArchive::NHandlerPropID::kName, &prop) != S_OK) {
      prop_clear(&prop);
      continue;
    }
    std::string got;
    bool nul = false;
    bstr_utf8(prop.bstrVal, got, nul);
    prop_clear(&prop);
    if (!iequals(got, name)) continue;
    memset(&prop, 0, sizeof(prop));
    if (lib.GetHandlerProperty2(i, NArchive::NHandlerPropID::kClassID, &prop) != S_OK) {
      prop_clear(&prop);
      return false;
    }
    bool ok = guid_from_prop(prop, out);
    prop_clear(&prop);
    return ok;
  }
  return false;
}

class FileInStream Z7_final : public IInStream,
                              public IStreamGetSize,
                              public CMyUnknownImp {
  Z7_IFACES_IMP_UNK_3(IInStream, ISequentialInStream, IStreamGetSize)
 public:
#if defined(_WIN32)
  HANDLE handle = INVALID_HANDLE_VALUE;
  ~FileInStream() {
    if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle);
  }
#else
  int fd = -1;
  ~FileInStream() {
    if (fd >= 0) ::close(fd);
  }
#endif
  /* Visible range inside the real file. A renamed or appended archive is
     opened as this slice so 7-Zip does not have to search the carrier. */
  uint64_t view_base = 0;
  uint64_t view_size = 0;
  uint64_t view_pos = 0;

  bool seek_phys(uint64_t pos) {
#if defined(_WIN32)
    LARGE_INTEGER move;
    move.QuadPart = static_cast<LONGLONG>(pos);
    return ::SetFilePointerEx(handle, move, nullptr, FILE_BEGIN) != 0;
#else
    return ::lseek(fd, static_cast<off_t>(pos), SEEK_SET) >= 0;
#endif
  }
};

Z7_COM7F_IMF(FileInStream::Read(void *data, UInt32 size, UInt32 *processedSize)) {
  if (processedSize) *processedSize = 0;
  if (view_pos >= view_size) return S_OK;
  uint64_t remain = view_size - view_pos;
  uint32_t want = size;
  if (static_cast<uint64_t>(want) > remain) want = static_cast<uint32_t>(remain);
  if (want == 0) return S_OK;
  if (!seek_phys(view_base + view_pos)) return E_FAIL;
#if defined(_WIN32)
  DWORD got = 0;
  if (!::ReadFile(handle, data, want, &got, nullptr)) return E_FAIL;
  view_pos += got;
  if (processedSize) *processedSize = static_cast<UInt32>(got);
#else
  ssize_t n = ::read(fd, data, want);
  if (n < 0) return E_FAIL;
  view_pos += static_cast<uint64_t>(n);
  if (processedSize) *processedSize = static_cast<UInt32>(n);
#endif
  return S_OK;
}

Z7_COM7F_IMF(FileInStream::Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition)) {
  int64_t next = 0;
  if (seekOrigin == 0) next = offset;
  else if (seekOrigin == 1) next = static_cast<int64_t>(view_pos) + offset;
  else if (seekOrigin == 2) next = static_cast<int64_t>(view_size) + offset;
  else return E_INVALIDARG;
  if (next < 0) return HRESULT_WIN32_ERROR_NEGATIVE_SEEK;
  view_pos = static_cast<uint64_t>(next);
  if (newPosition) *newPosition = view_pos;
  return S_OK;
}

Z7_COM7F_IMF(FileInStream::GetSize(UInt64 *size)) {
  *size = view_size;
  return S_OK;
}

class OpenCallback Z7_final : public IArchiveOpenCallback,
                              public ICryptoGetTextPassword,
                              public CMyUnknownImp {
  Z7_IFACES_IMP_UNK_2(IArchiveOpenCallback, ICryptoGetTextPassword)
 public:
  PasswordState *password = nullptr;
};

Z7_COM7F_IMF(OpenCallback::SetTotal(const UInt64 * /*files*/, const UInt64 * /*bytes*/)) {
  return S_OK;
}
Z7_COM7F_IMF(OpenCallback::SetCompleted(const UInt64 * /*files*/, const UInt64 * /*bytes*/)) {
  return S_OK;
}
Z7_COM7F_IMF(OpenCallback::CryptoGetTextPassword(BSTR *passwordOut)) {
  if (!password) return E_ABORT;
  password->asked = true;
  if (!password->provided) return E_ABORT;
  *passwordOut = alloc_wide(password->text.c_str(), password->text.size());
  return *passwordOut ? S_OK : E_OUTOFMEMORY;
}

struct ExtractCtx {
  PasswordState *password = nullptr;
  archiver::SafeDest *dest = nullptr;
  std::string *message = nullptr;
  bool path_failed = false;
  bool io_failed = false;
  bool unsupported = false;
  bool wrong_password = false;
  bool data_error = false;
  bool pending_link = false;
  std::string pending_path;
  std::string *link_buf = nullptr;
  uint64_t written = 0;
  uint64_t total = 0;
  ArchiverProgressCallback progress = nullptr;
  void *progress_user = nullptr;
  bool cancelled = false;

  bool report_progress() {
    if (cancelled) return false;
    if (progress && progress(written, total, progress_user)) {
      cancelled = true;
      if (message) *message = "extraction cancelled";
      return false;
    }
    return true;
  }
};

class FileOutStream Z7_final : public ISequentialOutStream, public CMyUnknownImp {
  Z7_IFACES_IMP_UNK_1(ISequentialOutStream)
 public:
  std::unique_ptr<archiver::SafeDest::File> file;
  ExtractCtx *ctx = nullptr;
  ~FileOutStream() = default;
};

Z7_COM7F_IMF(FileOutStream::Write(const void *data, UInt32 size, UInt32 *processedSize)) {
  if (processedSize) *processedSize = 0;
  if (!file || !ctx) return E_FAIL;
  if (!ctx->report_progress()) return E_ABORT;
  if (size > kWriteCap || ctx->written > kWriteCap - size) {
    ctx->io_failed = true;
    if (ctx->message) *ctx->message = "output is larger than this build will write";
    return E_ABORT;
  }
  std::string error;
  if (!file->write(data, size, error)) {
    ctx->io_failed = true;
    if (ctx->message) *ctx->message = "could not write output";
    return E_FAIL;
  }
  ctx->written += size;
  if (processedSize) *processedSize = size;
  if (!ctx->report_progress()) return E_ABORT;
  return S_OK;
}

class MemOutStream Z7_final : public ISequentialOutStream, public CMyUnknownImp {
  Z7_IFACES_IMP_UNK_1(ISequentialOutStream)
 public:
  std::string *dest = nullptr;
  ExtractCtx *ctx = nullptr;
};

Z7_COM7F_IMF(MemOutStream::Write(const void *data, UInt32 size, UInt32 *processedSize)) {
  if (processedSize) *processedSize = 0;
  if (!dest) return E_FAIL;
  if (dest->size() + size > kMaxSymlink) {
    if (ctx) {
      ctx->path_failed = true;
      if (ctx->message) *ctx->message = "symlink target is too long";
    }
    return E_ABORT;
  }
  dest->append(static_cast<const char *>(data), size);
  if (processedSize) *processedSize = size;
  return S_OK;
}

void note_write_error(ExtractCtx *ctx, const std::string &error) {
  if (error == "io" || error == "destination-exists") {
    ctx->io_failed = true;
    if (ctx->message && ctx->message->empty()) {
      *ctx->message = error == "destination-exists"
                          ? "destination file already exists; nothing was overwritten"
                          : "could not write output";
    }
    return;
  }
  if (error == "windows-symlink-untested") {
    ctx->unsupported = true;
    if (ctx->message) *ctx->message = "Windows symlink extraction was not verified and is refused";
    return;
  }
  ctx->path_failed = true;
  if (ctx->message && ctx->message->empty()) *ctx->message = "refused path";
}

class ExtractCallback Z7_final : public IArchiveExtractCallback,
                                 public ICryptoGetTextPassword,
                                 public CMyUnknownImp {
  Z7_COM_UNKNOWN_IMP_SPEC(Z7_COM_QI_ENTRY_UNKNOWN(IArchiveExtractCallback)
                              Z7_COM_QI_ENTRY(IArchiveExtractCallback)
                                  Z7_COM_QI_ENTRY(IProgress) Z7_COM_QI_ENTRY(ICryptoGetTextPassword))
  Z7_IFACE_COM7_IMP(IProgress)
  Z7_IFACE_COM7_IMP(IArchiveExtractCallback)
  Z7_IFACE_COM7_IMP(ICryptoGetTextPassword)
 public:
  IInArchive *archive = nullptr;
  ExtractCtx *ctx = nullptr;
  std::string link_buf;
  CMyComPtr<ISequentialOutStream> active_output;
  archiver::SafeDest::File *active_file = nullptr;
};

bool prop_bool(const PROPVARIANT &prop) {
  return prop.vt == VT_BOOL && prop.boolVal != VARIANT_FALSE;
}

uint64_t prop_u64(const PROPVARIANT &prop) {
  if (prop.vt == VT_UI8) return prop.uhVal.QuadPart;
  if (prop.vt == VT_UI4) return prop.ulVal;
  if (prop.vt == VT_UI2) return prop.uiVal;
  if (prop.vt == VT_I8 && prop.hVal.QuadPart > 0) return static_cast<uint64_t>(prop.hVal.QuadPart);
  return 0;
}

HRESULT read_string_prop(IInArchive *archive, UInt32 index, PROPID id, std::string &out,
                         bool &embedded_nul) {
  PROPVARIANT prop;
  memset(&prop, 0, sizeof(prop));
  HRESULT hr = archive->GetProperty(index, id, &prop);
  if (hr != S_OK) {
    prop_clear(&prop);
    return hr;
  }
  if (prop.vt == VT_EMPTY || prop.vt == VT_NULL) {
    out.clear();
    embedded_nul = false;
    prop_clear(&prop);
    return S_OK;
  }
  if (prop.vt != VT_BSTR) {
    prop_clear(&prop);
    out.clear();
    return S_OK;
  }
  bstr_utf8(prop.bstrVal, out, embedded_nul);
  prop_clear(&prop);
  return S_OK;
}

Z7_COM7F_IMF(ExtractCallback::SetTotal(UInt64 total)) {
  if (!ctx) return E_FAIL;
  ctx->total = total;
  return ctx->report_progress() ? S_OK : E_ABORT;
}
Z7_COM7F_IMF(ExtractCallback::SetCompleted(const UInt64 * /*completeValue*/)) {
  return ctx && ctx->report_progress() ? S_OK : E_ABORT;
}

Z7_COM7F_IMF(ExtractCallback::GetStream(UInt32 index, ISequentialOutStream **outStream,
                                        Int32 askExtractMode)) {
  *outStream = nullptr;
  if (!ctx || !archive) return E_FAIL;
  active_output.Release();
  active_file = nullptr;
  ctx->pending_link = false;
  link_buf.clear();
  if (askExtractMode != NArchive::NExtract::NAskMode::kExtract) return S_OK;

  std::string path;
  bool embedded = false;
  if (read_string_prop(archive, index, kpidPath, path, embedded) != S_OK) return E_FAIL;
  if (path.empty()) return S_OK;
  if (embedded) {
    ctx->path_failed = true;
    if (ctx->message) *ctx->message = "refused path: control-character";
    return E_ABORT;
  }
  archiver::CheckedPath checked = archiver::check_entry_path(path);
  if (checked.reject != archiver::PathReject::Ok) {
    ctx->path_failed = true;
    if (ctx->message) {
      *ctx->message = std::string("refused path: ") + archiver::path_reject_string(checked.reject);
    }
    return E_ABORT;
  }

  PROPVARIANT prop;
  memset(&prop, 0, sizeof(prop));
  if (archive->GetProperty(index, kpidIsDir, &prop) != S_OK) {
    prop_clear(&prop);
    return E_FAIL;
  }
  bool is_dir = prop_bool(prop);
  prop_clear(&prop);

  std::string link;
  if (read_string_prop(archive, index, kpidSymLink, link, embedded) != S_OK) return E_FAIL;
  if (embedded) link.clear();

  memset(&prop, 0, sizeof(prop));
  uint32_t posix = 0;
  if (archive->GetProperty(index, kpidPosixAttrib, &prop) == S_OK && prop.vt == VT_UI4) {
    posix = prop.ulVal;
  }
  prop_clear(&prop);
  bool posix_link = (posix & 0170000u) == 0120000u;

  if (is_dir) {
    std::string error;
    if (!ctx->dest->mkdir_entry(checked.relative, error)) {
      note_write_error(ctx, error);
      return E_ABORT;
    }
    return S_OK;
  }

  if (!link.empty()) {
    archiver::CheckedPath target = archiver::check_symlink_target(checked.relative, link);
    if (target.reject != archiver::PathReject::Ok) {
      ctx->path_failed = true;
      if (ctx->message) *ctx->message = "refused symlink";
      return E_ABORT;
    }
    std::string error;
    if (!ctx->dest->make_symlink(checked.relative, link, error)) {
      note_write_error(ctx, error);
      return E_ABORT;
    }
    return S_OK;
  }

  if (posix_link) {
    ctx->pending_link = true;
    ctx->pending_path = checked.relative;
    ctx->link_buf = &link_buf;
    MemOutStream *mem = new MemOutStream();
    mem->dest = &link_buf;
    mem->ctx = ctx;
    CMyComPtr<ISequentialOutStream> held(mem);
    *outStream = held.Detach();
    return S_OK;
  }

  std::string error;
  std::unique_ptr<archiver::SafeDest::File> file = ctx->dest->create_file(checked.relative, error);
  if (!file) {
    note_write_error(ctx, error);
    return E_ABORT;
  }
  FileOutStream *out = new FileOutStream();
  out->file = std::move(file);
  out->ctx = ctx;
  active_output = out;
  active_file = out->file.get();
  CMyComPtr<ISequentialOutStream> held(out);
  *outStream = held.Detach();
  return S_OK;
}

Z7_COM7F_IMF(ExtractCallback::PrepareOperation(Int32 /*askExtractMode*/)) { return S_OK; }

Z7_COM7F_IMF(ExtractCallback::SetOperationResult(Int32 opRes)) {
  if (!ctx) return S_OK;
  if (active_output) {
    if (opRes == NArchive::NExtract::NOperationResult::kOK && !ctx->cancelled) {
      std::string error;
      if (!active_file->commit(error)) note_write_error(ctx, error);
    }
    active_output.Release();
    active_file = nullptr;
  }
  if (opRes == NArchive::NExtract::NOperationResult::kWrongPassword) ctx->wrong_password = true;
  if (opRes != NArchive::NExtract::NOperationResult::kOK &&
      opRes != NArchive::NExtract::NOperationResult::kWrongPassword) {
    ctx->data_error = true;
  }
  if (ctx->pending_link) {
    ctx->pending_link = false;
    if (opRes != NArchive::NExtract::NOperationResult::kOK || ctx->cancelled) {
      wipe_string(link_buf);
      return S_OK;
    }
    if (link_buf.find('\0') != std::string::npos || link_buf.size() > kMaxSymlink) {
      ctx->path_failed = true;
      if (ctx->message) *ctx->message = "refused symlink";
      return S_OK;
    }
    archiver::CheckedPath target = archiver::check_symlink_target(ctx->pending_path, link_buf);
    if (target.reject != archiver::PathReject::Ok) {
      ctx->path_failed = true;
      if (ctx->message) *ctx->message = "refused symlink";
      return S_OK;
    }
    std::string error;
    if (ctx->dest && !ctx->dest->make_symlink(ctx->pending_path, link_buf, error)) {
      note_write_error(ctx, error);
    }
    wipe_string(link_buf);
  }
  return S_OK;
}

Z7_COM7F_IMF(ExtractCallback::CryptoGetTextPassword(BSTR *passwordOut)) {
  if (!passwordOut) return E_ABORT;
  *passwordOut = nullptr;
  if (!ctx || !ctx->password) return E_ABORT;
  ctx->password->asked = true;
  if (!ctx->password->provided) return E_ABORT;
  *passwordOut = alloc_wide(ctx->password->text.c_str(), ctx->password->text.size());
  return *passwordOut ? S_OK : E_OUTOFMEMORY;
}

FileInStream *open_fd(const std::string &path, uint64_t slice_base = 0) {
  FileInStream *stream = new FileInStream();
#if defined(_WIN32)
  std::wstring wide = utf8_to_wide(path.c_str());
  HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    delete stream;
    return nullptr;
  }
  stream->handle = handle;
  LARGE_INTEGER length;
  if (!::GetFileSizeEx(handle, &length) || slice_base > static_cast<uint64_t>(length.QuadPart)) {
    delete stream;
    return nullptr;
  }
  stream->view_size = static_cast<uint64_t>(length.QuadPart) - slice_base;
#else
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    delete stream;
    return nullptr;
  }
  stream->fd = fd;
  struct stat st;
  if (::fstat(fd, &st) != 0 || slice_base > static_cast<uint64_t>(st.st_size)) {
    delete stream;
    return nullptr;
  }
  stream->view_size = static_cast<uint64_t>(st.st_size) - slice_base;
#endif
  stream->view_base = slice_base;
  stream->view_pos = 0;
  return stream;
}

bool read_at(const std::string &path, uint64_t offset, unsigned char *buf, size_t size, size_t *got) {
  *got = 0;
  if (size == 0) return true;
#if defined(_WIN32)
  std::wstring wide = utf8_to_wide(path.c_str());
  HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER move;
  move.QuadPart = static_cast<LONGLONG>(offset);
  if (!::SetFilePointerEx(handle, move, nullptr, FILE_BEGIN)) {
    ::CloseHandle(handle);
    return false;
  }
  DWORD n = 0;
  BOOL ok = ::ReadFile(handle, buf, static_cast<DWORD>(size), &n, nullptr);
  ::CloseHandle(handle);
  if (!ok) return false;
  *got = n;
#else
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  ssize_t n = ::pread(fd, buf, size, static_cast<off_t>(offset));
  ::close(fd);
  if (n < 0) return false;
  *got = static_cast<size_t>(n);
#endif
  return true;
}

bool file_size_of(const std::string &path, uint64_t *size) {
#if defined(_WIN32)
  std::wstring wide = utf8_to_wide(path.c_str());
  HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER length;
  BOOL ok = ::GetFileSizeEx(handle, &length);
  ::CloseHandle(handle);
  if (!ok || length.QuadPart < 0) return false;
  *size = static_cast<uint64_t>(length.QuadPart);
#else
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return false;
  *size = static_cast<uint64_t>(st.st_size);
#endif
  return true;
}

bool read_prefix(const std::string &path, unsigned char *buf, size_t size, size_t *got) {
  *got = 0;
#if defined(_WIN32)
  std::wstring wide = utf8_to_wide(path.c_str());
  HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  DWORD n = 0;
  BOOL ok = ::ReadFile(handle, buf, static_cast<DWORD>(size), &n, nullptr);
  ::CloseHandle(handle);
  if (!ok) return false;
  *got = n;
#else
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  ssize_t n = ::read(fd, buf, size);
  ::close(fd);
  if (n < 0) return false;
  *got = static_cast<size_t>(n);
#endif
  return true;
}

ArchiverResult from_open_hr(HRESULT hr, const PasswordState &password) {
  if (hr == S_OK) return ARCHIVER_OK;
  if (password.asked && !password.provided) return ARCHIVER_ERR_PASSWORD_REQUIRED;
  if (password.asked && password.provided) return ARCHIVER_ERR_PASSWORD;
  return ARCHIVER_ERR_FORMAT;
}

bool archive_has_encrypted_item(IInArchive *archive) {
  UInt32 count = 0;
  if (!archive || archive->GetNumberOfItems(&count) != S_OK) return false;
  for (UInt32 i = 0; i < count; ++i) {
    PROPVARIANT prop;
    memset(&prop, 0, sizeof(prop));
    if (archive->GetProperty(i, kpidEncrypted, &prop) == S_OK && prop_bool(prop)) {
      prop_clear(&prop);
      return true;
    }
    prop_clear(&prop);
  }
  return false;
}

ArchiverResult from_extract(HRESULT hr, const ExtractCtx &ctx, const PasswordState &password,
                            bool encrypted) {
  if (ctx.path_failed) return ARCHIVER_ERR_PATH;
  if (ctx.unsupported) return ARCHIVER_ERR_UNSUPPORTED;
  if (ctx.io_failed) return ARCHIVER_ERR_IO;
  if (ctx.cancelled) return ARCHIVER_ERR_CANCELLED;
  if (password.asked && !password.provided) return ARCHIVER_ERR_PASSWORD_REQUIRED;
  if (ctx.wrong_password || (ctx.data_error && (encrypted || password.asked))) {
    return password.provided ? ARCHIVER_ERR_PASSWORD : ARCHIVER_ERR_PASSWORD_REQUIRED;
  }
  if (encrypted && !password.provided && hr != S_OK) return ARCHIVER_ERR_PASSWORD_REQUIRED;
  if (ctx.data_error || hr != S_OK) return ARCHIVER_ERR_FORMAT;
  return ARCHIVER_OK;
}

bool make_temp_dir(std::string &out) {
#if defined(_WIN32)
  wchar_t base[MAX_PATH];
  DWORD n = ::GetTempPathW(MAX_PATH, base);
  if (n == 0 || n >= MAX_PATH) return false;
  for (unsigned i = 0; i < 1000; ++i) {
    std::wstring dir = std::wstring(base) + L"local-archiver-" +
                       std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(i);
    if (::CreateDirectoryW(dir.c_str(), nullptr)) {
      out = wide_to_utf8(dir.c_str(), dir.size());
      return true;
    }
    if (::GetLastError() != ERROR_ALREADY_EXISTS) return false;
  }
  return false;
#else
  const char *base = getenv("TMPDIR");
  std::string dir = (base && base[0]) ? base : "/tmp";
  if (dir.back() != '/') dir.push_back('/');
  dir += "local-archiver-XXXXXX";
  std::vector<char> buf(dir.begin(), dir.end());
  buf.push_back('\0');
  if (!mkdtemp(buf.data())) return false;
  out = buf.data();
  return true;
#endif
}

ArchiverResult open_with(EngineLib &lib, const GUID &clsid, const std::string &path,
                         PasswordState &password, CMyComPtr<IInArchive> &archive,
                         CMyComPtr<IInStream> &stream, std::string &message,
                         uint64_t slice_base = 0) {
  FileInStream *raw = open_fd(path, slice_base);
  if (!raw) {
    message = "could not open archive file";
    return ARCHIVER_ERR_IO;
  }
  CMyComPtr<IInStream> in(raw);
  void *obj = nullptr;
  HRESULT hr = lib.CreateObject(&clsid, &IID_IInArchive, &obj);
  if (hr != S_OK || !obj) {
    message = "7-Zip did not create an archive handler";
    return ARCHIVER_ERR_ENGINE;
  }
  CMyComPtr<IInArchive> in_archive;
  in_archive.Attach(static_cast<IInArchive *>(obj));
  OpenCallback *callback = new OpenCallback();
  callback->password = &password;
  CMyComPtr<IArchiveOpenCallback> held(callback);
  UInt64 max_check = 0;
  hr = in_archive->Open(in, &max_check, held);
  ArchiverResult result = from_open_hr(hr, password);
  if (result != ARCHIVER_OK) {
    in_archive->Close();
    if (message.empty()) {
      if (result == ARCHIVER_ERR_PASSWORD_REQUIRED) message = "password required";
      else if (result == ARCHIVER_ERR_PASSWORD) message = "password rejected";
      else message = "not a supported archive";
    }
    return result;
  }
  archive = in_archive;
  stream = in;
  return ARCHIVER_OK;
}

ArchiverResult read_entries(IInArchive *archive, std::vector<ListedEntry> &entries,
                            std::string &message) {
  UInt32 count = 0;
  if (archive->GetNumberOfItems(&count) != S_OK) {
    message = "could not list archive";
    return ARCHIVER_ERR_FORMAT;
  }
  if (count > kMaxEntries) {
    message = "archive has too many entries";
    return ARCHIVER_ERR_UNSUPPORTED;
  }
  uint64_t declared = 0;
  for (UInt32 i = 0; i < count; ++i) {
    std::string path;
    bool embedded = false;
    if (read_string_prop(archive, i, kpidPath, path, embedded) != S_OK) {
      message = "could not read entry name";
      return ARCHIVER_ERR_FORMAT;
    }
    if (path.empty()) continue;
    if (embedded) {
      message = "refused path: control-character";
      return ARCHIVER_ERR_PATH;
    }
    archiver::CheckedPath checked = archiver::check_entry_path(path);
    if (checked.reject != archiver::PathReject::Ok) {
      message = std::string("refused path: ") + archiver::path_reject_string(checked.reject);
      return ARCHIVER_ERR_PATH;
    }
    std::string link;
    bool link_nul = false;
    if (read_string_prop(archive, i, kpidSymLink, link, link_nul) != S_OK) {
      message = "could not read entry";
      return ARCHIVER_ERR_FORMAT;
    }
    if (!link.empty()) {
      if (link_nul || archiver::check_symlink_target(checked.relative, link).reject !=
                          archiver::PathReject::Ok) {
        message = "refused symlink";
        return ARCHIVER_ERR_PATH;
      }
    }
    PROPVARIANT prop;
    memset(&prop, 0, sizeof(prop));
    bool is_dir = false;
    if (archive->GetProperty(i, kpidIsDir, &prop) == S_OK) is_dir = prop_bool(prop);
    prop_clear(&prop);
    memset(&prop, 0, sizeof(prop));
    uint64_t size = 0;
    if (archive->GetProperty(i, kpidSize, &prop) == S_OK) size = prop_u64(prop);
    prop_clear(&prop);
    if (size > kWriteCap || declared > kWriteCap - size) {
      message = "archive declares more data than this build will write";
      return ARCHIVER_ERR_UNSUPPORTED;
    }
    declared += size;
    memset(&prop, 0, sizeof(prop));
    uint32_t posix = 0;
    if (archive->GetProperty(i, kpidPosixAttrib, &prop) == S_OK && prop.vt == VT_UI4) posix = prop.ulVal;
    prop_clear(&prop);
    ListedEntry entry;
    entry.path = checked.relative;
    entry.is_dir = is_dir;
    entry.is_symlink = !link.empty() || (posix & 0170000u) == 0120000u;
    entry.size = size;
    entries.push_back(entry);
  }
  return ARCHIVER_OK;
}

const char *outer_name(Kind kind) {
  switch (kind) {
    case Kind::Zip: return "zip";
    case Kind::SevenZ: return "7z";
    case Kind::Tar: return "tar";
    case Kind::TarGz: return "gzip";
    case Kind::TarBz2: return "bzip2";
    case Kind::TarXz: return "xz";
    case Kind::TarZst: return "zstd";
    default: return "";
  }
}

bool handler_for(EngineLib &lib, Kind kind, const std::string &path, GUID &out, std::string &message) {
  if (kind == Kind::Rar) {
    unsigned char sig[8];
    memset(sig, 0, sizeof(sig));
    size_t n = 0;
    if (!read_prefix(path, sig, sizeof(sig), &n)) {
      message = "could not open archive file";
      return false;
    }
    bool rar5 = n >= 8 && sig[0] == 'R' && sig[1] == 'a' && sig[2] == 'r' && sig[3] == '!' &&
                sig[6] == 1;
    if (find_handler(lib, rar5 ? "Rar5" : "Rar", out)) return true;
    message = "7-Zip library has no RAR handler";
    return false;
  }
  if (kind == Kind::TarZst) {
    if (find_handler(lib, "zstd", out) || find_handler(lib, "zstd2", out)) return true;
    message = "7-Zip library has no zstd handler";
    return false;
  }
  const char *name = outer_name(kind);
  if (!name[0] || !find_handler(lib, name, out)) {
    message = "7-Zip library has no handler for this extension";
    return false;
  }
  return true;
}

class BorrowedFileOut Z7_final : public ISequentialOutStream, public CMyUnknownImp {
  Z7_IFACES_IMP_UNK_1(ISequentialOutStream)
 public:
  archiver::SafeDest::File *file = nullptr;
  bool *failed = nullptr;
  uint64_t written = 0;
};

Z7_COM7F_IMF(BorrowedFileOut::Write(const void *data, UInt32 size, UInt32 *processedSize)) {
  if (processedSize) *processedSize = 0;
  if (!file) return E_FAIL;
  if (size > kWriteCap || written > kWriteCap - size) {
    if (failed) *failed = true;
    return E_ABORT;
  }
  std::string error;
  if (!file->write(data, size, error)) {
    if (failed) *failed = true;
    return E_FAIL;
  }
  written += size;
  if (processedSize) *processedSize = size;
  return S_OK;
}

class SingleCallback Z7_final : public IArchiveExtractCallback,
                                public ICryptoGetTextPassword,
                                public CMyUnknownImp {
  Z7_IFACES_IMP_UNK_2(IArchiveExtractCallback, ICryptoGetTextPassword)
  Z7_IFACE_COM7_IMP(IProgress)
 public:
  PasswordState *password = nullptr;
  std::unique_ptr<archiver::SafeDest::File> file;
  bool failed = false;
  bool wrong_password = false;
  UInt32 seen = 0;
};

Z7_COM7F_IMF(SingleCallback::SetTotal(UInt64 /*total*/)) { return S_OK; }
Z7_COM7F_IMF(SingleCallback::SetCompleted(const UInt64 * /*completeValue*/)) { return S_OK; }
Z7_COM7F_IMF(SingleCallback::PrepareOperation(Int32 /*askExtractMode*/)) { return S_OK; }
Z7_COM7F_IMF(SingleCallback::SetOperationResult(Int32 opRes)) {
  if (opRes == NArchive::NExtract::NOperationResult::kWrongPassword) wrong_password = true;
  if (opRes != NArchive::NExtract::NOperationResult::kOK &&
      opRes != NArchive::NExtract::NOperationResult::kUnavailable) {
    failed = true;
  }
  return S_OK;
}
Z7_COM7F_IMF(SingleCallback::CryptoGetTextPassword(BSTR *passwordOut)) {
  if (!passwordOut) return E_ABORT;
  *passwordOut = nullptr;
  if (!password) return E_ABORT;
  password->asked = true;
  if (!password->provided) return E_ABORT;
  *passwordOut = alloc_wide(password->text.c_str(), password->text.size());
  return *passwordOut ? S_OK : E_OUTOFMEMORY;
}
Z7_COM7F_IMF(SingleCallback::GetStream(UInt32 index, ISequentialOutStream **outStream,
                                       Int32 askExtractMode)) {
  *outStream = nullptr;
  if (askExtractMode != NArchive::NExtract::NAskMode::kExtract) return S_OK;
  ++seen;
  if (seen != 1 || !file) {
    failed = true;
    return E_ABORT;
  }
  (void)index;
  /* Ignore the name stored in the compressor. The bytes go to a path we chose. */
  BorrowedFileOut *out = new BorrowedFileOut();
  out->file = file.get();
  out->failed = &failed;
  CMyComPtr<ISequentialOutStream> held(out);
  *outStream = held.Detach();
  return S_OK;
}

ArchiverResult extract_single(IInArchive *archive, archiver::SafeDest &dest, PasswordState &password,
                              std::string &message) {
  UInt32 count = 0;
  if (archive->GetNumberOfItems(&count) != S_OK || count != 1) {
    message = "compressed tar must contain one stream";
    return ARCHIVER_ERR_FORMAT;
  }
  std::string error;
  std::unique_ptr<archiver::SafeDest::File> file = dest.create_file("inner.bin", error);
  if (!file) {
    message = "could not write temporary tar";
    return ARCHIVER_ERR_IO;
  }
  SingleCallback *callback = new SingleCallback();
  callback->password = &password;
  callback->file = std::move(file);
  CMyComPtr<IArchiveExtractCallback> held(callback);
  UInt32 all = static_cast<UInt32>(-1);
  HRESULT hr = archive->Extract(nullptr, all, 0, held);
  if (password.asked && !password.provided) return ARCHIVER_ERR_PASSWORD_REQUIRED;
  if (callback->wrong_password || (callback->failed && password.asked && password.provided)) {
    message = "password rejected";
    return ARCHIVER_ERR_PASSWORD;
  }
  if (hr != S_OK || callback->failed || callback->seen != 1) {
    message = "could not unwrap tar compression";
    return ARCHIVER_ERR_FORMAT;
  }
  if (!callback->file->commit(error)) {
    message = "could not write temporary tar";
    return ARCHIVER_ERR_IO;
  }
  return ARCHIVER_OK;
}

PasswordState password_from(const char *password) {
  PasswordState state;
  if (password) {
    state.text = utf8_to_wide(password);
    state.provided = true;
  }
  return state;
}

ArchiverResult open_kind(ArchiverArchive *archive, Kind kind, const std::string &path,
                         PasswordState &password) {
  GUID clsid;
  if (!handler_for(archive->engine, kind, path, clsid, archive->message)) {
    return archive->message.find("could not open") != std::string::npos ? ARCHIVER_ERR_IO
                                                                         : ARCHIVER_ERR_ENGINE;
  }
  return open_with(archive->engine, clsid, path, password, archive->archive, archive->stream,
                   archive->message);
}

static const char *kEmptyMessage = "";

const char *archiver_result_string(ArchiverResult result) {
  switch (result) {
    case ARCHIVER_OK: return "ok";
    case ARCHIVER_ERR_ARGUMENT: return "bad-argument";
    case ARCHIVER_ERR_IO: return "io";
    case ARCHIVER_ERR_FORMAT: return "format";
    case ARCHIVER_ERR_ENGINE: return "engine";
    case ARCHIVER_ERR_PASSWORD_REQUIRED: return "password-required";
    case ARCHIVER_ERR_PASSWORD: return "password-rejected";
    case ARCHIVER_ERR_PATH: return "path";
    case ARCHIVER_ERR_UNSUPPORTED: return "unsupported";
    case ARCHIVER_ERR_CANCELLED: return "cancelled";
  }
  return "error";
}

const char *archiver_message(const ArchiverArchive *archive) {
  if (!archive || archive->message.empty()) return kEmptyMessage;
  return archive->message.c_str();
}

ArchiverResult archiver_check_entry_path(const char *path) {
  if (!path) return ARCHIVER_ERR_ARGUMENT;
  archiver::CheckedPath checked = archiver::check_entry_path(path);
  return checked.reject == archiver::PathReject::Ok ? ARCHIVER_OK : ARCHIVER_ERR_PATH;
}

ArchiverResult archiver_check_symlink(const char *link_path, const char *target) {
  if (!link_path || !target) return ARCHIVER_ERR_ARGUMENT;
  archiver::CheckedPath link = archiver::check_entry_path(link_path);
  if (link.reject != archiver::PathReject::Ok) return ARCHIVER_ERR_PATH;
  archiver::CheckedPath checked = archiver::check_symlink_target(link.relative, target);
  return checked.reject == archiver::PathReject::Ok ? ARCHIVER_OK : ARCHIVER_ERR_PATH;
}

bool bytes_eq(const unsigned char *p, const char *lit, size_t n) {
  return memcmp(p, lit, n) == 0;
}

bool is_zip_local(const unsigned char *p) {
  return p[0] == 'P' && p[1] == 'K' && p[2] == 3 && p[3] == 4;
}
bool is_7z_sig(const unsigned char *p) {
  return bytes_eq(p, "\x37\x7A\xBC\xAF\x27\x1C", 6);
}
bool is_rar_sig(const unsigned char *p) {
  return bytes_eq(p, "Rar!\x1A\x07", 6);
}

uint32_t read_u32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t read_u16(const unsigned char *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

bool guid_for_kind(EngineLib &lib, Kind kind, const std::string &path, uint64_t offset, GUID &out,
                   std::string &message) {
  if (kind == Kind::Rar) {
    unsigned char sig[8];
    memset(sig, 0, sizeof(sig));
    size_t n = 0;
    if (!read_at(path, offset, sig, sizeof(sig), &n)) {
      message = "could not open archive file";
      return false;
    }
    bool rar5 = n >= 8 && is_rar_sig(sig) && sig[6] == 1;
    if (find_handler(lib, rar5 ? "Rar5" : "Rar", out)) return true;
    message = "7-Zip library has no RAR handler";
    return false;
  }
  const char *name = outer_name(kind);
  if (!name[0] || !find_handler(lib, name, out)) {
    message = "7-Zip library has no handler for this extension";
    return false;
  }
  return true;
}

void close_open_archive(ArchiverArchive *archive) {
  if (archive->archive) archive->archive->Close();
  archive->archive.Release();
  archive->stream.Release();
  archive->entries.clear();
}

/* Open one candidate and keep it only when the handler can list at least one file.
   A signature match alone is not enough. */
ArchiverResult try_listed_archive(ArchiverArchive *archive, Kind kind, const std::string &path,
                                  uint64_t offset, PasswordState &password) {
  GUID clsid;
  if (!guid_for_kind(archive->engine, kind, path, offset, clsid, archive->message)) {
    return ARCHIVER_ERR_ENGINE;
  }
  ArchiverResult opened = open_with(archive->engine, clsid, path, password, archive->archive,
                                     archive->stream, archive->message, offset);
  if (opened != ARCHIVER_OK) return opened;
  std::vector<ListedEntry> probe;
  std::string message;
  ArchiverResult listed = read_entries(archive->archive, probe, message);
  if (listed == ARCHIVER_OK && !probe.empty()) {
    archive->message.clear();
    return ARCHIVER_OK;
  }
  close_open_archive(archive);
  if (listed == ARCHIVER_ERR_PATH || listed == ARCHIVER_ERR_PASSWORD ||
      listed == ARCHIVER_ERR_PASSWORD_REQUIRED || listed == ARCHIVER_ERR_UNSUPPORTED) {
    archive->message = message;
    return listed;
  }
  return ARCHIVER_ERR_FORMAT;
}

struct SliceHit {
  Kind kind;
  uint64_t offset;
};

void remember_hit(std::vector<SliceHit> &hits, Kind kind, uint64_t offset) {
  for (const SliceHit &hit : hits) {
    if (hit.kind == kind && hit.offset == offset) return;
  }
  hits.push_back(SliceHit{kind, offset});
}

bool zip_start_from_eocd(const std::string &path, uint64_t file_size, uint64_t *start) {
  if (file_size < 22) return false;
  uint64_t tail = file_size < (65535ull + 22ull) ? file_size : (65535ull + 22ull);
  std::vector<unsigned char> end(static_cast<size_t>(tail));
  size_t got = 0;
  if (!read_at(path, file_size - tail, end.data(), static_cast<size_t>(tail), &got) || got < 22) {
    return false;
  }
  for (size_t i = got - 22;; --i) {
    if (end[i] == 'P' && end[i + 1] == 'K' && end[i + 2] == 5 && end[i + 3] == 6) {
      uint16_t comment = read_u16(end.data() + i + 20);
      uint64_t eocd = (file_size - got) + i;
      if (eocd + 22ull + comment == file_size) {
        uint32_t cd_size = read_u32(end.data() + i + 12);
        uint32_t cd_off = read_u32(end.data() + i + 16);
        int64_t origin = static_cast<int64_t>(eocd) - static_cast<int64_t>(cd_size) -
                         static_cast<int64_t>(cd_off);
        if (origin > 0 && static_cast<uint64_t>(origin) < file_size) {
          *start = static_cast<uint64_t>(origin);
          return true;
        }
      }
    }
    if (i == 0) break;
  }
  return false;
}

void scan_appended_signatures(const std::string &path, uint64_t file_size, std::vector<SliceHit> &jpeg,
                              std::vector<SliceHit> &tail) {
  const uint64_t overlap = 16;
  std::vector<unsigned char> buf(1u << 20);
  uint64_t pos = 0;
  while (pos < file_size) {
    size_t want = buf.size();
    if (file_size - pos < want) want = static_cast<size_t>(file_size - pos);
    size_t got = 0;
    if (!read_at(path, pos, buf.data(), want, &got) || got < 6) break;
    for (size_t i = 0; i + 6 <= got; ++i) {
      uint64_t off = pos + i;
      if (off == 0) continue;
      const unsigned char *p = buf.data() + i;
      if (is_7z_sig(p)) remember_hit(tail, Kind::SevenZ, off);
      else if (i + 7 <= got && is_rar_sig(p)) remember_hit(tail, Kind::Rar, off);
      if (p[0] == 0xFF && p[1] == 0xD9) {
        const unsigned char *next = p + 2;
        uint64_t after = off + 2;
        if (i + 6 <= got && is_zip_local(next)) remember_hit(jpeg, Kind::Zip, after);
        else if (i + 8 <= got && is_7z_sig(next)) remember_hit(jpeg, Kind::SevenZ, after);
        else if (i + 9 <= got && is_rar_sig(next)) remember_hit(jpeg, Kind::Rar, after);
      }
    }
    if (pos + got >= file_size) break;
    uint64_t step = got > overlap ? got - overlap : got;
    if (step == 0) break;
    pos += step;
  }
  if (tail.size() > 32) tail.erase(tail.begin(), tail.end() - 32);
}

ArchiverResult open_disguised(ArchiverArchive *archive, const std::string &path, PasswordState &password) {
  std::vector<SliceHit> hits;
  unsigned char head[8];
  memset(head, 0, sizeof(head));
  size_t head_n = 0;
  read_at(path, 0, head, sizeof(head), &head_n);
  if (head_n >= 4 && is_zip_local(head)) remember_hit(hits, Kind::Zip, 0);
  else if (head_n >= 6 && is_7z_sig(head)) remember_hit(hits, Kind::SevenZ, 0);
  else if (head_n >= 7 && is_rar_sig(head)) remember_hit(hits, Kind::Rar, 0);

  uint64_t file_size = 0;
  if (!file_size_of(path, &file_size)) {
    archive->message = "could not open archive file";
    return ARCHIVER_ERR_IO;
  }
  uint64_t zip_at = 0;
  if (zip_start_from_eocd(path, file_size, &zip_at)) remember_hit(hits, Kind::Zip, zip_at);

  std::vector<SliceHit> jpeg;
  std::vector<SliceHit> tail;
  /* Header hits are enough for a renamed file. Still scan when the header is
     media, and also when a header match might be a false start before a real tail. */
  if (hits.empty() || hits[0].offset != 0) {
    scan_appended_signatures(path, file_size, jpeg, tail);
    for (const SliceHit &hit : jpeg) remember_hit(hits, hit.kind, hit.offset);
    for (size_t i = tail.size(); i > 0; --i) remember_hit(hits, tail[i - 1].kind, tail[i - 1].offset);
  }

  if (hits.empty()) {
    archive->message = "no archive in this file";
    return ARCHIVER_ERR_UNSUPPORTED;
  }
  for (const SliceHit &hit : hits) {
    ArchiverResult result = try_listed_archive(archive, hit.kind, path, hit.offset, password);
    if (result == ARCHIVER_OK) return ARCHIVER_OK;
    if (result == ARCHIVER_ERR_PASSWORD_REQUIRED || result == ARCHIVER_ERR_PASSWORD ||
        result == ARCHIVER_ERR_PATH) {
      return result;
    }
  }
  archive->message = "no archive in this file";
  return ARCHIVER_ERR_UNSUPPORTED;
}

ArchiverResult archiver_open(const char *archive_path, const char *engine_path, const char *password,
                             ArchiverArchive **out) {
  if (!out) return ARCHIVER_ERR_ARGUMENT;
  *out = nullptr;
  if (!archive_path || !archive_path[0]) return ARCHIVER_ERR_ARGUMENT;

  Kind kind = classify(archive_path);
  if (kind == Kind::Split) return ARCHIVER_ERR_UNSUPPORTED;
  bool disguise = false;
  if (kind == Kind::Unsupported) {
    if (!is_disguise_carrier(archive_path)) return ARCHIVER_ERR_UNSUPPORTED;
    disguise = true;
  }

  std::unique_ptr<ArchiverArchive> archive(new ArchiverArchive());
  PasswordState pw = password_from(password);
  if (!load_engine(archive->engine, engine_path, archive->message)) {
    *out = archive.release();
    return ARCHIVER_ERR_ENGINE;
  }

  ArchiverResult result = ARCHIVER_OK;
  if (disguise) {
    result = open_disguised(archive.get(), archive_path, pw);
  } else if (kind == Kind::TarGz || kind == Kind::TarBz2 || kind == Kind::TarXz || kind == Kind::TarZst) {
    result = open_kind(archive.get(), kind, archive_path, pw);
    if (result == ARCHIVER_OK) {
      if (!make_temp_dir(archive->temp_dir)) {
        archive->message = "could not create a temporary directory";
        result = ARCHIVER_ERR_IO;
      } else {
        std::string error;
        std::unique_ptr<archiver::SafeDest> temp = archiver::SafeDest::open(archive->temp_dir, error);
        if (!temp) {
          archive->message = "could not create a temporary directory";
          result = ARCHIVER_ERR_IO;
        } else {
          result = extract_single(archive->archive, *temp, pw, archive->message);
        }
      }
      if (archive->archive) archive->archive->Close();
      archive->archive.Release();
      archive->stream.Release();
      if (result == ARCHIVER_OK) {
        GUID tar_id;
        if (!find_handler(archive->engine, "tar", tar_id)) {
          archive->message = "7-Zip library has no tar handler";
          result = ARCHIVER_ERR_ENGINE;
        } else {
          result = open_with(archive->engine, tar_id, archive->temp_dir + "/inner.bin", pw,
                             archive->archive, archive->stream, archive->message);
          if (result != ARCHIVER_OK && archive->message.find("password") == std::string::npos) {
            archive->message = "inner stream is not a tar archive";
            result = ARCHIVER_ERR_FORMAT;
          }
        }
      }
    }
  } else {
    result = open_kind(archive.get(), kind, archive_path, pw);
  }

  if (result == ARCHIVER_OK && archive->entries.empty()) {
    result = read_entries(archive->archive, archive->entries, archive->message);
    if (result != ARCHIVER_OK) {
      if (archive->archive) archive->archive->Close();
      archive->archive.Release();
      archive->stream.Release();
    }
  }
  *out = archive.release();
  return result;
}

ArchiverResult archiver_entry_count(const ArchiverArchive *archive, uint32_t *count) {
  if (!archive || !count) return ARCHIVER_ERR_ARGUMENT;
  *count = static_cast<uint32_t>(archive->entries.size());
  return ARCHIVER_OK;
}

ArchiverResult archiver_entry_at(const ArchiverArchive *archive, uint32_t index, ArchiverEntry *out) {
  if (!archive || !out) return ARCHIVER_ERR_ARGUMENT;
  if (index >= archive->entries.size()) return ARCHIVER_ERR_ARGUMENT;
  const ListedEntry &entry = archive->entries[index];
  out->path = static_cast<char *>(malloc(entry.path.size() + 1));
  if (!out->path) return ARCHIVER_ERR_IO;
  memcpy(out->path, entry.path.c_str(), entry.path.size() + 1);
  out->is_directory = entry.is_dir ? 1 : 0;
  out->is_symlink = entry.is_symlink ? 1 : 0;
  out->size = entry.size;
  return ARCHIVER_OK;
}

void archiver_entry_free(ArchiverEntry *entry) {
  if (!entry) return;
  free(entry->path);
  entry->path = nullptr;
}

ArchiverResult archiver_extract(ArchiverArchive *archive, const char *dest_dir, const char *password) {
  return archiver_extract_with_progress(archive, dest_dir, password, nullptr, nullptr);
}

ArchiverResult archiver_extract_with_progress(
    ArchiverArchive *archive, const char *dest_dir, const char *password,
    ArchiverProgressCallback progress, void *user) {
  if (!archive || !dest_dir || !dest_dir[0]) return ARCHIVER_ERR_ARGUMENT;
  if (!archive->archive) {
    if (archive->message.empty()) archive->message = "archive is not open";
    return ARCHIVER_ERR_FORMAT;
  }
  PasswordState pw = password_from(password);
  archive->message.clear();
  bool encrypted = archive_has_encrypted_item(archive->archive);
  if (encrypted && !pw.provided) {
    archive->message = "password required";
    return ARCHIVER_ERR_PASSWORD_REQUIRED;
  }
  std::string error;
  std::unique_ptr<archiver::SafeDest> dest = archiver::SafeDest::open(dest_dir, error);
  if (!dest) {
    archive->message = error == "symlink-component" ? "refused destination link"
                                                    : "could not open destination";
    return error == "symlink-component" ? ARCHIVER_ERR_PATH : ARCHIVER_ERR_IO;
  }
  ExtractCtx ctx;
  ctx.password = &pw;
  ctx.dest = dest.get();
  ctx.message = &archive->message;
  ctx.progress = progress;
  ctx.progress_user = user;
  for (const ListedEntry &entry : archive->entries) ctx.total += entry.size;
  if (!ctx.report_progress()) return ARCHIVER_ERR_CANCELLED;
  ExtractCallback *callback = new ExtractCallback();
  callback->archive = archive->archive;
  callback->ctx = &ctx;
  CMyComPtr<IArchiveExtractCallback> held(callback);
  UInt32 all = static_cast<UInt32>(-1);
  HRESULT hr = archive->archive->Extract(nullptr, all, 0, held);
  return from_extract(hr, ctx, pw, encrypted);
}

void archiver_close(ArchiverArchive *archive) { delete archive; }
