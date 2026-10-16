#include "password_book.hpp"

#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#endif

namespace {
constexpr uint32_t kMagic = 0x3142504cu; /* 'LPB1' little-endian */
constexpr int kMaxPasswords = 64;
constexpr int kMaxPasswordBytes = 1024;
}

bool PasswordBook::available() {
#if defined(_WIN32)
  return true;
#else
  return false;
#endif
}

QVector<QByteArray> PasswordBook::load() {
  QVector<QByteArray> out;
#if defined(_WIN32)
  PCREDENTIALW cred = nullptr;
  if (!::CredReadW(L"local-archiver/book", CRED_TYPE_GENERIC, 0, &cred) || !cred) return out;
  const unsigned char *cursor = cred->CredentialBlob;
  DWORD left = cred->CredentialBlobSize;
  auto take = [&](uint32_t &value) {
    if (left < 4) return false;
    memcpy(&value, cursor, 4);
    cursor += 4;
    left -= 4;
    return true;
  };
  uint32_t magic = 0;
  uint32_t count = 0;
  if (take(magic) && take(count) && magic == kMagic && count <= kMaxPasswords) {
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t len = 0;
      if (!take(len) || len > kMaxPasswordBytes || len > left) break;
      out.append(QByteArray(reinterpret_cast<const char *>(cursor), static_cast<int>(len)));
      cursor += len;
      left -= len;
    }
  }
  if (cred->CredentialBlob && cred->CredentialBlobSize)
    ::SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
  ::CredFree(cred);
#else
  (void)out;
#endif
  return out;
}

bool PasswordBook::store(const QVector<QByteArray> &passwords) {
#if defined(_WIN32)
  auto wipe = [](QByteArray &bytes) {
    if (!bytes.isEmpty()) {
      volatile char *data = bytes.data();
      for (qsizetype i = 0; i < bytes.size(); ++i) data[i] = 0;
    }
    bytes.clear();
  };
  if (passwords.size() > kMaxPasswords) return false;
  QByteArray blob;
  auto append32 = [&](uint32_t value) {
    char raw[4];
    memcpy(raw, &value, 4);
    blob.append(raw, 4);
  };
  append32(kMagic);
  append32(static_cast<uint32_t>(passwords.size()));
  for (const QByteArray &password : passwords) {
    if (password.size() > kMaxPasswordBytes) {
      wipe(blob);
      return false;
    }
    append32(static_cast<uint32_t>(password.size()));
    blob.append(password);
  }
  CREDENTIALW cred;
  memset(&cred, 0, sizeof(cred));
  cred.Type = CRED_TYPE_GENERIC;
  cred.TargetName = const_cast<LPWSTR>(L"local-archiver/book");
  cred.CredentialBlobSize = static_cast<DWORD>(blob.size());
  cred.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
  cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
  cred.UserName = const_cast<LPWSTR>(L"local-archiver");
  BOOL ok = ::CredWriteW(&cred, 0);
  wipe(blob);
  return ok != 0;
#else
  (void)passwords;
  return false;
#endif
}
