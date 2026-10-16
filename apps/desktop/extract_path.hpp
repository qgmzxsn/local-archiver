#ifndef LOCAL_ARCHIVER_EXTRACT_PATH_HPP
#define LOCAL_ARCHIVER_EXTRACT_PATH_HPP

#include <QString>

/* Folder name Windows will accept as a new directory. Strips archive and
   media suffixes, replaces \ / : * ? " < > | and control characters, and
   drops trailing spaces and dots. Empty, reserved, or still-illegal names
   become "解压文件". */
QString sanitizeFolderComponent(const QString &name);

/* Directory we will create for this archive. Never the archive file itself.
   A component or full path that is still too long becomes "解压文件". */
QString suggestedExtractDirectory(const QString &archivePath);

/* Last step before mkdir. An existing directory the user picked is kept.
   Anything else is sanitized the same way as the suggestion. */
QString normalizeExtractDirectory(const QString &requested, const QString &archivePath);

/* Starting directory for the folder dialog. Always an existing directory,
   never the archive file and never a folder that has not been created yet. */
QString existingDirectoryForDialog(const QString &suggested);

#endif
