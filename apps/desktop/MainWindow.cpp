#include "MainWindow.hpp"
#include "archiver.h"
#include <QAbstractTableModel>
#include <QAction>
#include <QCloseEvent>
#include <QDesktopServices>
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
#include <QLocale>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
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
  QString path;
  uint64_t size;
  bool directory;
  bool symlink;
};
class ArchiveModel final : public QAbstractTableModel {
 public:
  explicit ArchiveModel(QObject *parent) : QAbstractTableModel(parent) {}
  int rowCount(const QModelIndex &parent = {}) const override {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
  }
  int columnCount(const QModelIndex &parent = {}) const override {
    return parent.isValid() ? 0 : 3;
  }
  QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
      if (section == 0) return QStringLiteral("文件路径");
      if (section == 1) return QStringLiteral("大小");
      if (section == 2) return QStringLiteral("类型");
    }
    return QAbstractTableModel::headerData(section, orientation, role);
  }
  QVariant data(const QModelIndex &index, int role) const override {
    if (!index.isValid() || index.row() >= rows_.size()) return {};
    const auto &row = rows_[index.row()];
    if (role == Qt::TextAlignmentRole && index.column() == 1)
      return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::ToolTipRole) return row.path;
    if (role != Qt::DisplayRole) return {};
    if (index.column() == 0) return row.path;
    if (index.column() == 1)
      return row.directory ? QStringLiteral("—") : QLocale().formattedDataSize(row.size);
    return row.directory ? QStringLiteral("文件夹") :
           row.symlink ? QStringLiteral("链接") : QStringLiteral("文件");
  }
  void setRows(QVector<ArchiveRow> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
  }
 private:
  QVector<ArchiveRow> rows_;
};
namespace {
struct OpenResult {
  ArchiverResult code = ARCHIVER_OK;
  QString detail;
  std::shared_ptr<ArchiveSession> session;
  QVector<ArchiveRow> rows;
  uint64_t total = 0;
};
struct ExtractResult {
  ArchiverResult code = ARCHIVER_OK;
  QString detail;
};
QString suggestedDestination(const QString &path) {
  const QFileInfo info(path);
  QString name = info.fileName();
  const QStringList suffixes = {".tar.bz2", ".tar.zst", ".tar.gz", ".tar.xz",
      ".tbz2", ".tgz", ".tbz", ".txz", ".tzst", ".zip", ".7z", ".tar", ".rar"};
  for (const auto &suffix : suffixes) {
    if (name.endsWith(suffix, Qt::CaseInsensitive)) { name.chop(suffix.size()); break; }
  }
  if (name.isEmpty()) name = QStringLiteral("解压文件");
  return info.dir().filePath(name);
}
int reportProgress(uint64_t completed, uint64_t total, void *user) {
  auto *state = static_cast<DesktopWorkState *>(user);
  state->completed.store(completed, std::memory_order_relaxed);
  state->total.store(total, std::memory_order_relaxed);
  return state->cancel.load(std::memory_order_relaxed) ? 1 : 0;
}
QString errorText(int code, const QString &detail) {
  switch (code) {
    case ARCHIVER_ERR_PATH:
      return QStringLiteral("已停止：压缩包或输出路径包含不安全的路径或链接。");
    case ARCHIVER_ERR_UNSUPPORTED:
      if (detail.contains("more data"))
        return QStringLiteral("压缩包展开后的大小超过本版本的 64 GiB 上限。");
      if (detail.contains("too many entries"))
        return QStringLiteral("压缩包包含超过 100 万个文件条目，暂时无法打开。");
      return detail.contains("symlink")
        ? QStringLiteral("这个压缩包包含当前 Windows 版不支持创建的符号链接。")
        : QStringLiteral("暂不支持这个文件。请选择 ZIP、7z、tar 或 RAR；分卷压缩包暂不支持。");
    case ARCHIVER_ERR_ENGINE:
      return QStringLiteral("无法加载解压引擎。请完整解压程序包，保留程序旁的所有文件后重试。");
    case ARCHIVER_ERR_IO:
      return detail.contains("already exists")
        ? QStringLiteral("输出位置已有同名文件，原文件已保留。请选择一个空文件夹后重试。")
        : QStringLiteral("无法读取或写入文件。请检查文件是否可读、输出位置是否可写，以及磁盘空间。");
    case ARCHIVER_ERR_FORMAT:
      return QStringLiteral("无法完整读取这个压缩包。文件可能损坏、不完整，或与扩展名不符。");
    default: return QStringLiteral("操作失败，请重新选择文件后再试。");
  }
}
}
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("本地归档"));
  resize(900, 620);
  setMinimumSize(640, 440);
  setAcceptDrops(true);
  auto *central = new QWidget(this);
  auto *layout = new QVBoxLayout(central);
  layout->setContentsMargins(24, 20, 24, 20);
  layout->setSpacing(12);
  auto *title = new QLabel(QStringLiteral("本地归档"), central);
  QFont heading = title->font();
  heading.setPointSize(20);
  heading.setBold(true);
  title->setFont(heading);
  layout->addWidget(title);
  layout->addWidget(new QLabel(QStringLiteral("查看与解压本机文件，无需联网。"), central));
  auto *buttons = new QHBoxLayout();
  openButton_ = new QPushButton(QStringLiteral("打开压缩包…"), central);
  openButton_->setObjectName("openButton");
  openButton_->setToolTip(QStringLiteral("打开压缩包（Ctrl+O），也可以把文件拖到窗口中。"));
  outputButton_ = new QPushButton(QStringLiteral("打开输出文件夹"), central);
  outputButton_->setObjectName("outputButton");
  buttons->addWidget(openButton_);
  buttons->addStretch();
  buttons->addWidget(outputButton_);
  layout->addLayout(buttons);
  archiveLabel_ = new QLabel(QStringLiteral("拖入压缩包，或点击“打开压缩包…”"), central);
  archiveLabel_->setTextFormat(Qt::PlainText);
  archiveLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  archiveLabel_->setWordWrap(true);
  layout->addWidget(archiveLabel_);
  model_ = new ArchiveModel(this);
  table_ = new QTableView(central);
  table_->setObjectName("archiveTable");
  table_->setModel(model_);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setAlternatingRowColors(true);
  table_->verticalHeader()->hide();
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table_->setColumnWidth(1, 110);
  table_->setColumnWidth(2, 80);
  layout->addWidget(table_, 1);
  summary_ = new QLabel(QStringLiteral("支持 ZIP、7z、tar 及其压缩格式，RAR 仅解压。"), central);
  layout->addWidget(summary_);
  auto *destRow = new QHBoxLayout();
  destRow->addWidget(new QLabel(QStringLiteral("解压到"), central));
  destination_ = new QLineEdit(central);
  destination_->setObjectName("destinationEdit");
  destination_->setPlaceholderText(QStringLiteral("打开压缩包后，会自动建议一个文件夹"));
  chooseButton_ = new QPushButton(QStringLiteral("选择…"), central);
  chooseButton_->setObjectName("chooseDestinationButton");
  destRow->addWidget(destination_, 1);
  destRow->addWidget(chooseButton_);
  layout->addLayout(destRow);
  auto *actionRow = new QHBoxLayout();
  extractButton_ = new QPushButton(QStringLiteral("开始解压"), central);
  extractButton_->setObjectName("extractButton");
  cancelButton_ = new QPushButton(QStringLiteral("取消解压"), central);
  cancelButton_->setObjectName("cancelButton");
  progress_ = new QProgressBar(central);
  progress_->setObjectName("progressBar");
  actionRow->addWidget(extractButton_);
  actionRow->addWidget(cancelButton_);
  actionRow->addWidget(progress_, 1);
  layout->addLayout(actionRow);
  status_ = new QLabel(QStringLiteral("已有文件会保留；密码只在本次打开期间使用，不写入文件。"), central);
  status_->setObjectName("statusLabel");
  status_->setTextFormat(Qt::PlainText);
  status_->setWordWrap(true);
  layout->addWidget(status_);
  setCentralWidget(central);
  auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件"));
  auto *openAction = fileMenu->addAction(QStringLiteral("打开压缩包…"));
  openAction->setShortcut(QKeySequence::Open);
  connect(openAction, &QAction::triggered, this, &MainWindow::chooseArchive);
  fileMenu->addSeparator();
  connect(fileMenu->addAction(QStringLiteral("退出")), &QAction::triggered, this, &QWidget::close);
  auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助"));
  connect(helpMenu->addAction(QStringLiteral("关于本地归档")), &QAction::triggered, this, [this] {
    QMessageBox::about(this, QStringLiteral("关于本地归档"),
      QStringLiteral("本地归档 %1\n\n查看和解压本机压缩包，不联网、不保存密码。\n"
                     "程序与第三方组件的许可文件在程序目录中。").arg(ARCHIVER_VERSION));
  });
  connect(openButton_, &QPushButton::clicked, this, &MainWindow::chooseArchive);
  connect(chooseButton_, &QPushButton::clicked, this, &MainWindow::chooseDestination);
  connect(extractButton_, &QPushButton::clicked, this, [this] {
    startExtract(QDir::fromNativeSeparators(destination_->text().trimmed()));
  });
  connect(cancelButton_, &QPushButton::clicked, this, &MainWindow::cancelExtraction);
  connect(outputButton_, &QPushButton::clicked, this, [this] {
    if (!lastOutput_.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(lastOutput_));
  });
  timer_ = new QTimer(this);
  timer_->setInterval(100);
  connect(timer_, &QTimer::timeout, this, &MainWindow::refreshProgress);
  setBusy(false);
}
MainWindow::~MainWindow() = default;
void MainWindow::setBusy(bool busy, bool extracting) {
  busy_ = busy;
  extracting_ = extracting;
  openButton_->setEnabled(!busy);
  extractButton_->setEnabled(!busy && archive_ != nullptr);
  chooseButton_->setEnabled(!busy);
  destination_->setEnabled(!busy);
  outputButton_->setEnabled(!busy && !lastOutput_.isEmpty());
  cancelButton_->setVisible(busy && extracting);
  cancelButton_->setEnabled(busy && extracting);
  cancelButton_->setText(QStringLiteral("取消解压"));
  progress_->setVisible(busy);
  if (busy) { progress_->setRange(0, 0); timer_->start(); }
  else timer_->stop();
}
void MainWindow::chooseArchive() {
  if (busy_) return;
  const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("打开压缩包"), {},
    QStringLiteral("压缩包 (*.zip *.7z *.tar *.tgz *.tar.gz *.tbz *.tbz2 *.tar.bz2 "
                   "*.txz *.tar.xz *.tzst *.tar.zst *.rar);;所有文件 (*)"));
  if (!path.isEmpty()) openPath(path);
}
void MainWindow::chooseDestination() {
  if (busy_) return;
  const QString path = QFileDialog::getExistingDirectory(
    this, QStringLiteral("选择输出文件夹"), destination_->text());
  if (!path.isEmpty()) destination_->setText(QDir::toNativeSeparators(path));
}
void MainWindow::openPath(const QString &path) {
  if (busy_) return;
  if (!QFileInfo(path).isFile()) { showError(ARCHIVER_ERR_IO, {}); return; }
  archive_.reset();
  model_->setRows({});
  lastOutput_.clear();
  archivePath_ = QFileInfo(path).absoluteFilePath();
  archiveLabel_->setText(QDir::toNativeSeparators(archivePath_));
  archiveLabel_->setToolTip(archiveLabel_->text());
  setWindowTitle(QFileInfo(path).fileName() + QStringLiteral(" — 本地归档"));
  destination_->setText(QDir::toNativeSeparators(suggestedDestination(archivePath_)));
  summary_->setText(QStringLiteral("正在读取文件列表…"));
  startOpen(archivePath_);
}
std::shared_ptr<DesktopSecret> MainWindow::askPassword(bool rejected) {
  bool accepted = false;
  QString password = QInputDialog::getText(this, QStringLiteral("压缩包密码"),
    rejected ? QStringLiteral("密码不正确，请重试。关闭压缩包后会清除本次密码。")
             : QStringLiteral("请输入密码。仅在本次打开期间使用，不会保存到文件。"),
    QLineEdit::Password, {}, &accepted);
  if (!accepted) { password.fill(QChar(0)); return {}; }
  auto secret = std::make_shared<DesktopSecret>();
  secret->bytes = password.toUtf8();
  password.fill(QChar(0));
  return secret;
}
void MainWindow::startOpen(const QString &path, std::shared_ptr<DesktopSecret> secret) {
  work_ = std::make_shared<DesktopWorkState>();
  auto state = work_;
  setBusy(true);
  status_->setText(QStringLiteral("正在打开压缩包…"));
  auto *watcher = new QFutureWatcher<OpenResult>(this);
  connect(watcher, &QFutureWatcher<OpenResult>::finished, this, [this, watcher, path] {
    OpenResult result = watcher->result();
    watcher->deleteLater();
    if (result.code == ARCHIVER_OK) archive_ = result.session;
    setBusy(false);
    if (closeAfterFinish_) { close(); return; }
    if (result.code == ARCHIVER_ERR_PASSWORD_REQUIRED || result.code == ARCHIVER_ERR_PASSWORD) {
      auto nextSecret = askPassword(result.code == ARCHIVER_ERR_PASSWORD);
      if (nextSecret) startOpen(path, std::move(nextSecret));
      else {
        summary_->setText(QStringLiteral("尚未打开压缩包"));
        status_->setText(QStringLiteral("已取消密码输入。"));
      }
      return;
    }
    if (result.code != ARCHIVER_OK) {
      summary_->setText(QStringLiteral("尚未打开压缩包"));
      showError(result.code, result.detail);
      return;
    }
    const qsizetype count = result.rows.size();
    model_->setRows(std::move(result.rows));
    summary_->setText(QStringLiteral("共 %1 项 · 文件总大小 %2")
      .arg(QLocale().toString(static_cast<qlonglong>(count)))
      .arg(QLocale().formattedDataSize(result.total)));
    status_->setText(QStringLiteral("已打开。确认输出文件夹后，点击“开始解压”。"));
  });
  watcher->setFuture(QtConcurrent::run([path, secret, state] {
    OpenResult result;
    result.session = std::make_shared<ArchiveSession>();
    result.session->secret = secret;
    try {
      const QByteArray pathBytes = path.toUtf8();
      result.code = archiver_open(pathBytes.constData(), nullptr,
        secret ? secret->bytes.constData() : nullptr, &result.session->handle);
      result.detail = QString::fromUtf8(archiver_message(result.session->handle));
      if (result.code == ARCHIVER_OK) {
        uint32_t count = 0;
        result.code = archiver_entry_count(result.session->handle, &count);
        result.rows.reserve(count);
        for (uint32_t i = 0; result.code == ARCHIVER_OK && i < count; ++i) {
          if (state->cancel.load()) { result.code = ARCHIVER_ERR_CANCELLED; break; }
          ArchiverEntry entry = {};
          result.code = archiver_entry_at(result.session->handle, i, &entry);
          if (result.code != ARCHIVER_OK) break;
          ArchiveRow row{QString::fromUtf8(entry.path), entry.size,
                         entry.is_directory != 0, entry.is_symlink != 0};
          archiver_entry_free(&entry);
          if (!row.directory) result.total += row.size;
          result.rows.push_back(std::move(row));
        }
      }
    } catch (const std::exception &) { result.code = ARCHIVER_ERR_IO; }
    catch (...) { result.code = ARCHIVER_ERR_FORMAT; }
    return result;
  }));
}
void MainWindow::startExtract(const QString &dest, std::shared_ptr<DesktopSecret> secret) {
  if (busy_ || !archive_) return;
  if (dest.isEmpty() || !QDir::isAbsolutePath(dest)) {
    status_->setText(QStringLiteral("请选择一个完整的输出文件夹路径。"));
    return;
  }
  auto session = archive_;
  if (!secret) secret = session->secret;
  work_ = std::make_shared<DesktopWorkState>();
  auto state = work_;
  setBusy(true, true);
  status_->setText(QStringLiteral("正在解压…"));
  auto *watcher = new QFutureWatcher<ExtractResult>(this);
  connect(watcher, &QFutureWatcher<ExtractResult>::finished, this,
    [this, watcher, dest, secret, session] {
      const ExtractResult result = watcher->result();
      watcher->deleteLater();
      if (QDir(dest).exists()) lastOutput_ = dest;
      if (result.code == ARCHIVER_OK) session->secret = secret;
      setBusy(false);
      if (closeAfterFinish_) { close(); return; }
      if (result.code == ARCHIVER_ERR_PASSWORD_REQUIRED || result.code == ARCHIVER_ERR_PASSWORD) {
        auto nextSecret = askPassword(result.code == ARCHIVER_ERR_PASSWORD);
        if (nextSecret) startExtract(dest, std::move(nextSecret));
        else status_->setText(QStringLiteral("已取消密码输入，可以重新开始解压。"));
        return;
      }
      if (result.code == ARCHIVER_ERR_CANCELLED)
        status_->setText(QStringLiteral("已取消解压。已完成的文件和文件夹可能保留。"));
      else if (result.code != ARCHIVER_OK) showError(result.code, result.detail);
      else status_->setText(QStringLiteral("解压完成。文件已保存到 %1")
                            .arg(QDir::toNativeSeparators(dest)));
  });
  watcher->setFuture(QtConcurrent::run([session, secret, state, dest] {
    ExtractResult result;
    try {
      if (!QDir().mkpath(dest)) { result.code = ARCHIVER_ERR_IO; return result; }
      const QByteArray bytes = dest.toUtf8();
      result.code = archiver_extract_with_progress(session->handle, bytes.constData(),
        secret ? secret->bytes.constData() : nullptr, reportProgress, state.get());
      result.detail = QString::fromUtf8(archiver_message(session->handle));
    } catch (...) { result.code = ARCHIVER_ERR_IO; }
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
  } else event->accept();
}
