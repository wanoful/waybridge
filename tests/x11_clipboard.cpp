#include <QGuiApplication>
#include <QClipboard>
#include <QFile>
#include <QMimeData>
#include <QTimer>
#include <cstdio>
int main(int argc,char**argv) {
  QGuiApplication app(argc,argv);
  if(argc!=4)return 2;
  auto clipboard=app.clipboard();
  if(QString(argv[1])=="copy") {
    QFile input(argv[3]);if(!input.open(QIODevice::ReadOnly))return 3;
    auto mime=new QMimeData;auto bytes=input.readAll();
    if(QString(argv[2]).startsWith("text/plain"))mime->setText(QString::fromUtf8(bytes));else mime->setData(argv[2],bytes);
    clipboard->setMimeData(mime);
    std::puts("READY");std::fflush(stdout);return app.exec();
  }
  QTimer::singleShot(100,[&]{
    auto bytes=QString(argv[2]).startsWith("text/plain")?clipboard->text().toUtf8():clipboard->mimeData()->data(argv[2]);QFile out(argv[3]);
    if(!out.open(QIODevice::WriteOnly)||out.write(bytes)!=bytes.size())app.exit(4);else app.quit();
  });
  return app.exec();
}
