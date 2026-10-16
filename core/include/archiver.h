/* Original code in this tree is MIT. See LICENSE.
   7-Zip is not MIT: LICENSES/7-zip-license.txt and LICENSES/lgpl-2.1.txt.
   RAR decoding is not MIT: LICENSES/unrar-license.txt. */

#ifndef LOCAL_ARCHIVER_ARCHIVER_H
#define LOCAL_ARCHIVER_ARCHIVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(ARCHIVER_BUILDING)
#    define ARCHIVER_API __declspec(dllexport)
#  else
#    define ARCHIVER_API __declspec(dllimport)
#  endif
#else
#  define ARCHIVER_API __attribute__((visibility("default")))
#endif

#define ARCHIVER_VERSION "0.2.0"

typedef enum ArchiverResult {
  ARCHIVER_OK = 0,
  ARCHIVER_ERR_ARGUMENT = 1,
  ARCHIVER_ERR_IO = 2,
  ARCHIVER_ERR_FORMAT = 3,
  ARCHIVER_ERR_ENGINE = 4,
  ARCHIVER_ERR_PASSWORD_REQUIRED = 5,
  ARCHIVER_ERR_PASSWORD = 6,
  ARCHIVER_ERR_PATH = 7,
  ARCHIVER_ERR_UNSUPPORTED = 8,
  ARCHIVER_ERR_CANCELLED = 9
} ArchiverResult;

typedef struct ArchiverArchive ArchiverArchive;

typedef struct ArchiverEntry {
  char *path;
  int is_directory;
  int is_symlink;
  uint64_t size;
} ArchiverEntry;

/* password is used only for this call. NULL means the user did not type one.
   The library does not write it to disk or to any log.
   engine_path is 7z.so, 7z.dll, or lib7z.dylib. NULL searches next to this
   library and the ARCHIVER_7ZIP variable.
   On failure *out may still be set so archiver_message() can be read.
   If *out is set, the caller must archiver_close() it. */
ARCHIVER_API ArchiverResult archiver_open(const char *archive_path,
                                           const char *engine_path,
                                           const char *password,
                                           ArchiverArchive **out);

ARCHIVER_API ArchiverResult archiver_entry_count(const ArchiverArchive *archive,
                                                  uint32_t *count);
ARCHIVER_API ArchiverResult archiver_entry_at(const ArchiverArchive *archive,
                                               uint32_t index,
                                               ArchiverEntry *out);
ARCHIVER_API void archiver_entry_free(ArchiverEntry *entry);

ARCHIVER_API ArchiverResult archiver_extract(ArchiverArchive *archive,
                                              const char *dest_dir,
                                              const char *password);

/* Runs synchronously on the caller's thread. The callback must not call back
   into this archive. Return nonzero to cancel; the current incomplete file is
   removed, while completed files/directories may remain. No callback is kept
   after this function returns. total may be zero when it is not yet known. */
typedef int (*ArchiverProgressCallback)(uint64_t completed, uint64_t total, void *user);
ARCHIVER_API ArchiverResult archiver_extract_with_progress(
    ArchiverArchive *archive, const char *dest_dir, const char *password,
    ArchiverProgressCallback progress, void *user);

ARCHIVER_API void archiver_close(ArchiverArchive *archive);

/* Stable English token such as "ok" or "password-required". */
ARCHIVER_API const char *archiver_result_string(ArchiverResult result);

/* Empty string when there is no extra detail. Never contains a password. */
ARCHIVER_API const char *archiver_message(const ArchiverArchive *archive);

ARCHIVER_API ArchiverResult archiver_check_entry_path(const char *path);
ARCHIVER_API ArchiverResult archiver_check_symlink(const char *link_path,
                                                    const char *target);

#ifdef __cplusplus
}
#endif

#endif
