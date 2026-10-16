/* Lexical path checks. They do not touch the filesystem. */

#ifndef LOCAL_ARCHIVER_PATH_SAFETY_HPP
#define LOCAL_ARCHIVER_PATH_SAFETY_HPP

#include <string>

namespace archiver {

enum class PathReject {
  Ok = 0,
  Empty,
  Absolute,
  DotDot,
  Control,
  Escapes,
  TooLong
};

struct CheckedPath {
  PathReject reject = PathReject::Ok;
  std::string relative;
};

/* Rejects absolute paths and any ".." component, even if it would normalize
   back inside the destination. */
CheckedPath check_entry_path(const std::string &raw);

/* link_relative is a path already accepted by check_entry_path.
   target is the link text. Absolute targets and targets that resolve outside
   the destination are rejected. A ".." that stays inside is allowed. */
CheckedPath check_symlink_target(const std::string &link_relative,
                                 const std::string &target);

const char *path_reject_string(PathReject reject);

}  /* namespace archiver */

#endif
