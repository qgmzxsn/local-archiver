#include "path_safety.hpp"

#include <cctype>
#include <vector>

namespace archiver {
namespace {

constexpr size_t kMaxPath = 4096;

bool is_sep(char c) { return c == '/' || c == '\\'; }

bool has_control(const std::string &s) {
  for (unsigned char c : s) {
    if (c < 0x20 || c == 0x7f) return true;
  }
  return false;
}

bool is_absolute_path(const std::string &raw) {
  if (raw.empty()) return false;
  if (is_sep(raw[0])) return true;
  if (raw.size() >= 2 && std::isalpha(static_cast<unsigned char>(raw[0])) &&
      raw[1] == ':') {
    return true;
  }
  return false;
}

std::vector<std::string> split_parts(const std::string &raw) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : raw) {
    if (is_sep(c)) {
      parts.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  parts.push_back(cur);
  return parts;
}

CheckedPath fail(PathReject reject) {
  CheckedPath out;
  out.reject = reject;
  return out;
}

}  /* namespace */

CheckedPath check_entry_path(const std::string &raw) {
  if (raw.empty() || raw.size() > kMaxPath) {
    return fail(raw.empty() ? PathReject::Empty : PathReject::TooLong);
  }
  if (has_control(raw)) return fail(PathReject::Control);
  if (is_absolute_path(raw)) return fail(PathReject::Absolute);

  std::vector<std::string> kept;
  for (const std::string &part : split_parts(raw)) {
    if (part.empty() || part == ".") continue;
    if (part == "..") return fail(PathReject::DotDot);
    if (part.find(':') != std::string::npos) return fail(PathReject::Absolute);
    kept.push_back(part);
  }
  if (kept.empty()) return fail(PathReject::Empty);

  CheckedPath out;
  for (size_t i = 0; i < kept.size(); ++i) {
    if (i) out.relative.push_back('/');
    out.relative += kept[i];
  }
  if (out.relative.size() > kMaxPath) return fail(PathReject::TooLong);
  return out;
}

CheckedPath check_symlink_target(const std::string &link_relative,
                                 const std::string &target) {
  if (target.empty() || target.size() > kMaxPath) {
    return fail(target.empty() ? PathReject::Empty : PathReject::TooLong);
  }
  if (has_control(target) || has_control(link_relative)) {
    return fail(PathReject::Control);
  }
  if (is_absolute_path(target)) return fail(PathReject::Absolute);
  if (target.find(':') != std::string::npos) return fail(PathReject::Absolute);

  std::vector<std::string> stack = split_parts(link_relative);
  if (!stack.empty()) stack.pop_back();
  std::vector<std::string> base;
  for (const std::string &part : stack) {
    if (part.empty() || part == ".") continue;
    if (part == "..") return fail(PathReject::DotDot);
    base.push_back(part);
  }

  for (const std::string &part : split_parts(target)) {
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      if (base.empty()) return fail(PathReject::Escapes);
      base.pop_back();
      continue;
    }
    if (part.find(':') != std::string::npos) return fail(PathReject::Absolute);
    base.push_back(part);
  }

  CheckedPath out;
  for (size_t i = 0; i < base.size(); ++i) {
    if (i) out.relative.push_back('/');
    out.relative += base[i];
  }
  return out;
}

const char *path_reject_string(PathReject reject) {
  switch (reject) {
    case PathReject::Ok: return "ok";
    case PathReject::Empty: return "empty-path";
    case PathReject::Absolute: return "absolute-path";
    case PathReject::DotDot: return "dot-dot";
    case PathReject::Control: return "control-character";
    case PathReject::Escapes: return "symlink-escapes";
    case PathReject::TooLong: return "path-too-long";
  }
  return "path";
}

}  /* namespace archiver */
