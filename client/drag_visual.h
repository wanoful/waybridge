#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <climits>
#include <string>
#include <vector>
#include "third_party/json.hpp"

// One ephemeral layered ghost. It has no input/focus role and owns no remote
// application window; source pixels arrive as bounded, premultiplied RGBA.
class DragVisual {
  using Json = nlohmann::json;
  HWND controller = nullptr, ghost = nullptr;
  bool negotiated = false, dirty = false;
  uint64_t generation = 0, revision = 0, ended = 0;
  int width = 0, height = 0, offsetX = 0, offsetY = 0;
  double scale = 1;
  UINT dpi = 0;
  POINT last{LONG_MIN,LONG_MIN};
  std::vector<uint8_t> rgba;
  static constexpr const wchar_t *ClassName = L"WaybridgeDndVisual";
  static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return TRUE;
    return DefWindowProcW(hwnd,message,wp,lp);
  }
  void hide() {
    if (controller) KillTimer(controller,Timer);
    if (ghost) { DestroyWindow(ghost); ghost=nullptr; }
    rgba.clear(); width=height=0; dpi=0; dirty=false; last={LONG_MIN,LONG_MIN};
  }
  bool create() {
    if (ghost) return true;
    WNDCLASSW cls{}; cls.hInstance=GetModuleHandleW(nullptr); cls.lpfnWndProc=proc; cls.lpszClassName=ClassName;
    RegisterClassW(&cls);
    ghost=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,
      ClassName,L"",WS_POPUP,0,0,1,1,controller,nullptr,cls.hInstance,nullptr);
    return ghost != nullptr;
  }
  static bool integer(const Json &m,const char *key,int low,int high,int &result) {
    auto value=m.find(key); if(value==m.end()||!value->is_number_integer())return false;
    if(value->is_number_unsigned()&&value->get<uint64_t>()>uint64_t(INT_MAX))return false;
    auto n=value->get<int64_t>();if(n<low||n>high)return false;result=int(n);return true;
  }
  static bool decode(const std::string &text,std::vector<uint8_t> &out,size_t count) {
    if(text.size()!=((count+2)/3)*4)return false;
    auto digit=[](unsigned char c) { return c>='A'&&c<='Z'?int(c-'A'):c>='a'&&c<='z'?int(c-'a'+26):c>='0'&&c<='9'?int(c-'0'+52):c=='+'?62:c=='/'?63:-1; };
    out.resize(count);size_t at=0;
    for(size_t i=0;i<text.size();i+=4) {
      int a=digit(text[i]),b=digit(text[i+1]),c=text[i+2]=='='?0:digit(text[i+2]),d=text[i+3]=='='?0:digit(text[i+3]);
      size_t n=(std::min)(size_t(3),count-at);
      if(a<0||b<0||c<0||d<0||(n==3&&(text[i+2]=='='||text[i+3]=='='))||
        (n==2&&(text[i+2]=='='||text[i+3]!='='||(c&3)))||(n==1&&(text[i+2]!='='||text[i+3]!='='||(b&15))))return false;
      uint32_t v=(uint32_t(a)<<18)|(uint32_t(b)<<12)|(uint32_t(c)<<6)|uint32_t(d);
      out[at++]=uint8_t(v>>16);if(n>1)out[at++]=uint8_t(v>>8);if(n>2)out[at++]=uint8_t(v);
    }
    return true;
  }
  bool paint(POINT point,UINT nextDpi) {
    double factor=double(nextDpi)/96.0/scale;
    factor=(std::min)(factor,(std::min)(512.0/width,512.0/height));
    factor=(std::min)(factor,std::sqrt(65536.0/(double(width)*height)));
    int w=(std::max)(1,int(std::lround(width*factor))),h=(std::max)(1,int(std::lround(height*factor)));
    if(uint64_t(w)*h>65536) { factor*=0.99;w=(std::max)(1,int(std::lround(width*factor)));h=(std::max)(1,int(std::lround(height*factor))); }
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;
    info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void *bits=nullptr;HBITMAP bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!bitmap||!bits) { if(bitmap)DeleteObject(bitmap);return false; }
    auto *output=static_cast<uint8_t *>(bits);
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
      auto *pixel=&rgba[(size_t(y)*height/h*width+size_t(x)*width/w)*4];
      size_t i=(size_t(y)*w+x)*4;output[i]=pixel[2];output[i+1]=pixel[1];output[i+2]=pixel[0];output[i+3]=pixel[3];
    }
    HDC dc=CreateCompatibleDC(nullptr);if(!dc){DeleteObject(bitmap);return false;}
    auto old=SelectObject(dc,bitmap);POINT source{};SIZE size{w,h};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    bool ok=UpdateLayeredWindow(ghost,nullptr,&point,&size,dc,&source,0,&blend,ULW_ALPHA)!=FALSE;
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    if(ok) { ShowWindow(ghost,SW_SHOWNOACTIVATE);SetWindowPos(ghost,HWND_TOPMOST,point.x,point.y,0,0,SWP_NOSIZE|SWP_NOACTIVATE); }
    return ok;
  }
public:
  static constexpr UINT_PTR Timer = 0x57424456;
  DragVisual()=default;
  DragVisual(const DragVisual &)=delete;
  DragVisual &operator=(const DragVisual &)=delete;
  ~DragVisual(){hide();}
  void attach(HWND owner){controller=owner;}
  void configure(bool enabled){hide();negotiated=enabled;generation=revision=ended=0;}
  void reset(){ended=(std::max)(ended,generation);hide();}
  bool owns(HWND hwnd)const{return ghost&&hwnd==ghost;}
  HWND window()const{return ghost;}
  bool receive(const Json &m) {
    try {
      if(!negotiated||m.value("type","")!="drag_visual"||!m.contains("generation")||!m["generation"].is_number_unsigned()||
        !m.contains("revision")||!m["revision"].is_number_unsigned()||!m.contains("active")||!m["active"].is_boolean())return false;
      uint64_t next=m["generation"].get<uint64_t>(),version=m["revision"].get<uint64_t>();
      bool active=m["active"].get<bool>();
      if(!next||!version||next<generation||(next==generation&&version<=revision)||(active&&next<=ended))return false;
      if(!active) { generation=next;revision=version;ended=(std::max)(ended,next);hide();return true; }
      if(!m.value("visible",false)) { generation=next;revision=version;hide();return true; }
      int w,h,x,y;double sourceScale=m.value("scale",0.0);
      if(m.value("format","")!="rgba8-premultiplied"||!integer(m,"width",1,512,w)||!integer(m,"height",1,512,h)||
        uint64_t(w)*h>65536||!integer(m,"offset_x",-32768,32768,x)||!integer(m,"offset_y",-32768,32768,y)||
        !std::isfinite(sourceScale)||sourceScale<1||sourceScale>4||!m.contains("pixels")||!m["pixels"].is_string())return false;
      std::vector<uint8_t> pixels;
      if(!decode(m["pixels"].get_ref<const std::string &>(),pixels,size_t(w)*h*4))return false;
      generation=next;revision=version;width=w;height=h;offsetX=x;offsetY=y;scale=sourceScale;rgba=std::move(pixels);dirty=true;
      if(!create()){hide();return false;}
      if(controller)SetTimer(controller,Timer,16,nullptr);
      tick();return true;
    } catch(const Json::exception &){return false;}
  }
  void tick() {
    if(!ghost||rgba.empty())return;
    POINT cursor{};if(!GetCursorPos(&cursor))return;
    UINT nextDpi=GetDpiForWindow(ghost);if(!nextDpi)nextDpi=96;nextDpi=(std::clamp)(nextDpi,96u,384u);
    POINT point{cursor.x+MulDiv(offsetX,int(nextDpi),96),cursor.y+MulDiv(offsetY,int(nextDpi),96)};
    if(dirty||dpi!=nextDpi) { if(!paint(point,nextDpi)){hide();return;}dirty=false;dpi=nextDpi; }
    else if(point.x!=last.x||point.y!=last.y)SetWindowPos(ghost,HWND_TOPMOST,point.x,point.y,0,0,SWP_NOSIZE|SWP_NOACTIVATE);
    last=point;
  }
};
