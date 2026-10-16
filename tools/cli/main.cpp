#include "archiver.h"

#include <stdio.h>
#include <string.h>

#include <iostream>
#include <string>

#if !defined(_WIN32)
#include <termios.h>
#include <unistd.h>
#endif

namespace {

void wipe(std::string &s) {
  if (!s.empty()) {
    volatile char *p = &s[0];
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
  }
  s.clear();
}

void print_result(ArchiverResult result, const ArchiverArchive *archive) {
  const char *detail = archiver_message(archive);
  switch (result) {
    case ARCHIVER_OK:
      std::cout << "完成\n";
      break;
    case ARCHIVER_ERR_PASSWORD_REQUIRED:
      std::cout << "需要密码。请输入，程序不会保存。\n";
      break;
    case ARCHIVER_ERR_PASSWORD:
      std::cout << "密码不对。\n";
      break;
    case ARCHIVER_ERR_PATH:
      std::cout << "已拒绝不安全的路径。\n";
      break;
    case ARCHIVER_ERR_UNSUPPORTED:
      std::cout << "这个文件不在第一阶段的范围内。\n";
      break;
    case ARCHIVER_ERR_FORMAT:
      std::cout << "打不开这个压缩包。\n";
      break;
    case ARCHIVER_ERR_ENGINE:
      std::cout << "找不到 7-Zip 动态库。\n";
      break;
    case ARCHIVER_ERR_IO:
      std::cout << "读写文件失败。\n";
      break;
    default:
      std::cout << "失败。\n";
      break;
  }
  if (detail && detail[0]) std::cout << detail << "\n";
}

bool read_password(std::string &out) {
#if !defined(_WIN32)
  if (isatty(STDIN_FILENO)) {
    std::fputs("密码（只用于这一次，不会保存）: ", stderr);
    termios old_term;
    if (tcgetattr(STDIN_FILENO, &old_term) == 0) {
      termios hidden = old_term;
      hidden.c_lflag = static_cast<tcflag_t>(hidden.c_lflag & ~ECHO);
      tcsetattr(STDIN_FILENO, TCSANOW, &hidden);
      std::getline(std::cin, out);
      tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
      std::fputc('\n', stderr);
      return !out.empty() || std::cin.good();
    }
  }
#endif
  if (!std::getline(std::cin, out)) return false;
  if (!out.empty() && out.back() == '\r') out.pop_back();
  return true;
}

void usage() {
  std::cerr
      << "用法:\n"
      << "  archiver-cli list <压缩包> [--engine 7z.so]\n"
      << "  archiver-cli extract <压缩包> <目标文件夹> [--engine 7z.so]\n"
      << "需要密码时从键盘输入。密码不会写入文件，也不会出现在命令行参数里。\n"
      << "不会运行解压出来的程序。\n";
}

const char *take_engine(int &i, int argc, char **argv) {
  if (i + 1 < argc && strcmp(argv[i], "--engine") == 0) {
    i += 1;
    return argv[i];
  }
  return nullptr;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    usage();
    return ARCHIVER_ERR_ARGUMENT;
  }
  const char *cmd = argv[1];
  const char *archive_path = argv[2];
  const char *dest = nullptr;
  const char *engine = nullptr;
  int index = 3;
  if (strcmp(cmd, "extract") == 0) {
    if (argc < 4) {
      usage();
      return ARCHIVER_ERR_ARGUMENT;
    }
    dest = argv[3];
    index = 4;
  } else if (strcmp(cmd, "list") != 0) {
    usage();
    return ARCHIVER_ERR_ARGUMENT;
  }
  for (; index < argc; ++index) {
    const char *found = take_engine(index, argc, argv);
    if (!found) {
      usage();
      return ARCHIVER_ERR_ARGUMENT;
    }
    engine = found;
  }

  std::string password;
  bool have_password = false;
  ArchiverArchive *archive = nullptr;
  const char *pw = nullptr;
  ArchiverResult result = archiver_open(archive_path, engine, pw, &archive);
  if (result == ARCHIVER_ERR_PASSWORD_REQUIRED) {
    archiver_close(archive);
    archive = nullptr;
    if (!read_password(password)) {
      wipe(password);
      print_result(result, nullptr);
      return result;
    }
    have_password = true;
    result = archiver_open(archive_path, engine, password.c_str(), &archive);
  }
  if (result != ARCHIVER_OK) {
    print_result(result, archive);
    archiver_close(archive);
    wipe(password);
    return result;
  }

  if (strcmp(cmd, "list") == 0) {
    uint32_t count = 0;
    archiver_entry_count(archive, &count);
    for (uint32_t i = 0; i < count; ++i) {
      ArchiverEntry entry;
      memset(&entry, 0, sizeof(entry));
      if (archiver_entry_at(archive, i, &entry) != ARCHIVER_OK) continue;
      std::cout << (entry.is_directory ? "dir " : entry.is_symlink ? "link " : "file ")
                << entry.path << "\n";
      archiver_entry_free(&entry);
    }
    archiver_close(archive);
    wipe(password);
    return ARCHIVER_OK;
  }

  result = archiver_extract(archive, dest, have_password ? password.c_str() : nullptr);
  if (result == ARCHIVER_ERR_PASSWORD_REQUIRED) {
    if (!read_password(password)) {
      print_result(result, archive);
      archiver_close(archive);
      wipe(password);
      return result;
    }
    have_password = true;
    result = archiver_extract(archive, dest, password.c_str());
  }
  print_result(result, archive);
  archiver_close(archive);
  wipe(password);
  return result;
}
