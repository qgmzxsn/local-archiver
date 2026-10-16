#include "utf.hpp"
#include <cstdio>

int main() {
  const std::u16string text = u"目录\U0001f600/文件\U0001f98a.txt";
  const std::string utf8 = u8"目录\U0001f600/文件\U0001f98a.txt";
  const char16_t invalid[] = {0xd800, u'a', 0xdc00};
  if (archiver::utf16_to_utf8(text.data(), text.size()) != utf8 ||
      archiver::utf8_to_utf16(utf8) != text ||
      archiver::utf16_to_utf8(invalid, 3) != u8"\ufffda\ufffd" ||
      archiver::utf8_to_utf16("\xc0\x80") != u"\ufffd\ufffd" ||
      archiver::utf8_to_utf16("\xf0\x9f") != u"\ufffd\ufffd" ||
      archiver::utf8_to_utf16(std::string("a\0b", 3)) != std::u16string(u"a\0b", 3)) {
    std::fputs("Unicode conversion failed\n", stderr);
    return 1;
  }
  std::puts("Unicode checks passed");
  return 0;
}
