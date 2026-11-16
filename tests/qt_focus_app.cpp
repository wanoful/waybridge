// Isolated test application: distinct process titles identify each GUI launch.
#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QShortcut>
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  QApplication::setDesktopFileName("org.waybridge.FocusProbe");
  const auto pid = QString::number(QCoreApplication::applicationPid());
  QWidget main;
  main.setWindowTitle("Waybridge GUI focus " + pid);
  main.resize(640, 420);
  auto layout = new QVBoxLayout(&main);
  layout->addWidget(new QLabel("Disposable Waybridge focus fixture"));
  layout->addWidget(new QLineEdit);
  auto button = new QPushButton("Open owned modal (Ctrl+D)");
  layout->addWidget(button);
  auto open = [&] {
    auto dialog = new QDialog(&main);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Waybridge GUI modal " + pid);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->resize(360, 180);
    auto contents = new QVBoxLayout(dialog);
    contents->addWidget(new QLabel("Escape closes this owned modal"));
    contents->addWidget(new QLineEdit);
    dialog->show();
  };
  QObject::connect(button, &QPushButton::clicked, open);
  auto shortcut = new QShortcut(QKeySequence("Ctrl+D"), &main);
  QObject::connect(shortcut, &QShortcut::activated, open);
  main.show();
  return app.exec();
}
