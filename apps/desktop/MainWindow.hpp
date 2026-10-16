#ifndef LOCAL_ARCHIVER_MAIN_WINDOW_HPP
#define LOCAL_ARCHIVER_MAIN_WINDOW_HPP
#include <QMainWindow>
#include <QString>
#include <memory>
class ArchiveModel;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableView;
class QTimer;
struct ArchiveSession;
struct DesktopSecret;
struct DesktopWorkState;
class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow() override;
  void openPath(const QString &path);
  bool isBusy() const { return busy_; }
 protected:
  void dragEnterEvent(QDragEnterEvent *event) override;
  void dropEvent(QDropEvent *event) override;
  void closeEvent(QCloseEvent *event) override;
 private:
  void chooseArchive();
  void chooseDestination();
  void startOpen(const QString &path, std::shared_ptr<DesktopSecret> secret = {});
  void startExtract(const QString &dest, std::shared_ptr<DesktopSecret> secret = {});
  void setBusy(bool busy, bool extracting = false);
  void refreshProgress();
  void cancelExtraction();
  std::shared_ptr<DesktopSecret> askPassword(bool rejected);
  void showError(int result, const QString &detail);
  ArchiveModel *model_ = nullptr;
  QTableView *table_ = nullptr;
  QLabel *archiveLabel_ = nullptr;
  QLabel *summary_ = nullptr;
  QLabel *status_ = nullptr;
  QLineEdit *destination_ = nullptr;
  QProgressBar *progress_ = nullptr;
  QPushButton *openButton_ = nullptr;
  QPushButton *extractButton_ = nullptr;
  QPushButton *chooseButton_ = nullptr;
  QPushButton *cancelButton_ = nullptr;
  QPushButton *outputButton_ = nullptr;
  QTimer *timer_ = nullptr;
  std::shared_ptr<ArchiveSession> archive_;
  std::shared_ptr<DesktopWorkState> work_;
  QString archivePath_;
  QString lastOutput_;
  bool busy_ = false;
  bool extracting_ = false;
  bool closeAfterFinish_ = false;
};
#endif
