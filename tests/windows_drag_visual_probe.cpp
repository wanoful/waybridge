// Own cross-process underlay plus production DND ghost. No user app is touched.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "drag_visual.h"
#include <iostream>
#include <stdexcept>
static unsigned clicks;
static LRESULT CALLBACK underlayProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
  if(message==WM_LBUTTONDOWN){SetPropW(hwnd,L"WaybridgeProbeClicks",reinterpret_cast<HANDLE>(uintptr_t(++clicks)));return 0;}
  if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
  return DefWindowProcW(hwnd,message,wp,lp);
}
static void require(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
static void pump(){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(20);}
static std::string encode(const std::vector<uint8_t> &bytes){
 static constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
 std::string result;for(size_t i=0;i<bytes.size();i+=3){uint32_t value=uint32_t(bytes[i])<<16;if(i+1<bytes.size())value|=uint32_t(bytes[i+1])<<8;if(i+2<bytes.size())value|=bytes[i+2];
 result+=alphabet[(value>>18)&63];result+=alphabet[(value>>12)&63];result+=i+1<bytes.size()?alphabet[(value>>6)&63]:'=';result+=i+2<bytes.size()?alphabet[value&63]:'=';}return result;
}
int wmain(int argc,wchar_t **argv){
 if(argc==3&&std::wstring(argv[1])==L"--underlay"){
  std::wstring name=L"WaybridgeDndProbeUnderlay-"+std::wstring(argv[2]);
  WNDCLASSW cls{};cls.lpfnWndProc=underlayProc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=name.c_str();cls.hbrBackground=CreateSolidBrush(RGB(0,0,255));RegisterClassW(&cls);
  HWND hwnd=CreateWindowExW(0,name.c_str(),L"Waybridge isolated drag underlay",WS_OVERLAPPEDWINDOW,140,140,520,360,nullptr,nullptr,cls.hInstance,nullptr);
  ShowWindow(hwnd,SW_SHOW);MSG m{};while(GetMessageW(&m,nullptr,0,0)>0){TranslateMessage(&m);DispatchMessageW(&m);}DeleteObject(cls.hbrBackground);return 0;
 }
 POINT saved{};GetCursorPos(&saved);HWND original=GetForegroundWindow(),underlay=nullptr,owner=nullptr;PROCESS_INFORMATION child{};
 try{
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  std::wstring id=std::to_wstring(GetCurrentProcessId()),name=L"WaybridgeDndProbeUnderlay-"+id;
  wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
  std::wstring command=L"\""+std::wstring(executable)+L"\" --underlay "+id;STARTUPINFOW startup{};startup.cb=sizeof(startup);
  require(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&child)!=FALSE,"spawn own underlay");CloseHandle(child.hThread);child.hThread=nullptr;
  ULONGLONG deadline=GetTickCount64()+10000;while(!(underlay=FindWindowW(name.c_str(),nullptr))&&GetTickCount64()<deadline)pump();require(underlay,"own underlay not ready");
  ShowWindow(underlay,SW_SHOW);SetForegroundWindow(underlay);SetWindowPos(underlay,HWND_TOP,140,140,520,360,SWP_NOACTIVATE);
  for(int i=0;i<10;i++)pump();RECT client{};GetClientRect(underlay,&client);POINT point{client.right/2,client.bottom/2};ClientToScreen(underlay,&point);
  require(GetAncestor(WindowFromPoint(point),GA_ROOT)==underlay,"initial click is not over own underlay");
  SetCursorPos(point.x,point.y);INPUT click[2]{};click[0].type=click[1].type=INPUT_MOUSE;click[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;click[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(2,click,sizeof(INPUT));for(int i=0;i<5;i++)pump();require(GetForegroundWindow()==underlay,"self-click did not activate own underlay");
  owner=CreateWindowExW(0,L"STATIC",L"",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);require(owner,"create timer controller");
  DragVisual visual;visual.attach(owner);visual.configure(true);
  std::vector<uint8_t> pixels(32*32*4);for(size_t i=0;i<pixels.size();i+=4){pixels[i]=128;pixels[i+3]=128;}
  nlohmann::json image={{"type","drag_visual"},{"generation",uint64_t(1)},{"revision",uint64_t(1)},
   {"active",true},{"visible",true},{"format","rgba8-premultiplied"},{"width",32},{"height",32},{"scale",1.0},{"offset_x",-8},{"offset_y",-8},{"pixels",encode(pixels)}};
  require(visual.receive(image),"valid ghost rejected");for(int i=0;i<5;i++){visual.tick();pump();}
  HWND ghost=visual.window();require(ghost&&IsWindowVisible(ghost)&&visual.owns(ghost),"ghost not visible");
  auto style=GetWindowLongPtrW(ghost,GWL_EXSTYLE);require((style&(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW))==(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW),"missing ghost window styles");
  require(GetForegroundWindow()==underlay,"ghost took foreground");require(SendMessageW(ghost,WM_NCHITTEST,0,MAKELPARAM(point.x,point.y))==HTTRANSPARENT,"ghost hit test not transparent");
  uintptr_t before=reinterpret_cast<uintptr_t>(GetPropW(underlay,L"WaybridgeProbeClicks"));
  require(SendInput(2,click,sizeof(INPUT))==2,"test click failed");for(int i=0;i<6;i++)pump();
  require(reinterpret_cast<uintptr_t>(GetPropW(underlay,L"WaybridgeProbeClicks"))==before+1,"ghost blocked click to other-process underlay");require(GetForegroundWindow()==underlay,"click on ghost changed foreground");
  if(argc==2&&std::wstring(argv[1])==L"--pixels"){
   HDC screen=GetDC(nullptr);COLORREF color=GetPixel(screen,point.x,point.y);ReleaseDC(nullptr,screen);
   require(GetRValue(color)>110&&GetRValue(color)<145&&GetBValue(color)>110&&GetBValue(color)<145&&GetGValue(color)<10,"premultiplied ghost alpha not composited over blue underlay");
  }
  POINT moved{point.x+50,point.y+40};SetCursorPos(moved.x,moved.y);visual.tick();RECT rect{};GetWindowRect(ghost,&rect);require(rect.left>point.x&&rect.top>point.y,"ghost did not follow pointer");
  require(visual.receive({{"type","drag_visual"},{"generation",uint64_t(1)},{"revision",uint64_t(2)},{"active",false}})&&!visual.window(),"end did not destroy ghost");
  image["revision"]=uint64_t(99);require(!visual.receive(image)&&!visual.window(),"ended generation revived");
  image["generation"]=uint64_t(2);image["revision"]=uint64_t(1);require(visual.receive(image),"new drag not displayed");
  auto bad=image;bad["width"]=513;bad["revision"]=uint64_t(2);require(!visual.receive(bad),"oversized drag accepted");
  visual.reset();require(!visual.window(),"disconnect left ghost");require(!visual.receive(image),"old frame accepted after reset");visual.configure(false);require(!visual.receive(image),"unnegotiated frame accepted");
  if(IsWindow(original))SetForegroundWindow(original);PostMessageW(underlay,WM_CLOSE,0,0);WaitForSingleObject(child.hProcess,3000);CloseHandle(child.hProcess);child.hProcess=nullptr;DestroyWindow(owner);owner=nullptr;SetCursorPos(saved.x,saved.y);
  std::cout<<"PASS: real layered premultiplied DND ghost, cross-process mouse passthrough, NOACTIVATE foreground, pointer tracking, ended/stale generation rejection and disconnect cleanup\n";return 0;
 }catch(const std::exception &error){
  if(IsWindow(original))SetForegroundWindow(original);if(underlay)PostMessageW(underlay,WM_CLOSE,0,0);if(child.hProcess){if(WaitForSingleObject(child.hProcess,2000)!=WAIT_OBJECT_0)TerminateProcess(child.hProcess,1);CloseHandle(child.hProcess);}if(owner)DestroyWindow(owner);SetCursorPos(saved.x,saved.y);std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
 }
}
