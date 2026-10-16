#include "MainWindow.hpp"

#include <QApplication>

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("local-archiver"));
  MainWindow window;
  window.show();
  return app.exec();
}
