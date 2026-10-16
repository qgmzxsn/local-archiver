#include "archiver.h"

#include <jni.h>

#include <string.h>

#include <string>

namespace {

void wipe(std::string &s) {
  if (!s.empty()) {
    volatile char *p = &s[0];
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
  }
  s.clear();
}

std::string from_chars(JNIEnv *env, jcharArray chars) {
  if (!chars) return std::string();
  jsize n = env->GetArrayLength(chars);
  if (n <= 0) return std::string();
  jchar *data = env->GetCharArrayElements(chars, nullptr);
  if (!data) return std::string();
  std::wstring wide(data, data + n);
  env->ReleaseCharArrayElements(chars, data, JNI_ABORT);
  std::string out;
  for (wchar_t ch : wide) {
    uint32_t cp = static_cast<uint32_t>(ch);
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

std::string from_jstring(JNIEnv *env, jstring value) {
  if (!value) return std::string();
  const char *utf = env->GetStringUTFChars(value, nullptr);
  std::string out = utf ? utf : "";
  if (utf) env->ReleaseStringUTFChars(value, utf);
  return out;
}

jstring result_text(JNIEnv *env, ArchiverResult result, ArchiverArchive *archive) {
  std::string text = archiver_result_string(result);
  const char *detail = archiver_message(archive);
  if (detail && detail[0]) {
    text.push_back('\n');
    text += detail;
  }
  return env->NewStringUTF(text.c_str());
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_app_localarchiver_ArchiverNative_listArchive(JNIEnv *env, jobject, jstring archive_path,
                                                   jstring engine_path, jcharArray password) {
  std::string path = from_jstring(env, archive_path);
  std::string engine = from_jstring(env, engine_path);
  std::string pw = from_chars(env, password);
  const char *pw_ptr = password ? pw.c_str() : nullptr;
  ArchiverArchive *archive = nullptr;
  ArchiverResult result =
      archiver_open(path.c_str(), engine.empty() ? nullptr : engine.c_str(), pw_ptr, &archive);
  wipe(pw);
  if (result != ARCHIVER_OK) {
    jstring text = result_text(env, result, archive);
    archiver_close(archive);
    return text;
  }
  std::string text = "ok";
  uint32_t count = 0;
  archiver_entry_count(archive, &count);
  for (uint32_t i = 0; i < count; ++i) {
    ArchiverEntry entry;
    memset(&entry, 0, sizeof(entry));
    if (archiver_entry_at(archive, i, &entry) != ARCHIVER_OK) continue;
    text.push_back('\n');
    text += entry.path ? entry.path : "";
    archiver_entry_free(&entry);
  }
  archiver_close(archive);
  return env->NewStringUTF(text.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_localarchiver_ArchiverNative_extractArchive(JNIEnv *env, jobject, jstring archive_path,
                                                      jstring dest_path, jstring engine_path,
                                                      jcharArray password) {
  std::string path = from_jstring(env, archive_path);
  std::string dest = from_jstring(env, dest_path);
  std::string engine = from_jstring(env, engine_path);
  std::string pw = from_chars(env, password);
  const char *pw_ptr = password ? pw.c_str() : nullptr;
  ArchiverArchive *archive = nullptr;
  ArchiverResult result =
      archiver_open(path.c_str(), engine.empty() ? nullptr : engine.c_str(), pw_ptr, &archive);
  if (result == ARCHIVER_OK) {
    result = archiver_extract(archive, dest.c_str(), pw_ptr);
  }
  wipe(pw);
  jstring text = result_text(env, result, archive);
  archiver_close(archive);
  return text;
}
