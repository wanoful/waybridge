// A real Qt SNI + DBusMenu exporter. Every action has an observable application effect.
#include <QApplication>
#include <QAction>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QTimer>
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  app.setDesktopFileName("org.waybridge.TrayAction");
  app.setQuitOnLastWindowClosed(false);
  if (argc < 2)
    return 2;
  QFile log(argv[1]);
  if (!log.open(QIODevice::WriteOnly | QIODevice::Append))
    return 3;
  QMainWindow window;
  window.setWindowTitle("Waybridge tray action fixture");
  window.resize(500, 300);
  auto record = [&](QString event, int count = 0) {
    log.write(QJsonDocument(
                  QJsonObject{{"event", event}, {"visible", window.isVisible()}, {"count", count}})
                  .toJson(QJsonDocument::Compact) +
              "\n");
    log.flush();
  };
  QMenu menu;
  int count = 0;
  QAction *restore = menu.addAction("&Restore");
  QObject::connect(restore, &QAction::triggered, [&] {
    window.show();
    record("restore");
  });
  QAction *increment = menu.addAction("&Increment");
  QObject::connect(increment, &QAction::triggered, [&] {
    ++count;
    window.setWindowTitle(QString("Waybridge action %1").arg(count));
    record("increment", count);
  });
  QAction *quit = menu.addAction("&Quit");
  QObject::connect(quit, &QAction::triggered, [&] {
    record("quit");
    app.quit();
  });
  QPixmap pixmap(32, 32);
  pixmap.fill(Qt::cyan);
  QSystemTrayIcon tray{QIcon(pixmap)};
  tray.setToolTip("Waybridge isolated action fixture");
  tray.setContextMenu(&menu);
  QObject::connect(&tray, &QSystemTrayIcon::activated,
                   [&](QSystemTrayIcon::ActivationReason reason) {
                     if (reason == QSystemTrayIcon::Trigger) {
                       window.setVisible(!window.isVisible());
                       record("activate");
                     }
                   });
  tray.show();
  record("ready");
  QTimer::singleShot(120000, &app, &QApplication::quit);
  return app.exec();
}
