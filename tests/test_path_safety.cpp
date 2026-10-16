#include "archiver.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect_path(const char *path, int ok) {
  ArchiverResult result = archiver_check_entry_path(path);
  int good = ok ? result == ARCHIVER_OK : result == ARCHIVER_ERR_PATH;
  if (!good) {
    fprintf(stderr, "path %s => %s, wanted %s\n", path, archiver_result_string(result),
            ok ? "ok" : "path");
    ++failures;
  }
}

static void expect_link(const char *link, const char *target, int ok) {
  ArchiverResult result = archiver_check_symlink(link, target);
  int good = ok ? result == ARCHIVER_OK : result == ARCHIVER_ERR_PATH;
  if (!good) {
    fprintf(stderr, "link %s -> %s => %s, wanted %s\n", link, target,
            archiver_result_string(result), ok ? "ok" : "path");
    ++failures;
  }
}

int main() {
  expect_path("foo/bar.txt", 1);
  expect_path("foo/./bar.txt", 1);
  expect_path("foo//bar.txt", 1);
  expect_path("../etc/passwd", 0);
  expect_path("foo/../../etc/passwd", 0);
  expect_path("foo/../bar", 0);
  expect_path("/etc/passwd", 0);
  expect_path("\\etc\\passwd", 0);
  expect_path("C:/Windows/notepad.exe", 0);
  expect_path("C:\\Windows\\notepad.exe", 0);
  expect_path("\\\\server\\share\\file", 0);
  expect_path("foo/\x01 bar", 0);
  expect_path("", 0);
  expect_path(".", 0);

  expect_link("dir/link", "file.txt", 1);
  expect_link("dir/link", "../dir/file.txt", 1);
  expect_link("sub/link", "../sub/ok", 1);
  expect_link("link", "/etc/passwd", 0);
  expect_link("link", "../outside", 0);
  expect_link("sub/link", "../../outside", 0);
  expect_link("dir/link", "C:/Windows", 0);
  expect_link("a/b/link", "foo/../../x", 1);

  if (failures) {
    fprintf(stderr, "%d path checks failed\n", failures);
    return 1;
  }
  printf("path checks passed\n");
  return 0;
}
