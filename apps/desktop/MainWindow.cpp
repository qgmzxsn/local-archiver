#include "MainWindow.hpp"
#include "extract_path.hpp"
#include "password_book.hpp"
#include "archiver.h"

#include <QAbstractTableModel>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <exception>

struct DesktopSecret {
  QByteArray bytes;
  ~DesktopSecret() {
    volatile char *data = bytes.data();
    for (qsizetype i = 0; i < bytes.size(); ++i) data[i] = 0;
  }
};
struct ArchiveSession {
  ArchiverArchive *handle = nullptr;
  std::shared_ptr<DesktopSecret> secret;
  ~ArchiveSession() { archiver_close(handle); }
};
struct DesktopWorkState {
  std::atomic<bool> cancel{false};
  std::atomic<uint64_t> completed{0};
  std::atomic<uint64_t> total{0};
};
struct ArchiveRow {
  QString name;
  QString path;
  uint64_t size = 0;
  uint64_t packed = 0;
  qint64 modified = -1;
  bool directory = false;
  bool symlink = false;
  bool parent = false;
  bool archive = false;
  bool hasPacked = false;
};

class ArchiveModel final : public QAbstractTableModel {
 public:
  explicit ArchiveModel(QObject *parent) : QAbstractTableModel(parent) {}
  int rowCount(const QModelIndex &parent = {}) const override {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
  }
  int columnCount(const QModelIndex &parent = {}) const override {
    return parent.isValid() ? 0 : 4;
  }
  QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
      if (section == 0) return QStringLiteral("名称");
      if (section == 1) return QStringLiteral("大小");
      if (section == 2) return QStringLiteral("压缩后大小");
      if (section == 3) return QStringLiteral("修改时间");
    }
    return QAbstractTableModel::headerData(section, orientation, role);
  }
  QVariant data(const QModelIndex &index, int role) const override {
    if (!index.isValid() || index.row() >= rows_.size()) return {};
    const auto &row = rows_[index.row()];
    if (role == Qt::TextAlignmentRole && (index.column() == 1 || index.column() == 2))
      return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::ToolTipRole) return row.path;
    if (role != Qt::DisplayRole) return {};
    if (index.column() == 0) return row.name;
    if (index.column() == 1) {
      if (row.parent) return {};
      return QLocale().formattedDataSize(row.size);
    }
    if (index.column() == 2) {
      if (!row.hasPacked) return {};
      return QLocale().formattedDataSize(row.packed);
    }
    if (row.modified < 0) return {};
    return QLocale().toString(QDateTime::fromSecsSinceEpoch(row.modified), QLocale::ShortFormat);
  }
  const ArchiveRow *rowAt(int row) const {
    if (row < 0 || row >= rows_.size()) return nullptr;
    return &rows_[row];
  }
  void setRows(QVector<ArchiveRow> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
  }
  uint64_t totalSize() const {
    uint64_t total = 0;
    for (const auto &row : rows_)
      if (!row.parent && !row.directory) total += row.size;
    return total;
  }
 private:
  QVector<ArchiveRow> rows_;
};

namespace {
int reportProgress(uint64_t completed, uint64_t total, void *user) {
  auto *state = static_cast<DesktopWorkState *>(user);
  state->completed.store(completed, std::memory_order_relaxed);
  state->total.store(total, std::memory_order_relaxed);
  return state->cancel.load(std::memory_order_relaxed) ? 1 : 0;
}

bool looksLikeArchive(const QString &name) {
  const QString lower = name.toLower();
  const QStringList suffixes = {
      QStringLiteral(".zip"), QStringLiteral(".7z"), QStringLiteral(".tar"),
      QStringLiteral(".tgz"), QStringLiteral(".tar.gz"), QStringLiteral(".tbz"),
      QStringLiteral(".tbz2"), QStringLiteral(".tar.bz2"), QStringLiteral(".txz"),
      QStringLiteral(".tar.xz"), QStringLiteral(".tzst"), QStringLiteral(".tar.zst"),
      QStringLiteral(".rar"), QStringLiteral(".mp4"), QStringLiteral(".m4v"),
      QStringLiteral(".mov"), QStringLiteral(".jpg"), QStringLiteral(".jpeg"),
      QStringLiteral(".png"), QStringLiteral(".gif"), QStringLiteral(".bmp"),
      QStringLiteral(".webp")};
  for (const auto &suffix : suffixes)
    if (lower.endsWith(suffix)) return true;
  const int dot = lower.lastIndexOf(QLatin1Char('.'));
  if (dot < 0) return false;
  const QString ext = lower.mid(dot + 1);
  if (ext.size() >= 2 && ext.size() <= 4) {
    bool digits = true;
    for (const QChar ch : ext) digits = digits && ch.isDigit();
    if (digits) return true;
  }
  if (ext.size() == 3 && (ext[0] == QLatin1Char('z') || ext[0] == QLatin1Char('r')) &&
      ext[1].isDigit() && ext[2].isDigit())
    return true;
  return lower.contains(QStringLiteral(".part")) && lower.endsWith(QStringLiteral(".rar"));
}

QString errorText(int code, const QString &detail) {
  switch (code) {
    case ARCHIVER_ERR_PATH:
      return QStringLiteral("已停止：压缩包或输出路径包含不安全的路径或链接。");
    case ARCHIVER_ERR_MISSING_VOLUME:
      return QStringLiteral("缺少分卷。%1").arg(detail);
    case ARCHIVER_ERR_NEED_CHOICE:
      return QStringLiteral("这一层有多个压缩包，需要先选择一个。\n%1").arg(detail);
    case ARCHIVER_ERR_LIMIT:
      return QStringLiteral("已达到嵌套解压上限。%1").arg(detail);
    case ARCHIVER_ERR_UNSUPPORTED:
      if (detail.contains(QStringLiteral("more data")))
        return QStringLiteral("压缩包展开后的大小超过本版本的 64 GiB 上限。");
      if (detail.contains(QStringLiteral("too many entries")))
        return QStringLiteral("压缩包包含超过 100 万个文件条目，暂时无法打开。");
      if (detail.contains(QStringLiteral("no archive")))
        return QStringLiteral("这个视频或图片里没有压缩包。");
      return detail.contains(QStringLiteral("symlink"))
                 ? QStringLiteral("这个压缩包包含当前 Windows 版不支持创建的符号链接。")
                 : QStringLiteral("打不开这个文件。");
    case ARCHIVER_ERR_ENGINE:
      return QStringLiteral("无法加载解压引擎。请完整解压程序包，保留程序旁的所有文件后重试。");
    case ARCHIVER_ERR_IO:
      return detail.contains(QStringLiteral("already exists"))
                 ? QStringLiteral("输出位置已有同名文件，原文件已保留。请选择一个空文件夹后重试。")
                 : QStringLiteral("无法读取或写入文件。请检查文件是否可读、输出位置是否可写，以及磁盘空间。");
    case ARCHIVER_ERR_FORMAT:
      return QStringLiteral("无法完整读取这个压缩包。文件可能损坏、不完整，或与扩展名不符。");
    case ARCHIVER_ERR_PASSWORD:
      return QStringLiteral("密码不对。");
    default:
      return QStringLiteral("操作失败，请重新选择文件后再试。");
  }
}

struct OpenResult {
  ArchiverResult code = ARCHIVER_OK;
  QString detail;
  std::shared_ptr<ArchiveSession> session;
  QVector<ArchiveRow> rows;
  uint64_t total = 0;
  int matched = -1;
};
struct ExtractResult {
  ArchiverResult code = ARCHIVER_OK;
  QString detail;
};

void wipeList(QVector<QByteArray> &items) {
  for (QByteArray &item : items) {
    volatile char *data = item.data();
    for (qsizetype i = 0; i < item.size(); ++i) data[i] = 0;
    item.clear();
  }
  items.clear();
}
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("本地归档"));
  resize(900, 560);
  setMinimumSize(640, 400);
  setAcceptDrops(true);

  auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件"));
  auto *openAction = fileMenu->addAction(QStringLiteral("打开…"));
  openAction->setShortcut(QKeySequence::Open);
  connect(openAction, &QAction::triggered, this, &MainWindow::chooseArchive);
  connect(fileMenu->addAction(QStringLiteral("添加…")), &QAction::triggered, this, &MainWindow::addFiles);
  connect(fileMenu->addAction(QStringLiteral("解压…")), &QAction::triggered, this, [this] {
    extractButton_->click();
  });
  connect(fileMenu->addAction(QStringLiteral("测试")), &QAction::triggered, this, &MainWindow::testArchive);
  fileMenu->addSeparator();
  connect(fileMenu->addAction(QStringLiteral("退出")), &QAction::triggered, this, &QWidget::close);

  auto *editMenu = menuBar()->addMenu(QStringLiteral("编辑"));
  connect(editMenu->addAction(QStringLiteral("全选")), &QAction::triggered, this, [this] {
    table_->selectAll();
  });
  auto *viewMenu = menuBar()->addMenu(QStringLiteral("查看"));
  connect(viewMenu->addAction(QStringLiteral("向上")), &QAction::triggered, this, &MainWindow::goUp);
  auto *toolsMenu = menuBar()->addMenu(QStringLiteral("工具"));
  connect(toolsMenu->addAction(QStringLiteral("密码本…")), &QAction::triggered, this,
          &MainWindow::showPasswordBook);
  auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助"));
  connect(helpMenu->addAction(QStringLiteral("关于")), &QAction::triggered, this, [this] {
    QMessageBox::about(
        this, QStringLiteral("关于本地归档"),
        QStringLiteral("本地归档 %1\n\n查看和解压本机压缩包。密码本只写在 Windows 凭据管理器里。\n"
                       "不会创建 RAR，也不会运行解压出来的程序。")
            .arg(QLatin1String(ARCHIVER_VERSION)));
  });

  auto *toolbar = addToolBar(QStringLiteral("标准"));
  toolbar->setMovable(false);
  toolbar->setFloatable(false);
  auto addFlat = [&](const QString &text, const char *name) {
    auto *button = new QPushButton(text, toolbar);
    button->setObjectName(name);
    button->setFlat(true);
    toolbar->addWidget(button);
    return button;
  };
  addButton_ = addFlat(QStringLiteral("添加"), "addButton");
  extractButton_ = addFlat(QStringLiteral("解压"), "extractButton");
  testButton_ = addFlat(QStringLiteral("测试"), "testButton");
  infoButton_ = addFlat(QStringLiteral("信息"), "infoButton");
  openButton_ = addFlat(QStringLiteral("打开"), "openButton");
  upButton_ = addFlat(QStringLiteral("向上"), "upButton");

  auto *central = new QWidget(this);
  auto *layout = new QVBoxLayout(central);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  pathBar_ = new QLineEdit(central);
  pathBar_->setObjectName(QStringLiteral("pathBar"));
  pathBar_->setClearButtonEnabled(false);
  layout->addWidget(pathBar_);
  model_ = new ArchiveModel(this);
  table_ = new QTableView(central);
  table_->setObjectName(QStringLiteral("archiveTable"));
  table_->setModel(model_);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table_->setShowGrid(false);
  table_->verticalHeader()->hide();
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table_->setColumnWidth(1, 110);
  table_->setColumnWidth(2, 110);
  table_->setColumnWidth(3, 160);
  layout->addWidget(table_, 1);
  setCentralWidget(central);

  progress_ = new QProgressBar(this);
  progress_->setObjectName(QStringLiteral("progressBar"));
  progress_->setVisible(false);
  cancelButton_ = new QPushButton(QStringLiteral("取消解压"), this);
  cancelButton_->setObjectName(QStringLiteral("cancelButton"));
  cancelButton_->setVisible(false);
  status_ = new QLabel(this);
  status_->setObjectName(QStringLiteral("statusLabel"));
  status_->setTextFormat(Qt::PlainText);
  statusBar()->addWidget(status_, 1);
  statusBar()->addPermanentWidget(progress_);
  statusBar()->addPermanentWidget(cancelButton_);

  connect(openButton_, &QPushButton::clicked, this, &MainWindow::chooseArchive);
  connect(addButton_, &QPushButton::clicked, this, &MainWindow::addFiles);
  connect(extractButton_, &QPushButton::clicked, this, [this] {
    if (busy_ || !archive_) return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("解压"));
    auto *box = new QVBoxLayout(&dialog);
    auto *edit = new QLineEdit(QDir::toNativeSeparators(suggestedExtractDirectory(archivePath_)), &dialog);
    edit->setObjectName(QStringLiteral("destinationEdit"));
    auto *browse = new QPushButton(QStringLiteral("浏览…"), &dialog);
    auto *row = new QHBoxLayout();
    row->addWidget(edit, 1);
    row->addWidget(browse);
    box->addLayout(row);
    auto *nested = new QCheckBox(QStringLiteral("穿透嵌套压缩包"), &dialog);
    nested->setChecked(true);
    nested->setObjectName(QStringLiteral("nestedCheck"));
    box->addWidget(nested);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto *ok = buttons->button(QDialogButtonBox::Ok);
    ok->setObjectName(QStringLiteral("extractOk"));
    ok->setText(QStringLiteral("确定"));
    box->addWidget(buttons);
    connect(browse, &QPushButton::clicked, &dialog, [this, edit] {
      const QString start = existingDirectoryForDialog(edit->text());
      const QString path =
          QFileDialog::getExistingDirectory(this, QStringLiteral("选择输出文件夹"), start);
      if (!path.isEmpty()) edit->setText(QDir::toNativeSeparators(path));
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    startExtract(normalizeExtractDirectory(edit->text(), archivePath_), nested->isChecked());
  });
  connect(testButton_, &QPushButton::clicked, this, &MainWindow::testArchive);
  connect(infoButton_, &QPushButton::clicked, this, &MainWindow::showInfo);
  connect(upButton_, &QPushButton::clicked, this, &MainWindow::goUp);
  connect(cancelButton_, &QPushButton::clicked, this, &MainWindow::cancelExtraction);
  connect(pathBar_, &QLineEdit::returnPressed, this, [this] {
    const QString path = QDir::fromNativeSeparators(pathBar_->text().trimmed());
    QFileInfo info(path);
    if (info.isDir()) browseDirectory(info.absoluteFilePath());
    else if (info.isFile()) openPath(info.absoluteFilePath());
  });
  connect(table_, &QTableView::doubleClicked, this, &MainWindow::activateRow);
  timer_ = new QTimer(this);
  timer_->setInterval(100);
  connect(timer_, &QTimer::timeout, this, &MainWindow::refreshProgress);
  setBusy(false);
  browseDirectory(QDir::homePath());
}

MainWindow::~MainWindow() = default;

void MainWindow::setBusy(bool busy, bool extracting) {
  busy_ = busy;
  extracting_ = extracting;
  openButton_->setEnabled(!busy);
  addButton_->setEnabled(!busy && !insideArchive_);
  extractButton_->setEnabled(!busy && archive_ != nullptr);
  testButton_->setEnabled(!busy && archive_ != nullptr);
  infoButton_->setEnabled(!busy);
  upButton_->setEnabled(!busy);
  pathBar_->setEnabled(!busy);
  cancelButton_->setVisible(busy && extracting);
  cancelButton_->setEnabled(busy && extracting);
  progress_->setVisible(busy);
  if (busy) {
    progress_->setRange(0, 0);
    timer_->start();
  } else {
    timer_->stop();
  }
}

void MainWindow::browseDirectory(const QString &path) {
  QFileInfo info(path);
  if (!info.isDir()) return;
  insideArchive_ = false;
  archive_.reset();
  archiveRows_.clear();
  archivePrefix_.clear();
  directory_ = info.absoluteFilePath();
  pathBar_->setText(QDir::toNativeSeparators(directory_));
  setWindowTitle(QStringLiteral("本地归档"));
  QVector<ArchiveRow> rows;
  if (!info.isRoot()) {
    ArchiveRow parent;
    parent.name = QStringLiteral("..");
    parent.path = info.dir().absolutePath();
    parent.directory = true;
    parent.parent = true;
    rows.push_back(parent);
  }
  const auto entries = QDir(directory_).entryInfoList(
      QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::DirsFirst | QDir::Name);
  for (const QFileInfo &entry : entries) {
    ArchiveRow row;
    row.name = entry.fileName();
    row.path = entry.absoluteFilePath();
    row.size = entry.isDir() ? 0 : static_cast<uint64_t>(entry.size());
    row.modified = entry.lastModified().toSecsSinceEpoch();
    row.directory = entry.isDir();
    row.symlink = entry.isSymLink();
    row.archive = !entry.isDir() && looksLikeArchive(entry.fileName());
    rows.push_back(row);
  }
  model_->setRows(rows);
  status_->setText(QStringLiteral("%1 个对象").arg(entries.size()));
  setBusy(false);
}

void MainWindow::fillArchiveRows(const QVector<ArchiveRow> &rows) {
  QVector<ArchiveRow> view;
  if (!archivePrefix_.isEmpty()) {
    ArchiveRow parent;
    parent.name = QStringLiteral("..");
    parent.parent = true;
    parent.directory = true;
    view.push_back(parent);
  }
  QStringList folders;
  for (const ArchiveRow &row : rows) {
    if (!row.path.startsWith(archivePrefix_)) continue;
    QString rest = row.path.mid(archivePrefix_.size());
    if (rest.isEmpty()) continue;
    const int slash = rest.indexOf(QLatin1Char('/'));
    if (slash >= 0) {
      const QString folder = rest.left(slash);
      if (!folders.contains(folder)) {
        folders.push_back(folder);
        ArchiveRow dir;
        dir.name = folder;
        dir.path = archivePrefix_ + folder + QLatin1Char('/');
        dir.directory = true;
        view.push_back(dir);
      }
    } else if (!row.directory) {
      ArchiveRow file = row;
      file.name = rest;
      view.push_back(file);
    } else {
      ArchiveRow dir = row;
      dir.name = rest;
      view.push_back(dir);
    }
  }
  model_->setRows(view);
}

void MainWindow::enterArchiveFolder(const QString &prefix) {
  archivePrefix_ = prefix;
  pathBar_->setText(QDir::toNativeSeparators(archivePath_) + QLatin1Char('/') +
                    archivePrefix_.left(archivePrefix_.size() - (archivePrefix_.endsWith(QLatin1Char('/')) ? 1 : 0)));
  fillArchiveRows(archiveRows_);
}

void MainWindow::goUp() {
  if (busy_) return;
  if (insideArchive_) {
    if (!archivePrefix_.isEmpty()) {
      QString prefix = archivePrefix_;
      if (prefix.endsWith(QLatin1Char('/'))) prefix.chop(1);
      const int slash = prefix.lastIndexOf(QLatin1Char('/'));
      enterArchiveFolder(slash < 0 ? QString() : prefix.left(slash + 1));
      return;
    }
    const QString back = QFileInfo(archivePath_).absolutePath();
    browseDirectory(back);
    return;
  }
  QDir dir(directory_);
  if (dir.cdUp()) browseDirectory(dir.absolutePath());
}

void MainWindow::activateRow(const QModelIndex &index) {
  if (busy_) return;
  const ArchiveRow *row = model_->rowAt(index.row());
  if (!row) return;
  if (row->parent) {
    goUp();
    return;
  }
  if (insideArchive_) {
    if (row->directory) enterArchiveFolder(row->path.endsWith(QLatin1Char('/')) ? row->path : row->path + QLatin1Char('/'));
    return;
  }
  if (row->directory) browseDirectory(row->path);
  else if (row->archive || looksLikeArchive(row->name)) openPath(row->path);
}

void MainWindow::chooseArchive() {
  if (busy_) return;
  const QString path = QFileDialog::getOpenFileName(
      this, QStringLiteral("打开"), directory_,
      QStringLiteral("压缩包 (*.zip *.7z *.tar *.tgz *.tar.gz *.tbz *.tbz2 *.tar.bz2 "
                     "*.txz *.tar.xz *.tzst *.tar.zst *.rar *.001 *.mp4 *.jpg *.jpeg *.png);;所有文件 (*)"));
  if (!path.isEmpty()) openPath(path);
}

void MainWindow::addFiles() {
  if (busy_ || insideArchive_) return;
  const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("添加到压缩包"), directory_);
  if (files.isEmpty()) return;
  const QString zip = QFileDialog::getSaveFileName(this, QStringLiteral("保存 ZIP"), directory_,
                                                   QStringLiteral("ZIP (*.zip)"));
  if (zip.isEmpty()) return;
  QByteArray zipBytes = zip.toUtf8();
  QVector<QByteArray> stored;
  QVector<const char *> views;
  for (const QString &file : files) {
    stored.append(file.toUtf8());
    views.append(stored.back().constData());
  }
  const int code = archiver_create_zip(zipBytes.constData(), views.data(), static_cast<uint32_t>(views.size()));
  if (code != ARCHIVER_OK) showError(code, {});
  else {
    status_->setText(QStringLiteral("已创建 %1").arg(QDir::toNativeSeparators(zip)));
    browseDirectory(QFileInfo(zip).absolutePath());
  }
}

void MainWindow::testArchive() {
  if (busy_ || !archive_) return;
  const QByteArray password = archive_->secret ? archive_->secret->bytes : QByteArray();
  const int code = archiver_test(archive_->handle, password.isEmpty() ? nullptr : password.constData());
  const QString detail = QString::fromUtf8(archiver_message(archive_->handle));
  if (code == ARCHIVER_OK) status_->setText(QStringLiteral("测试通过。"));
  else showError(code, detail);
}

void MainWindow::showInfo() {
  if (insideArchive_) {
    uint32_t count = 0;
    archiver_entry_count(archive_->handle, &count);
    uint64_t bytes = 0;
    uint64_t packed = 0;
    for (const ArchiveRow &row : archiveRows_) {
      if (!row.directory) {
        bytes += row.size;
        packed += row.packed;
      }
    }
    QMessageBox::information(
        this, QStringLiteral("信息"),
        QStringLiteral("文件：%1\n大小：%2\n压缩后大小：%3\n%4")
            .arg(count)
            .arg(QLocale().formattedDataSize(bytes))
            .arg(QLocale().formattedDataSize(packed))
            .arg(QDir::toNativeSeparators(archivePath_)));
    return;
  }
  status_->setText(QStringLiteral("%1").arg(QDir::toNativeSeparators(directory_)));
}

void MainWindow::showPasswordBook() {
  if (!PasswordBook::available()) {
    QMessageBox::information(this, QStringLiteral("密码本"),
                             QStringLiteral("密码本只保存在 Windows 凭据管理器中。"));
    return;
  }
  QVector<QByteArray> items = PasswordBook::load();
  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("密码本"));
  auto *layout = new QVBoxLayout(&dialog);
  auto *list = new QListWidget(&dialog);
  auto refill = [&] {
    list->clear();
    for (int i = 0; i < items.size(); ++i)
      list->addItem(QStringLiteral("密码 %1").arg(i + 1));
  };
  refill();
  layout->addWidget(list);
  auto *row = new QHBoxLayout();
  auto *up = new QPushButton(QStringLiteral("上移"), &dialog);
  auto *down = new QPushButton(QStringLiteral("下移"), &dialog);
  auto *remove = new QPushButton(QStringLiteral("删除"), &dialog);
  row->addWidget(up);
  row->addWidget(down);
  row->addWidget(remove);
  layout->addLayout(row);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  connect(up, &QPushButton::clicked, &dialog, [&] {
    const int rowIndex = list->currentRow();
    if (rowIndex <= 0) return;
    std::swap(items[rowIndex], items[rowIndex - 1]);
    if (!PasswordBook::store(items)) showError(ARCHIVER_ERR_IO, {});
    refill();
    list->setCurrentRow(rowIndex - 1);
  });
  connect(down, &QPushButton::clicked, &dialog, [&] {
    const int rowIndex = list->currentRow();
    if (rowIndex < 0 || rowIndex + 1 >= items.size()) return;
    std::swap(items[rowIndex], items[rowIndex + 1]);
    if (!PasswordBook::store(items)) showError(ARCHIVER_ERR_IO, {});
    refill();
    list->setCurrentRow(rowIndex + 1);
  });
  connect(remove, &QPushButton::clicked, &dialog, [&] {
    const int rowIndex = list->currentRow();
    if (rowIndex < 0) return;
    volatile char *data = items[rowIndex].data();
    for (qsizetype i = 0; i < items[rowIndex].size(); ++i) data[i] = 0;
    items.removeAt(rowIndex);
    if (!PasswordBook::store(items)) showError(ARCHIVER_ERR_IO, {});
    refill();
  });
  dialog.exec();
  wipeList(items);
}

void MainWindow::openPath(const QString &path) {
  if (busy_) return;
  if (!QFileInfo(path).isFile()) {
    showError(ARCHIVER_ERR_IO, {});
    return;
  }
  archive_.reset();
  model_->setRows({});
  archiveRows_.clear();
  archivePrefix_.clear();
  insideArchive_ = false;
  archivePath_ = QFileInfo(path).absoluteFilePath();
  directory_ = QFileInfo(path).absolutePath();
  pathBar_->setText(QDir::toNativeSeparators(archivePath_));
  setWindowTitle(QFileInfo(path).fileName() + QStringLiteral(" — 本地归档"));
  startOpen(archivePath_);
}

std::shared_ptr<DesktopSecret> MainWindow::askPassword(bool rejected, bool *save) {
  if (save) *save = false;
#if defined(_WIN32)
  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("压缩包密码"));
  auto *layout = new QVBoxLayout(&dialog);
  layout->addWidget(new QLabel(rejected ? QStringLiteral("密码不正确，请重试。")
                                        : QStringLiteral("请输入密码。"),
                               &dialog));
  auto *edit = new QLineEdit(&dialog);
  edit->setEchoMode(QLineEdit::Password);
  layout->addWidget(edit);
  auto *keep = new QCheckBox(QStringLiteral("保存到密码本"), &dialog);
  layout->addWidget(keep);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted) return {};
  if (save) *save = keep->isChecked();
  auto secret = std::make_shared<DesktopSecret>();
  secret->bytes = edit->text().toUtf8();
  edit->clear();
  return secret;
#else
  bool accepted = false;
  QString password = QInputDialog::getText(
      this, QStringLiteral("压缩包密码"),
      rejected ? QStringLiteral("密码不正确，请重试。关闭压缩包后会清除本次密码。")
               : QStringLiteral("请输入密码。仅在本次打开期间使用，不会保存到文件。"),
      QLineEdit::Password, {}, &accepted);
  if (!accepted) {
    password.fill(QChar(0));
    return {};
  }
  auto secret = std::make_shared<DesktopSecret>();
  secret->bytes = password.toUtf8();
  password.fill(QChar(0));
  return secret;
#endif
}

void MainWindow::startOpen(const QString &path, std::shared_ptr<DesktopSecret> secret) {
  work_ = std::make_shared<DesktopWorkState>();
  auto state = work_;
  setBusy(true);
  status_->setText(QStringLiteral("正在打开…"));
  auto *watcher = new QFutureWatcher<OpenResult>(this);
  connect(watcher, &QFutureWatcher<OpenResult>::finished, this, [this, watcher, path] {
    OpenResult result = watcher->result();
    watcher->deleteLater();
    if (result.code == ARCHIVER_OK) {
      archive_ = result.session;
      insideArchive_ = true;
      archiveRows_ = result.rows;
    }
    setBusy(false);
    if (closeAfterFinish_) {
      close();
      return;
    }
    if (result.code == ARCHIVER_ERR_PASSWORD_REQUIRED || result.code == ARCHIVER_ERR_PASSWORD) {
      bool save = false;
      auto nextSecret = askPassword(result.code == ARCHIVER_ERR_PASSWORD, &save);
      if (nextSecret) {
        if (save) {
          QVector<QByteArray> book = PasswordBook::load();
          book.append(nextSecret->bytes);
          PasswordBook::store(book);
          wipeList(book);
        }
        startOpen(path, std::move(nextSecret));
      } else {
        insideArchive_ = false;
        model_->setRows({});
        status_->setText(QStringLiteral("已取消密码输入。"));
      }
      return;
    }
    if (result.code != ARCHIVER_OK) {
      insideArchive_ = false;
      showError(result.code, result.detail);
      browseDirectory(QFileInfo(path).absolutePath());
      return;
    }
    archivePrefix_.clear();
    fillArchiveRows(archiveRows_);
    status_->setText(QStringLiteral("共 %1 项 · %2")
                         .arg(QLocale().toString(static_cast<qlonglong>(result.rows.size())))
                         .arg(QLocale().formattedDataSize(result.total)));
  });
  watcher->setFuture(QtConcurrent::run([path, secret, state] {
    OpenResult result;
    result.session = std::make_shared<ArchiveSession>();
    QVector<QByteArray> book = PasswordBook::load();
    if (secret && !secret->bytes.isEmpty()) book.prepend(secret->bytes);
    QVector<const char *> views;
    for (const QByteArray &item : book) views.append(item.constData());
    try {
      const QByteArray pathBytes = path.toUtf8();
      int32_t matched = -1;
      result.code = archiver_open_with_passwords(
          pathBytes.constData(), nullptr, views.isEmpty() ? nullptr : views.data(),
          static_cast<uint32_t>(views.size()), &matched, &result.session->handle);
      result.detail = QString::fromUtf8(archiver_message(result.session->handle));
      result.matched = matched;
      if (result.code == ARCHIVER_OK && matched >= 0 && matched < book.size()) {
        result.session->secret = std::make_shared<DesktopSecret>();
        result.session->secret->bytes = book.at(matched);
      }
      if (result.code == ARCHIVER_OK) {
        uint32_t count = 0;
        result.code = archiver_entry_count(result.session->handle, &count);
        result.rows.reserve(static_cast<int>(count));
        for (uint32_t i = 0; result.code == ARCHIVER_OK && i < count; ++i) {
          if (state->cancel.load()) {
            result.code = ARCHIVER_ERR_CANCELLED;
            break;
          }
          ArchiverEntry entry = {};
          result.code = archiver_entry_at(result.session->handle, i, &entry);
          if (result.code != ARCHIVER_OK) break;
          ArchiveRow row;
          row.path = QString::fromUtf8(entry.path);
          row.name = row.path;
          row.size = entry.size;
          row.packed = entry.packed_size;
          row.hasPacked = true;
          row.modified = entry.modified_unix;
          row.directory = entry.is_directory != 0;
          row.symlink = entry.is_symlink != 0;
          archiver_entry_free(&entry);
          if (!row.directory) result.total += row.size;
          result.rows.push_back(std::move(row));
        }
      }
    } catch (const std::exception &) {
      result.code = ARCHIVER_ERR_IO;
    } catch (...) {
      result.code = ARCHIVER_ERR_FORMAT;
    }
    wipeList(book);
    return result;
  }));
}

void MainWindow::startExtract(const QString &dest, bool nested, std::shared_ptr<DesktopSecret> secret) {
  if (busy_ || !archive_) return;
  if (dest.isEmpty() || !QDir::isAbsolutePath(dest)) {
    status_->setText(QStringLiteral("请选择一个完整的输出文件夹路径。"));
    return;
  }
  auto session = archive_;
  if (!secret) secret = session->secret;
  work_ = std::make_shared<DesktopWorkState>();
  auto state = work_;
  const QString path = archivePath_;
  setBusy(true, true);
  status_->setText(QStringLiteral("正在解压…"));
  auto *watcher = new QFutureWatcher<ExtractResult>(this);
  connect(watcher, &QFutureWatcher<ExtractResult>::finished, this, [this, watcher, dest, secret, session] {
    const ExtractResult result = watcher->result();
    watcher->deleteLater();
    if (result.code == ARCHIVER_OK) session->secret = secret;
    setBusy(false);
    if (closeAfterFinish_) {
      close();
      return;
    }
    if (result.code == ARCHIVER_ERR_PASSWORD_REQUIRED || result.code == ARCHIVER_ERR_PASSWORD) {
      bool save = false;
      auto nextSecret = askPassword(result.code == ARCHIVER_ERR_PASSWORD, &save);
      if (nextSecret) {
        if (save) {
          QVector<QByteArray> book = PasswordBook::load();
          book.append(nextSecret->bytes);
          PasswordBook::store(book);
          wipeList(book);
        }
        startExtract(dest, true, std::move(nextSecret));
      } else {
        status_->setText(QStringLiteral("已取消密码输入，可以重新开始解压。"));
      }
      return;
    }
    if (result.code == ARCHIVER_ERR_CANCELLED)
      status_->setText(QStringLiteral("已取消解压。已完成的文件和文件夹可能保留。"));
    else if (result.code != ARCHIVER_OK)
      showError(result.code, result.detail);
    else
      status_->setText(QStringLiteral("解压完成。文件已保存到 %1").arg(QDir::toNativeSeparators(dest)));
  });
  watcher->setFuture(QtConcurrent::run([session, secret, state, dest, path, nested] {
    ExtractResult result;
    QVector<QByteArray> book = PasswordBook::load();
    if (secret && !secret->bytes.isEmpty()) {
      bool found = false;
      for (const QByteArray &item : book)
        if (item == secret->bytes) found = true;
      if (!found) book.prepend(secret->bytes);
    }
    QVector<const char *> views;
    for (const QByteArray &item : book) views.append(item.constData());
    try {
      if (!QDir().mkpath(dest)) {
        result.code = ARCHIVER_ERR_IO;
        wipeList(book);
        return result;
      }
      const QByteArray destBytes = dest.toUtf8();
      if (nested) {
        char message[1024];
        message[0] = 0;
        const QByteArray pathBytes = path.toUtf8();
        result.code = archiver_extract_nested(
            pathBytes.constData(), nullptr, destBytes.constData(),
            views.isEmpty() ? nullptr : views.data(), static_cast<uint32_t>(views.size()), nullptr,
            reportProgress, state.get(), message, sizeof(message));
        result.detail = QString::fromUtf8(message);
      } else {
        result.code = archiver_extract_with_progress(
            session->handle, destBytes.constData(),
            secret ? secret->bytes.constData() : nullptr, reportProgress, state.get());
        result.detail = QString::fromUtf8(archiver_message(session->handle));
      }
    } catch (...) {
      result.code = ARCHIVER_ERR_IO;
    }
    wipeList(book);
    return result;
  }));
}

void MainWindow::refreshProgress() {
  if (!busy_ || !extracting_ || !work_ || work_->cancel.load()) return;
  const uint64_t total = work_->total.load();
  const uint64_t completed = work_->completed.load();
  if (total) {
    progress_->setRange(0, 1000);
    progress_->setValue(static_cast<int>(std::min<uint64_t>(1000, completed * 1000 / total)));
    status_->setText(QStringLiteral("正在解压… %1 / %2")
                         .arg(QLocale().formattedDataSize(completed), QLocale().formattedDataSize(total)));
  }
}

void MainWindow::cancelExtraction() {
  if (!busy_ || !extracting_ || !work_) return;
  work_->cancel.store(true);
  cancelButton_->setEnabled(false);
  status_->setText(QStringLiteral("正在取消，清理当前未完成的文件…"));
}

void MainWindow::showError(int result, const QString &detail) {
  const QString text = errorText(result, detail);
  status_->setText(text);
  QMessageBox::warning(this, QStringLiteral("本地归档"), text);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
  const auto urls = event->mimeData()->urls();
  if (!busy_ && urls.size() == 1 && urls.first().isLocalFile()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event) {
  const auto urls = event->mimeData()->urls();
  if (!busy_ && urls.size() == 1 && urls.first().isLocalFile()) {
    event->acceptProposedAction();
    openPath(urls.first().toLocalFile());
  }
}

void MainWindow::closeEvent(QCloseEvent *event) {
  if (busy_) {
    closeAfterFinish_ = true;
    if (work_) work_->cancel.store(true);
    status_->setText(QStringLiteral("正在结束当前任务，完成清理后关闭窗口…"));
    event->ignore();
  } else {
    event->accept();
  }
}
