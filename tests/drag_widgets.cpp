// Generic Qt drag sources/targets: no application-name compatibility rules.
#include <QApplication>
#ifdef WAYBRIDGE_DND_XCB_TRACE
#include <QAbstractNativeEventFilter>
#include <QtGui/qguiapplication_platform.h>
#include <xcb/xcb.h>
#endif
#include <QFileSystemModel>
#include <QFile>
#include <QCryptographicHash>
#include <QLineEdit>
#include <QTreeView>
#include <QTimer>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QLabel>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDragMoveEvent>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#ifdef WAYBRIDGE_DND_XCB_TRACE
class NativeTrace:public QAbstractNativeEventFilter {
  xcb_atom_t finished=0,copy=0,move=0;
public:
 NativeTrace(){auto x=qGuiApp->nativeInterface<QNativeInterface::QX11Application>();if(!x)return;auto c=x->connection();auto atom=[&](const char*name){auto r=xcb_intern_atom_reply(c,xcb_intern_atom(c,0,strlen(name),name),nullptr);auto a=r?r->atom:0;free(r);return a;};finished=atom("XdndFinished");copy=atom("XdndActionCopy");move=atom("XdndActionMove");}
 bool nativeEventFilter(const QByteArray&,void*message,qintptr*)override{auto e=static_cast<xcb_generic_event_t*>(message);if((e->response_type&127)==XCB_CLIENT_MESSAGE){auto m=reinterpret_cast<xcb_client_message_event_t*>(e);if(m->type==finished)printf("XFINISHED %u %u\n",m->data.data32[1]&1,m->data.data32[2]==copy?1:m->data.data32[2]==move?2:0);}return false;}
};
#endif
class DragText : public QLabel {
public:
  using QLabel::QLabel;
  void mouseMoveEvent(QMouseEvent *event) override {
    if (!(event->buttons() & Qt::LeftButton)) return;
    auto drag=new QDrag(this);auto data=new QMimeData;QFile file(qEnvironmentVariable("WAYBRIDGE_DND_FIXTURE_TEXT"));data->setText(file.open(QIODevice::ReadOnly)?QString::fromUtf8(file.readAll()):QStringLiteral("Waybridge drag text"));drag->setMimeData(data);
    QPixmap image(130,28);image.fill(Qt::red);drag->setPixmap(image);drag->setHotSpot(QPoint(7,9));
    puts("TEXT_DRAG_STARTED");auto result=drag->exec(Qt::CopyAction|Qt::MoveAction);printf("TEXT_DRAG_ENDED %d\n",int(result));
  }
};
class DropEdit : public QLineEdit {
public:
  using QLineEdit::QLineEdit;
  void dragEnterEvent(QDragEnterEvent *event) override {printf("ENTER %s\n",event->mimeData()->formats().join(",").toUtf8().constData());QLineEdit::dragEnterEvent(event);}
  void dropEvent(QDropEvent *event) override {puts("DROP");QLineEdit::dropEvent(event);printf("DROP_ACTION %d %d\n",int(event->dropAction()),event->isAccepted());}
};
class DragTree : public QTreeView {
protected:
  void startDrag(Qt::DropActions actions) override {printf("FILE_DRAG_STARTED %d\n",int(actions));QTreeView::startDrag(actions);puts("FILE_DRAG_ENDED");}
  void mousePressEvent(QMouseEvent *e) override {printf("TREE_PRESS %d %d %s\n",int(e->position().x()),int(e->position().y()),indexAt(e->position().toPoint()).data().toString().toUtf8().constData());QTreeView::mousePressEvent(e);}
  // The matrix explicitly selects the standard MOVE action when available;
  // QFileSystemModel still performs the real filesystem operation.
  void dragMoveEvent(QDragMoveEvent *e) override {QTreeView::dragMoveEvent(e);if(qEnvironmentVariableIsSet("WAYBRIDGE_DND_FIXTURE_MOVE")&&e->isAccepted()&&e->possibleActions().testFlag(Qt::MoveAction)){e->setDropAction(Qt::MoveAction);e->accept();}}
  void dropEvent(QDropEvent *e) override {if(qEnvironmentVariableIsSet("WAYBRIDGE_DND_FIXTURE_MOVE")&&e->possibleActions().testFlag(Qt::MoveAction))e->setDropAction(Qt::MoveAction);puts("FILE_DROP");QTreeView::dropEvent(e);printf("FILE_DROP_ACTION %d %d\n",int(e->dropAction()),e->isAccepted());}
};
int main(int argc,char **argv) {
  QApplication app(argc,argv);
  setvbuf(stdout,nullptr,_IOLBF,0);
#ifdef WAYBRIDGE_DND_XCB_TRACE
  NativeTrace trace;if(app.platformName()=="xcb")app.installNativeEventFilter(&trace);

#endif
  QWidget first,second;
  first.setWindowTitle("Waybridge drag source"+qEnvironmentVariable("WAYBRIDGE_DND_FIXTURE_SIDE")); first.resize(960,640);
  second.setWindowTitle("Waybridge drag target"+qEnvironmentVariable("WAYBRIDGE_DND_FIXTURE_SIDE")); second.resize(960,640);
  DragText source("Waybridge drag text",&first);DropEdit same(&first),other(&second);
  same.setMaxLength(1048576);other.setMaxLength(1048576);
  source.setGeometry(20,20,320,50);
  same.setGeometry(20,100,320,50);other.setGeometry(20,100,320,50);
  QObject::connect(&same,&QLineEdit::textChanged,[](const QString &s){printf("SAME %s\n",s.toUtf8().constData());});
  QObject::connect(&other,&QLineEdit::textChanged,[](const QString &s){if(qEnvironmentVariableIsSet("WAYBRIDGE_DND_FIXTURE_TEXT"))printf("OTHER %lld bytes\n",static_cast<long long>(s.toUtf8().size()));else printf("OTHER %s\n",s.toUtf8().constData());printf("OTHER_SHA256 %s\n",QCryptographicHash::hash(s.toUtf8(),QCryptographicHash::Sha256).toHex().constData());});
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
  if(argc<=1)tree.hide();
  first.show();second.show();return app.exec();
}
