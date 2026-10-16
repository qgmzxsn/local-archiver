#include "MainWindow.hpp"

#include <string.h>

#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

QString resultText(ArchiverResult result) {
  switch (result) {
    case ARCHIVER_OK: return QStringLiteral("完成");
    case ARCHIVER_ERR_PASSWORD_REQUIRED: return QStringLiteral("需要密码");
    case ARCHIVER_ERR_PASSWORD: return QStringLiteral("密码不对");
    case ARCHIVER_ERR_PATH: return QStringLiteral("已拒绝不安全的路径");
    case ARCHIVER_ERR_UNSUPPORTED: return QStringLiteral("第一阶段不支持这个文件");
    case ARCHIVER_ERR_FORMAT: return QStringLiteral("打不开这个压缩包");
    case ARCHIVER_ERR_ENGINE: return QStringLiteral("找不到 7-Zip 动态库");
    case ARCHIVER_ERR_IO: return QStringLiteral("读写文件失败");
    default: return QStringLiteral("失败");
  }
}

}  // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("本地归档"));
  resize(720, 480);

  auto *central = new QWidget(this);
  auto *layout = new QVBoxLayout(central);
  auto *buttons = new QHBoxLayout();
  auto *open_button = new QPushButton(QStringLiteral("打开"), central);
  auto *extract_button = new QPushButton(QStringLiteral("解压到…"), central);
  buttons->addWidget(open_button);
  buttons->addWidget(extract_button);
  buttons->addStretch(1);

  table_ = new QTableWidget(central);
  table_->setColumnCount(3);
  table_->setHorizontalHeaderLabels(
      {QStringLiteral("路径"), QStringLiteral("大小"), QStringLiteral("类型")});
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);

  status_ = new QLabel(QStringLiteral("打开一个 ZIP、7z、tar 或 RAR。密码只在这一次使用，不会保存。"),
                       central);
  status_->setWordWrap(true);

  layout->addLayout(buttons);
  layout->addWidget(table_);
  layout->addWidget(status_);
  setCentralWidget(central);

  connect(open_button, &QPushButton::clicked, this, &MainWindow::openArchive);
  connect(extract_button, &QPushButton::clicked, this, &MainWindow::extractArchive);
}

MainWindow::~MainWindow() { archiver_close(archive_); }

bool MainWindow::askPassword(QString *password_out) {
  bool ok = false;
  QString password = QInputDialog::getText(
      this, QStringLiteral("密码"),
      QStringLiteral("这个压缩包需要密码。输入后只用于这一次，不会保存。"), QLineEdit::Password,
      QString(), &ok);
  if (!ok) {
    password.fill(QChar(0));
    return false;
  }
  *password_out = password;
  password.fill(QChar(0));
  return true;
}

void MainWindow::showResult(ArchiverResult result) {
  QString text = resultText(result);
  const char *detail = archiver_message(archive_);
  if (detail && detail[0]) text += QStringLiteral("\n") + QString::fromUtf8(detail);
  status_->setText(text);
  if (result != ARCHIVER_OK) {
    QMessageBox::warning(this, QStringLiteral("本地归档"), text);
  }
}

void MainWindow::fillTable() {
  table_->setRowCount(0);
  uint32_t count = 0;
  if (archiver_entry_count(archive_, &count) != ARCHIVER_OK) return;
  table_->setRowCount(static_cast<int>(count));
  for (uint32_t i = 0; i < count; ++i) {
    ArchiverEntry entry;
    memset(&entry, 0, sizeof(entry));
    if (archiver_entry_at(archive_, i, &entry) != ARCHIVER_OK) continue;
    table_->setItem(static_cast<int>(i), 0, new QTableWidgetItem(QString::fromUtf8(entry.path)));
    table_->setItem(static_cast<int>(i), 1,
                    new QTableWidgetItem(QString::number(static_cast<qulonglong>(entry.size))));
    const char *kind = entry.is_directory ? "目录" : entry.is_symlink ? "链接" : "文件";
    table_->setItem(static_cast<int>(i), 2, new QTableWidgetItem(QString::fromUtf8(kind)));
    archiver_entry_free(&entry);
  }
}

void MainWindow::openArchive() {
  QString path = QFileDialog::getOpenFileName(
      this, QStringLiteral("打开压缩包"), QString(),
      QStringLiteral("压缩包 (*.zip *.7z *.tar *.tgz *.tar.gz *.tbz *.tbz2 *.tar.bz2 *.txz "
                     "*.tar.xz *.tzst *.tar.zst *.rar);;所有文件 (*)"));
  if (path.isEmpty()) return;

  archiver_close(archive_);
  archive_ = nullptr;
  archive_path_ = path;
  QByteArray path_bytes = path.toUtf8();
  ArchiverResult result = archiver_open(path_bytes.constData(), nullptr, nullptr, &archive_);
  if (result == ARCHIVER_ERR_PASSWORD_REQUIRED) {
    archiver_close(archive_);
    archive_ = nullptr;
    QString password;
    if (!askPassword(&password)) {
      status_->setText(QStringLiteral("已取消"));
      return;
    }
    QByteArray pw = password.toUtf8();
    password.fill(QChar(0));
    result = archiver_open(path_bytes.constData(), nullptr, pw.constData(), &archive_);
    pw.fill('\0');
  }
  if (result != ARCHIVER_OK) {
    showResult(result);
    archiver_close(archive_);
    archive_ = nullptr;
    table_->setRowCount(0);
    return;
  }
  fillTable();
  status_->setText(QStringLiteral("已打开。解压不会运行里面的程序。"));
}

void MainWindow::extractArchive() {
  if (!archive_) {
    status_->setText(QStringLiteral("先打开一个压缩包。"));
    return;
  }
  QString dest = QFileDialog::getExistingDirectory(this, QStringLiteral("解压到"));
  if (dest.isEmpty()) return;
  QByteArray dest_bytes = dest.toUtf8();
  ArchiverResult result = archiver_extract(archive_, dest_bytes.constData(), nullptr);
  if (result == ARCHIVER_ERR_PASSWORD_REQUIRED) {
    QString password;
    if (!askPassword(&password)) {
      status_->setText(QStringLiteral("已取消"));
      return;
    }
    QByteArray pw = password.toUtf8();
    password.fill(QChar(0));
    result = archiver_extract(archive_, dest_bytes.constData(), pw.constData());
    pw.fill('\0');
  }
  showResult(result);
}
