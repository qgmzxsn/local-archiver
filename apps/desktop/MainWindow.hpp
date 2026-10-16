#ifndef LOCAL_ARCHIVER_MAIN_WINDOW_HPP
#define LOCAL_ARCHIVER_MAIN_WINDOW_HPP

#include "archiver.h"

#include <QMainWindow>
#include <QString>

class QLabel;
class QTableWidget;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow() override;

 private slots:
  void openArchive();
  void extractArchive();

 private:
  bool askPassword(QString *password);
  void showResult(ArchiverResult result);
  void fillTable();

  QTableWidget *table_ = nullptr;
  QLabel *status_ = nullptr;
  ArchiverArchive *archive_ = nullptr;
  QString archive_path_;
};

#endif
