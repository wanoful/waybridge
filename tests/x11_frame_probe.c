#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char**argv) {
  if(argc<2)return 2;
  Display*d=XOpenDisplay(NULL);if(!d)return 3;
  Atom type;int format;unsigned long count,left;unsigned char*bytes=NULL;
  Atom list=XInternAtom(d,"_NET_CLIENT_LIST",True);
  if(!list || XGetWindowProperty(d,DefaultRootWindow(d),list,0,256,False,XA_WINDOW,&type,&format,&count,&left,&bytes)!=Success)return 4;
  int result=5;
  if(type==XA_WINDOW&&format==32)for(unsigned long i=0;i<count;++i) {
    Window child=((Window*)bytes)[i];char*name=NULL;
    if(!XFetchName(d,child,&name)||!name)continue;
    int matches=!strcmp(name,argv[1]);XFree(name);if(!matches)continue;
    Window root,parent,*children=NULL;unsigned n=0;
    if(!XQueryTree(d,child,&root,&parent,&children,&n))break;
    if(children)XFree(children);
    XWindowAttributes a,b;
    if(!XGetWindowAttributes(d,child,&a)||!XGetWindowAttributes(d,parent,&b))break;
    printf("child depth=%d colormap=%lx frame depth=%d colormap=%lx\n",a.depth,a.colormap,b.depth,b.colormap);
    result=a.depth==b.depth && a.colormap!=None && b.colormap==a.colormap && XVisualIDFromVisual(a.visual)==XVisualIDFromVisual(b.visual) && (argc<3 || a.depth==atoi(argv[2]))?0:6;
    break;
  }
  if(bytes)XFree(bytes);
  XCloseDisplay(d);
  return result;
}
