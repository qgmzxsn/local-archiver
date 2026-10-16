#ifndef LOCAL_ARCHIVER_PASSWORD_BOOK_HPP
#define LOCAL_ARCHIVER_PASSWORD_BOOK_HPP

#include <QByteArray>
#include <QVector>

/* Windows Credential Manager only. Other platforms keep an empty book. */
class PasswordBook {
 public:
  static bool available();
  static QVector<QByteArray> load();
  static bool store(const QVector<QByteArray> &passwords);
};

#endif
