#include <QApplication>
#include <QWidget>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QFile>
#include <QJsonObject>
#include <QJsonDocument>
#include <QTimer>
#include <QUrl>
#include <cstdio>
class Target : public QWidget {
  QFile output;
  QString copy;
  void record(QJsonObject value) {
    output.write(QJsonDocument(value).toJson(QJsonDocument::Compact) + "\n");
    output.flush();
  }

public:
  Target(QString path, QString destination) : output(path), copy(destination) {
    output.open(QIODevice::WriteOnly | QIODevice::Append);
    setAcceptDrops(true);
    resize(500, 300);
    move(120, 90);
    setWindowTitle("Waybridge XDND Qt target");
  }
  void dragEnterEvent(QDragEnterEvent *event) override {
    record({{"event", "enter"},
            {"text", event->mimeData()->hasText()},
            {"uri", event->mimeData()->hasUrls()}});
    if (event->proposedAction() == Qt::CopyAction &&
        (event->mimeData()->hasText() || event->mimeData()->hasUrls()))
      event->acceptProposedAction();
  }
  void dragMoveEvent(QDragMoveEvent *event) override { event->acceptProposedAction(); }
  void dropEvent(QDropEvent *event) override {
    QByteArray data;
    bool copied = false;
    if (event->mimeData()->hasUrls()) {
      const auto urls = event->mimeData()->urls();
      if (!urls.isEmpty()) {
        QFile file(urls[0].toLocalFile());
        if (file.open(QIODevice::ReadOnly))
          data = file.readAll();
        copied = QFile::copy(urls[0].toLocalFile(), copy);
      }
    } else
      data = event->mimeData()->text().toUtf8();
    record({{"event", "drop"},
            {"data", QString::fromLatin1(data.toBase64())},
            {"copied", copied},
            {"x", event->position().x()},
            {"y", event->position().y()},
            {"action", int(event->proposedAction())}});
    event->acceptProposedAction();
  }
};
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  if (argc != 3)
    return 2;
  Target target(argv[1], argv[2]);
  target.show();
  std::printf("%llu\n", (unsigned long long)target.winId());
  std::fflush(stdout);
  QTimer::singleShot(120000, &app, &QApplication::quit);
  return app.exec();
}
