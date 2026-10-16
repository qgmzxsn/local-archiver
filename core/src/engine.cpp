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

#define INITGUID
#include "Common/MyGuidDef.h"
#include "Common/MyWindows.h"
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
  return *(reinterpret_cast<uint32_t *>(b) - 1);
}

BSTR alloc_wide(const wchar_t *s, size_t n) {
  if (n > 0x0fffffffu / sizeof(wchar_t)) return nullptr;
  uint32_t bytes = static_cast<uint32_t>(n * sizeof(wchar_t));
  void *mem = malloc(bytes + sizeof(uint32_t) + sizeof(wchar_t));
  if (!mem) return nullptr;
  *static_cast<uint32_t *>(mem) = bytes;
  wchar_t *b = reinterpret_cast<wchar_t *>(static_cast<uint32_t *>(mem) + 1);
  if (n) memcpy(b, s, bytes);
  b[n] = 0;
  return b;
}

void free_bstr(BSTR b) {
  if (!b) return;
  free(reinterpret_cast<uint32_t *>(b) - 1);
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
      ::unlink(inner.c_str());
      ::rmdir(temp_dir.c_str());
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
  int fd = -1;
  ~FileInStream() {
    if (fd >= 0) ::close(fd);
  }
};

Z7_COM7F_IMF(FileInStream::Read(void *data, UInt32 size, UInt32 *processedSize)) {
  if (processedSize) *processedSize = 0;
  ssize_t n = ::read(fd, data, size);
  if (n < 0) return E_FAIL;
  if (processedSize) *processedSize = static_cast<UInt32>(n);
  return S_OK;
}

Z7_COM7F_IMF(FileInStream::Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition)) {
  int whence = SEEK_SET;
  if (seekOrigin == 1) whence = SEEK_CUR;
  else if (seekOrigin == 2) whence = SEEK_END;
  else if (seekOrigin != 0) return E_INVALIDARG;
  off_t pos = ::lseek(fd, static_cast<off_t>(offset), whence);
  if (pos < 0) return MY_E_ERROR_NEGATIVE_SEEK;
  if (newPosition) *newPosition = static_cast<UInt64>(pos);
  return S_OK;
}

Z7_COM7F_IMF(FileInStream::GetSize(UInt64 *size)) {
  struct stat st;
  if (::fstat(fd, &st) != 0) return E_FAIL;
  *size = static_cast<UInt64>(st.st_size);
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
  if (error == "io") {
    ctx->io_failed = true;
    if (ctx->message && ctx->message->empty()) *ctx->message = "could not write output";
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

Z7_COM7F_IMF(ExtractCallback::SetTotal(UInt64 /*total*/)) { return S_OK; }
Z7_COM7F_IMF(ExtractCallback::SetCompleted(const UInt64 * /*completeValue*/)) { return S_OK; }

Z7_COM7F_IMF(ExtractCallback::GetStream(UInt32 index, ISequentialOutStream **outStream,
                                        Int32 askExtractMode)) {
  *outStream = nullptr;
  if (!ctx || !archive) return E_FAIL;
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
  CMyComPtr<ISequentialOutStream> held(out);
  *outStream = held.Detach();
  return S_OK;
}

Z7_COM7F_IMF(ExtractCallback::PrepareOperation(Int32 /*askExtractMode*/)) { return S_OK; }

Z7_COM7F_IMF(ExtractCallback::SetOperationResult(Int32 opRes)) {
  if (!ctx) return S_OK;
  if (opRes == NArchive::NExtract::NOperationResult::kWrongPassword) ctx->wrong_password = true;
  if (opRes == NArchive::NExtract::NOperationResult::kDataError ||
      opRes == NArchive::NExtract::NOperationResult::kCRCError ||
      opRes == NArchive::NExtract::NOperationResult::kUnsupportedMethod) {
    ctx->data_error = true;
  }
  if (ctx->pending_link) {
    ctx->pending_link = false;
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
    link_buf.clear();
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

FileInStream *open_fd(const std::string &path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return nullptr;
  FileInStream *stream = new FileInStream();
  stream->fd = fd;
  return stream;
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
  if (password.asked && !password.provided) return ARCHIVER_ERR_PASSWORD_REQUIRED;
  if (ctx.wrong_password || (ctx.data_error && (encrypted || password.asked || password.provided))) {
    return password.provided ? ARCHIVER_ERR_PASSWORD : ARCHIVER_ERR_PASSWORD_REQUIRED;
  }
  if (encrypted && !password.provided && hr != S_OK) return ARCHIVER_ERR_PASSWORD_REQUIRED;
  if (ctx.data_error || hr != S_OK) return ARCHIVER_ERR_FORMAT;
  return ARCHIVER_OK;
}

bool make_temp_dir(std::string &out) {
  const char *base = getenv("TMPDIR");
  std::string dir = (base && base[0]) ? base : "/tmp";
  if (dir.back() != '/') dir.push_back('/');
  dir += "local-archiver-XXXXXX";
  std::vector<char> buf(dir.begin(), dir.end());
  buf.push_back('\0');
  if (!mkdtemp(buf.data())) return false;
  out = buf.data();
  return true;
}

ArchiverResult open_with(EngineLib &lib, const GUID &clsid, const std::string &path,
                         PasswordState &password, CMyComPtr<IInArchive> &archive,
                         CMyComPtr<IInStream> &stream, std::string &message) {
  FileInStream *raw = open_fd(path);
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
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      message = "could not open archive file";
      return false;
    }
    ssize_t n = ::read(fd, sig, sizeof(sig));
    ::close(fd);
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
  if (callback->wrong_password || (callback->failed && password.provided)) {
    message = "password rejected";
    return ARCHIVER_ERR_PASSWORD;
  }
  if (hr != S_OK || callback->failed || callback->seen != 1) {
    message = "could not unwrap tar compression";
    return ARCHIVER_ERR_FORMAT;
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

ArchiverResult archiver_open(const char *archive_path, const char *engine_path, const char *password,
                             ArchiverArchive **out) {
  if (!out) return ARCHIVER_ERR_ARGUMENT;
  *out = nullptr;
  if (!archive_path || !archive_path[0]) return ARCHIVER_ERR_ARGUMENT;

  Kind kind = classify(archive_path);
  if (kind == Kind::Split || kind == Kind::Unsupported) return ARCHIVER_ERR_UNSUPPORTED;

  std::unique_ptr<ArchiverArchive> archive(new ArchiverArchive());
  PasswordState pw = password_from(password);
  if (!load_engine(archive->engine, engine_path, archive->message)) {
    *out = archive.release();
    return ARCHIVER_ERR_ENGINE;
  }

  ArchiverResult result = ARCHIVER_OK;
  if (kind == Kind::TarGz || kind == Kind::TarBz2 || kind == Kind::TarXz || kind == Kind::TarZst) {
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

  if (result == ARCHIVER_OK) {
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
    archive->message = "could not open destination";
    return ARCHIVER_ERR_IO;
  }
  ExtractCtx ctx;
  ctx.password = &pw;
  ctx.dest = dest.get();
  ctx.message = &archive->message;
  ExtractCallback *callback = new ExtractCallback();
  callback->archive = archive->archive;
  callback->ctx = &ctx;
  CMyComPtr<IArchiveExtractCallback> held(callback);
  UInt32 all = static_cast<UInt32>(-1);
  HRESULT hr = archive->archive->Extract(nullptr, all, 0, held);
  return from_extract(hr, ctx, pw, encrypted);
}

void archiver_close(ArchiverArchive *archive) { delete archive; }
