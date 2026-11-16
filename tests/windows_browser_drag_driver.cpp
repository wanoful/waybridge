// Physical mouse input to disposable browser fixtures only.
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <algorithm>
#include <iostream>
#include <string>
#include <thread>
static HWND nativeWindow=nullptr,remoteWindow=nullptr;
static BOOL CALLBACK collect(HWND hwnd,LPARAM){wchar_t title[512]{},kind[100]{};GetWindowTextW(hwnd,title,512);GetClassNameW(hwnd,kind,100);std::wstring t(title),c(kind);if(IsWindowVisible(hwnd)&&t.find(L"Waybridge Chromium DND")!=std::wstring::npos){if(c==L"Chrome_WidgetWin_1"&&t.find(L"native")!=std::wstring::npos)nativeWindow=hwnd;if(c==L"WaybridgeRemote"&&t.find(L"remote")!=std::wstring::npos)remoteWindow=hwnd;}return TRUE;}
static void button(DWORD flags){INPUT i{};i.type=INPUT_MOUSE;i.mi.dwFlags=flags;if(SendInput(1,&i,sizeof(i))!=1)ExitProcess(8);}
static POINT point(HWND hwnd,double x,double y,double innerHeight){RECT r{};GetClientRect(hwnd,&r);double scale=double(GetDpiForWindow(hwnd))/96;POINT p{LONG(x*scale+.5),LONG(r.bottom-innerHeight*scale+y*scale+.5)};ClientToScreen(hwnd,&p);return p;}
static bool hit(HWND hwnd,POINT p){return GetAncestor(WindowFromPoint(p),GA_ROOT)==hwnd;}
int wmain(int argc,wchar_t **argv){SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);EnumWindows(collect,0);if(!nativeWindow||!remoteWindow)return 3;if(argc==2&&std::wstring(argv[1])==L"prepare"){
 SetWindowPos(nativeWindow,HWND_TOPMOST,80,80,650,650,SWP_NOACTIVATE|SWP_SHOWWINDOW);SetWindowPos(remoteWindow,HWND_TOPMOST,900,80,650,650,SWP_NOACTIVATE|SWP_SHOWWINDOW);return 0;}
 if(argc==2&&std::wstring(argv[1])==L"restore"){for(HWND w:{nativeWindow,remoteWindow})SetWindowPos(w,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);return 0;}
 if(argc!=10)return 2;
 bool reverse=std::wstring(argv[1])==L"out";HWND source=reverse?remoteWindow:nativeWindow,target=reverse?nativeWindow:remoteWindow;
 POINT from=point(source,_wtof(argv[2]),_wtof(argv[3]),_wtof(argv[4]));POINT to=point(target,_wtof(argv[5]),_wtof(argv[6]),_wtof(argv[7]));int hover=_wtoi(argv[8]);bool cancel=std::wstring(argv[9])==L"cancel";
 if(!hit(source,from)||!hit(target,to)){std::cerr<<"fixture occluded\n";return 4;}
 POINT original{};GetCursorPos(&original);HWND foreground=GetForegroundWindow();SetForegroundWindow(source);Sleep(150);SetCursorPos(from.x,from.y);button(MOUSEEVENTF_LEFTDOWN);Sleep(120);
 SetCursorPos(from.x+15,from.y);Sleep(150);
 for(int i=1;i<=20;++i){SetCursorPos(from.x+(to.x-from.x)*i/20,from.y+(to.y-from.y)*i/20);if(i<20)Sleep(16);}
 if(hover)Sleep(hover);
 if(cancel){INPUT key{};key.type=INPUT_KEYBOARD;key.ki.wVk=VK_ESCAPE;SendInput(1,&key,sizeof(key));key.ki.dwFlags=KEYEVENTF_KEYUP;SendInput(1,&key,sizeof(key));}
 button(MOUSEEVENTF_LEFTUP);Sleep(250);SetCursorPos(original.x,original.y);if(IsWindow(foreground))SetForegroundWindow(foreground);
 std::cout<<"DRAG "<<(reverse?"out":"in")<<" hover="<<hover<<" cancelled="<<cancel<<" from="<<from.x<<","<<from.y<<" to="<<to.x<<","<<to.y<<std::endl;return 0;
}
