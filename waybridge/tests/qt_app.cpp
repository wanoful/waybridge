// Real Qt Widgets coverage: DPI, keyboard focus, popup grabs, dialogs and alpha.
#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QInputMethodEvent>
#include <QKeySequence>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QShortcut>
#include <QTimer>
#include <QWindow>
#include <cstdio>

class ImeEdit : public QLineEdit {
public:
  using QLineEdit::QLineEdit;
  void inputMethodEvent(QInputMethodEvent *event) override {
    std::printf("PREEDIT %s\n", event->preeditString().toUtf8().constData());
    QLineEdit::inputMethodEvent(event);
  }
};

class AlphaWindow : public QWidget {
public:
  AlphaWindow() {
    setWindowTitle("Waybridge alpha fixture");
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    resize(360, 240);
  }
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), Qt::transparent);
    p.fillRect(0, 0, width() / 3, height(), QColor(255, 0, 0, 255));
    p.fillRect(width() / 3, 0, width() / 3, height(), QColor(0, 255, 0, 128));
  }
};

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  setvbuf(stdout, nullptr, _IOLBF, 0);
  QWidget window;
  window.setWindowTitle("Waybridge Qt fixture");
  window.resize(960, 640);
  ImeEdit edit(&window);
  edit.setGeometry(20, 20, 300, 40);
  QObject::connect(&edit, &QLineEdit::textChanged, [](const QString &text) {
    std::printf("TEXT %s\n", text.toUtf8().constData());
  });
  auto spaceShortcut = new QShortcut(QKeySequence("Ctrl+Space"), &edit);
  QObject::connect(spaceShortcut, &QShortcut::activated, [] { std::puts("CTRL_SPACE"); });
  QPushButton button("Open menu", &window);
  button.setGeometry(20, 80, 160, 40);
  button.setShortcut(QKeySequence("Ctrl+M"));
  QMenu menu(&window);
  auto action = menu.addAction("Choose item");
  QObject::connect(action, &QAction::triggered, [] { std::puts("ACTION"); });
  QObject::connect(&button, &QPushButton::clicked, [&] {
    menu.popup(button.mapToGlobal(QPoint(0, button.height())));
    QTimer::singleShot(50, [&] {
      std::printf("MENU %d %d %d %d\n", menu.x(), menu.y(), menu.width(), menu.height());
    });
  });
  QPushButton dialogButton("Open dialog", &window);
  dialogButton.setGeometry(200, 80, 160, 40);
  QObject::connect(&dialogButton, &QPushButton::clicked, [&] {
    auto dialog = new QDialog(&window);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Waybridge Qt dialog");
    dialog->resize(400, 240);
    auto input = new QLineEdit(dialog);
    input->setGeometry(20, 20, 300, 40);
    QObject::connect(input, &QLineEdit::textChanged, [](const QString &text) {
      std::printf("DIALOG_TEXT %s\n", text.toUtf8().constData());
    });
    dialog->setWindowModality(Qt::WindowModal);
    dialog->show();
  });
  QPushButton overflowButton("Overflow menu", &window);
  overflowButton.setGeometry(400, 80, 160, 40);
  overflowButton.setShortcut(QKeySequence("Ctrl+O"));
  QMenu overflow(&window);
  auto overflowAction = overflow.addAction("Overflow action with a deliberately wide label");
  QObject::connect(overflowAction, &QAction::triggered, [] { std::puts("OVERFLOW_ACTION"); });
  auto nested = overflow.addMenu("Nested menu");
  auto nestedAction = nested->addAction("Nested action");
  QObject::connect(nestedAction, &QAction::triggered, [] { std::puts("NESTED_ACTION"); });
  QObject::connect(&overflowButton, &QPushButton::clicked, [&] {
    overflow.popup(window.mapToGlobal(QPoint(window.width() - 30, window.height() - 10)));
    QTimer::singleShot(50, [&] {
      std::printf("OVERFLOW %d %d %d %d\n", overflow.x(), overflow.y(), overflow.width(),
                  overflow.height());
    });
  });
  window.show();
  AlphaWindow alpha;
  alpha.show();
  QTimer timer;
  double previous = -1;
  QObject::connect(&timer, &QTimer::timeout, [&] {
    double scale = window.devicePixelRatioF();
    if (scale != previous) {
      previous = scale;
      std::printf("DPR %.2f\n", scale);
    }
  });
  timer.start(50);
  return app.exec();
}
