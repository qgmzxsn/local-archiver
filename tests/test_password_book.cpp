// Uses a separate Credential Manager namespace; never reads or changes the user's book.
#include "password_book.hpp"
#include <QtTest>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#include <cstring>

class PasswordBookTests : public QObject {
  Q_OBJECT
 private slots:
  void init() { QVERIFY(PasswordBook::store({})); }
  void cleanup() {
    QVERIFY(PasswordBook::store({}));
    ::CredDeleteW(L"local-archiver/tests/password-book", CRED_TYPE_GENERIC, 0);
  }
  void fullCapacityRoundTrips() {
    QVector<QByteArray> values;
    for (int i = 0; i < PasswordBook::maxPasswords; ++i)
      values.append(QByteArray::number(i).leftJustified(PasswordBook::maxPasswordBytes, 'x'));
    QVERIFY(PasswordBook::store(values));
    QCOMPARE(PasswordBook::load(), values);
    auto tooMany = values;
    tooMany.append("extra");
    QVERIFY(!PasswordBook::store(tooMany));
    QCOMPARE(PasswordBook::load(), values);
    QVERIFY(!PasswordBook::store({QByteArray(PasswordBook::maxPasswordBytes + 1, 'x')}));
    QCOMPARE(PasswordBook::load(), values);
  }
  void migrateLegacyCredential() {
    QByteArray legacy;
    auto append = [&](uint32_t value) { legacy.append(reinterpret_cast<const char *>(&value), 4); };
    append(0x3142504c); append(2);
    const QVector<QByteArray> values{"old password", QStringLiteral("旧密码🔑").toUtf8()};
    for (const auto &value : values) { append(static_cast<uint32_t>(value.size())); legacy.append(value); }
    CREDENTIALW cred = {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<wchar_t *>(L"local-archiver/tests/password-book");
    cred.UserName = const_cast<wchar_t *>(L"test");
    cred.CredentialBlobSize = static_cast<DWORD>(legacy.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(legacy.data());
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    QVERIFY(::CredWriteW(&cred, 0));
    QCOMPARE(PasswordBook::load(), values);
    QVERIFY(PasswordBook::store(values));
    QCOMPARE(PasswordBook::load(), values);
  }
};
QTEST_GUILESS_MAIN(PasswordBookTests)
#include "test_password_book.moc"
