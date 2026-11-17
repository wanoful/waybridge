#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
static void click(GLFWwindow *w,int b,int a,int m) {
  (void)b;(void)m;if(a!=GLFW_PRESS)return;
  double x,y;int width,height,fw,fh;
  glfwGetCursorPos(w,&x,&y);glfwGetWindowSize(w,&width,&height);glfwGetFramebufferSize(w,&fw,&fh);
  printf("CLICK %s %.3f %.3f %d %d %d %d\n",(const char*)glfwGetWindowUserPointer(w),x,y,width,height,fw,fh);
}
static GLFWwindow *create(const char *name,int x,int y) {
  GLFWwindow *w=glfwCreateWindow(640,480,name,NULL,NULL);if(!w)exit(3);
  glfwSetWindowUserPointer(w,(void*)name);glfwSetMouseButtonCallback(w,click);glfwSetWindowPos(w,x,y);
  return w;
}
static int dragging;
static double drag_x,drag_y;
static void csd_button(GLFWwindow *w,int b,int a,int m) {
  click(w,b,a,m);
  if(b==GLFW_MOUSE_BUTTON_LEFT) {
    double x,y;glfwGetCursorPos(w,&x,&y);
    dragging=a==GLFW_PRESS&&y<40;drag_x=x;drag_y=y;
    if(dragging&&getenv("WAYBRIDGE_TEST_EWMH")) {
      Display *display=glfwGetX11Display();int wx,wy;glfwGetWindowPos(w,&wx,&wy);
      XEvent event={0};event.xclient.type=ClientMessage;event.xclient.window=glfwGetX11Window(w);
      event.xclient.message_type=XInternAtom(display,"_NET_WM_MOVERESIZE",False);event.xclient.format=32;
      event.xclient.data.l[0]=wx+(long)x;event.xclient.data.l[1]=wy+(long)y;
      event.xclient.data.l[2]=8;event.xclient.data.l[3]=1;event.xclient.data.l[4]=1;
      XUngrabPointer(display,CurrentTime);
      XSendEvent(display,DefaultRootWindow(display),False,SubstructureRedirectMask|SubstructureNotifyMask,&event);
      XFlush(display);dragging=0;
    }
  }
}
static void csd_motion(GLFWwindow *w,double x,double y) {
  if(dragging) {
    int wx,wy;glfwGetWindowPos(w,&wx,&wy);
    glfwSetWindowPos(w,wx+(int)(x-drag_x),wy+(int)(y-drag_y));
  }
}
int main(void) {
  setvbuf(stdout,NULL,_IOLBF,0);glfwInitHint(GLFW_PLATFORM,GLFW_PLATFORM_X11);
  if(!glfwInit())return 2;
  if(getenv("WAYBRIDGE_TEST_CSD"))glfwWindowHint(GLFW_DECORATED,GLFW_FALSE);
  GLFWwindow *launcher=create("Waybridge GLFW launcher",120,80),*game=NULL;
  if(getenv("WAYBRIDGE_TEST_CSD")) {
    glfwSetMouseButtonCallback(launcher,csd_button);
    glfwSetCursorPosCallback(launcher,csd_motion);
  }
  fcntl(0,F_SETFL,fcntl(0,F_GETFL)|O_NONBLOCK);
  while(!glfwWindowShouldClose(launcher)) {
    char c;
    while(read(0,&c,1)==1) {
      if(c=='g'&&!game)game=create("Waybridge GLFW game",430,210);
      if(c=='l'&&game){glfwSetInputMode(game,GLFW_CURSOR,GLFW_CURSOR_DISABLED);puts("LOCKED");}
      if(c=='c'&&game){glfwDestroyWindow(game);game=NULL;puts("GAME_CLOSED");}
      if(c=='o'){glfwSetWindowPos(launcher,-283,591);puts("OFFSCREEN_REQUEST");}
      if(c=='m'&&game){glfwMaximizeWindow(game);puts("MAXIMIZE_REQUEST");}
      if(c=='f'&&game){GLFWmonitor *monitor=glfwGetPrimaryMonitor();const GLFWvidmode *mode=glfwGetVideoMode(monitor);glfwSetWindowMonitor(game,monitor,0,0,mode->width,mode->height,mode->refreshRate);puts("FULLSCREEN_REQUEST");}
      if(c=='q')glfwSetWindowShouldClose(launcher,GLFW_TRUE);
    }
    GLFWwindow *windows[]={launcher,game};
    for(int i=0;i<2;++i)if(windows[i]) {
      glfwMakeContextCurrent(windows[i]);int w,h;glfwGetFramebufferSize(windows[i],&w,&h);
      glViewport(0,0,w,h);glClearColor(i?.1f:.2f,.3f,i?.5f:.2f,1);glClear(GL_COLOR_BUFFER_BIT);glfwSwapBuffers(windows[i]);
    }
    glfwPollEvents();usleep(16000);
  }
  if(game)glfwDestroyWindow(game);glfwDestroyWindow(launcher);glfwTerminate();return 0;
}
