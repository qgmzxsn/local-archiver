#ifndef LOCAL_ARCHIVER_UTF_HPP
#define LOCAL_ARCHIVER_UTF_HPP

#include <cstdint>
#include <string>

namespace archiver {

inline void append_utf8(std::string &out, uint32_t cp) {
  if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
  if (cp < 0x80) out.push_back(static_cast<char>(cp));
  else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  }
}

template <class Char>
std::string utf16_to_utf8(const Char *text, size_t size) {
  std::string out;
  for (size_t i = 0; i < size; ++i) {
    uint32_t cp = text[i];
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < size) {
      uint32_t low = text[i + 1];
      if (low >= 0xdc00 && low <= 0xdfff) {
        cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
        ++i;
      }
    }
    append_utf8(out, cp);
  }
  return out;
}

inline std::u16string utf8_to_utf16(const std::string &text) {
  std::u16string out;
  size_t i = 0;
  while (i < text.size()) {
    uint32_t first = static_cast<unsigned char>(text[i]);
    uint32_t cp = first;
    size_t length = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 :
                    first >= 0xe0 && first <= 0xef ? 3 :
                    first >= 0xf0 && first <= 0xf4 ? 4 : 0;
    bool valid = length && i + length <= text.size();
    if (valid && length > 1) {
      cp &= (1u << (7 - length)) - 1;
      for (size_t j = 1; j < length; ++j) {
        uint32_t next = static_cast<unsigned char>(text[i + j]);
        if ((next & 0xc0) != 0x80) valid = false;
        cp = (cp << 6) | (next & 0x3f);
      }
      if (cp < (length == 2 ? 0x80u : length == 3 ? 0x800u : 0x10000u) ||
          cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) valid = false;
    }
    if (!valid) { cp = 0xfffd; length = 1; }
    i += length;
    if (cp > 0xffff) {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xd800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xdc00 + (cp & 0x3ff)));
    } else out.push_back(static_cast<char16_t>(cp));
  }
  return out;
}

}  // namespace archiver
#endif
