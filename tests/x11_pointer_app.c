#include <GLFW/glfw3.h>
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
int main(void) {
  setvbuf(stdout,NULL,_IOLBF,0);glfwInitHint(GLFW_PLATFORM,GLFW_PLATFORM_X11);
  if(!glfwInit())return 2;
  GLFWwindow *launcher=create("Waybridge GLFW launcher",120,80),*game=NULL;
  fcntl(0,F_SETFL,fcntl(0,F_GETFL)|O_NONBLOCK);
  while(!glfwWindowShouldClose(launcher)) {
    char c;
    while(read(0,&c,1)==1) {
      if(c=='g'&&!game)game=create("Waybridge GLFW game",430,210);
      if(c=='l'&&game){glfwSetInputMode(game,GLFW_CURSOR,GLFW_CURSOR_DISABLED);puts("LOCKED");}
      if(c=='c'&&game){glfwDestroyWindow(game);game=NULL;puts("GAME_CLOSED");}
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
