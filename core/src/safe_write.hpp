/* Writes only inside a destination directory.
   POSIX uses openat and O_NOFOLLOW. That path is what Linux tests run.
   The Windows branch was not executed in the phase 1 environment. */

#ifndef LOCAL_ARCHIVER_SAFE_WRITE_HPP
#define LOCAL_ARCHIVER_SAFE_WRITE_HPP

#include <memory>
#include <string>

namespace archiver {

class SafeDest {
 public:
  class File {
   public:
    virtual ~File() = default;
    virtual bool write(const void *data, size_t size, std::string &error) = 0;
  };

  virtual ~SafeDest() = default;

  static std::unique_ptr<SafeDest> open(const std::string &path, std::string &error);

  virtual bool mkdir_entry(const std::string &relative, std::string &error) = 0;
  virtual bool make_symlink(const std::string &relative, const std::string &target,
                            std::string &error) = 0;
  virtual std::unique_ptr<File> create_file(const std::string &relative,
                                             std::string &error) = 0;
};

}  /* namespace archiver */

#endif
