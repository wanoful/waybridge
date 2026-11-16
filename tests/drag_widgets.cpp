// Generic Qt drag sources/targets: no application-name compatibility rules.
#include <QApplication>
#include <QFileSystemModel>
#include <QLineEdit>
#include <QTreeView>
#include <QTimer>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QLabel>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <cstdio>

class DragText : public QLabel {
public:
  using QLabel::QLabel;
  void mouseMoveEvent(QMouseEvent *event) override {
    if (!(event->buttons() & Qt::LeftButton)) return;
    QDrag drag(this);auto data=new QMimeData;data->setText("Waybridge drag text");drag.setMimeData(data);
    QPixmap image(130,28);image.fill(Qt::red);drag.setPixmap(image);drag.setHotSpot(QPoint(7,9));
    puts("TEXT_DRAG_STARTED");auto result=drag.exec(Qt::CopyAction|Qt::MoveAction);printf("TEXT_DRAG_ENDED %d\n",int(result));
  }
};
class DropEdit : public QLineEdit {
public:
  using QLineEdit::QLineEdit;
  void dragEnterEvent(QDragEnterEvent *event) override {printf("ENTER %s\n",event->mimeData()->formats().join(",").toUtf8().constData());QLineEdit::dragEnterEvent(event);}
  void dropEvent(QDropEvent *event) override {puts("DROP");QLineEdit::dropEvent(event);}
};
class DragTree : public QTreeView {
protected:
  void startDrag(Qt::DropActions actions) override {printf("FILE_DRAG_STARTED %d\n",int(actions));QTreeView::startDrag(actions);puts("FILE_DRAG_ENDED");}
  void mousePressEvent(QMouseEvent *e) override {printf("TREE_PRESS %d %d %s\n",int(e->position().x()),int(e->position().y()),indexAt(e->position().toPoint()).data().toString().toUtf8().constData());QTreeView::mousePressEvent(e);}
  void dropEvent(QDropEvent *e) override {puts("FILE_DROP");QTreeView::dropEvent(e);printf("FILE_DROP_ACTION %d %d\n",int(e->dropAction()),e->isAccepted());}
};
int main(int argc,char **argv) {
  QApplication app(argc,argv);
  setvbuf(stdout,nullptr,_IOLBF,0);
  QWidget first,second;
  first.setWindowTitle("Waybridge drag source"); first.resize(960,640);
  second.setWindowTitle("Waybridge drag target"); second.resize(960,640);
  DragText source("Waybridge drag text",&first);DropEdit same(&first),other(&second);
  source.setGeometry(20,20,320,50);
  same.setGeometry(20,100,320,50);other.setGeometry(20,100,320,50);
  QObject::connect(&same,&QLineEdit::textChanged,[](const QString &s){printf("SAME %s\n",s.toUtf8().constData());});
  QObject::connect(&other,&QLineEdit::textChanged,[](const QString &s){printf("OTHER %s\n",s.toUtf8().constData());});
  QFileSystemModel model;
  DragTree tree;tree.setParent(&first);
  if(argc>1) {
    model.setReadOnly(false);model.setRootPath(argv[1]);tree.setModel(&model);tree.setRootIndex(model.index(argv[1]));
    tree.setGeometry(380,20,540,500);tree.setDragEnabled(true);tree.setAcceptDrops(true);tree.setDropIndicatorShown(true);
    tree.setDragDropMode(QAbstractItemView::DragDrop);tree.setDefaultDropAction(Qt::MoveAction);tree.setColumnWidth(0,300);
    QObject::connect(&model,&QFileSystemModel::directoryLoaded,[&](const QString &){
      QTimer::singleShot(100,[&]{for(const char *name:{"source.txt","folder"}) {
        auto index=model.index(QString(argv[1])+"/"+name);auto r=tree.visualRect(index);auto p=tree.viewport()->mapTo(&first,r.center());
        printf("ITEM %s %d %d\n",name,p.x(),p.y());
      }});
    });
  }
  first.show();second.show();return app.exec();
}
