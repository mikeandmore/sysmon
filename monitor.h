// -*- mode: c++ -*-

#ifndef MONITOR_H
#define MONITOR_H

#include <sstream>
#include <vector>
#include <limits>
#include <array>
#include <map>
#include <functional>
#include <cstdio>

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

namespace sysmon {

// Abstract factory stuff copied from my old project

template <typename T, typename ...Args>
class BaseFactory {
 public:
  typedef std::vector<std::function<T *(Args...)>> Table;
 protected:
  static Table table;
  static void AddToTable(std::function<T *(Args...)> f) {
    table.push_back(f);
  }
 public:
  static T *Create(int n, Args... args) {
    return table[n](args...);
  }
};

template <typename T, typename ...Args>
typename BaseFactory<T, Args...>::Table BaseFactory<T, Args...>::table;

template <typename T, int LastEnum, typename ...Args>
class Factory : public Factory<T, LastEnum - 1, Args...> {
  typedef Factory<T, LastEnum - 1, Args...> Super;
 public:
  static void Initialize() {
    Super::Initialize();
    Super::AddToTable([](Args... args) {
        return Factory<T, LastEnum - 1, Args...>::Construct(args...);
      });
  }
  static T *Construct(Args ...args);
};

template <typename T, typename ...Args>
class Factory<T, 0, Args...> : public BaseFactory<T, Args...> {
 public:
  static void Initialize() {}
  static T *Construct(Args ...args);
};

// sysmon factories
enum WidgetKind : int {
  CpuKind,
  StorageKind,
  NetworkKind,
  MemoryKind,
  BacklightKind,
  VolumeKind,
  TimeKind,
  BatteryKind,
};

class Widget;

class RenderContext {
  Display *dpy;
  XftFont *font;
  Window win;
  XftDraw *draw;
  XftColor color;
  ulong window_length;
  friend class Bar;
  RenderContext(Display *dpy, XftFont *font, Window win, ulong window_length)
      : dpy(dpy), font(font), win(win),
        draw(XftDrawCreate(dpy, win, XDefaultVisual(dpy, 0), XDefaultColormap(dpy, 0))),
        window_length(window_length) {
    ResetColor();
  }
  ~RenderContext() {
    XftDrawDestroy(draw);
  }
 public:
  static double g_dpi_scale;
  long Translate(Widget *, long offset);
  RenderContext *DrawText(Widget *, std::string str, long offset = 0);
  RenderContext *DrawBlock(Widget *, long offset, size_t length);
  RenderContext *DrawBitmap(Widget *, Pixmap bitmap, size_t width, size_t height, long offset = 0);
  RenderContext *ResetColor() {
    auto m = std::numeric_limits<ushort>::max();
    SetColor(m / 255 * 253, m / 255 * 254, m / 255 * 254);
    return this;
  }
  RenderContext *SetColor(ushort r, ushort g, ushort b) {
    color.color = {
      .red = r, .green = g, .blue = b, .alpha = std::numeric_limits<ushort>::max(),
    };
    color.pixel = std::numeric_limits<ulong>::max();
    return this;
  }
};

enum AlignmentType : int {
  Left, Right, AllTypes
};

struct Alignment {
  AlignmentType type;
  size_t pos;
};

class Bar;

class Widget {
  friend class Bar;
  friend class RenderContext;
  Alignment align;
 public:
  virtual void OnAdd(Bar *bar) {}
  virtual void Refresh() = 0;

  virtual void Render(RenderContext *ctx) = 0;
  virtual size_t Width() = 0;
};

class Bar {
  std::array<size_t, AlignmentType::AllTypes> pos;
  std::vector<Widget *> widgets;
  std::vector<std::function<void ()>> per_second_funcs;
  std::map<std::string, std::function<void ()>> cmd_map;
  Display *dpy;
  XftFont *font;
  std::vector<RenderContext *> ctxs;

  Window CreateWindow(int x, int y, int width, int height);

 public:
  static bool g_all_screens;
  static bool g_screen_top;
  static int g_height;
  Bar(Display *dpy);
  void Configure();
  void Add(Widget *widget, AlignmentType type);

  void RegisterPerSecondRefresh(std::function<void ()> func) {
    per_second_funcs.push_back(func);
  }
  void RegisterCommand(std::string cmd, std::function<void ()> func) {
    cmd_map[cmd] = func;
  }

  Pixmap LoadBitmap(const uint8_t* data, unsigned int width, unsigned int height);

  void RefreshPerSecond();
  void Refresh();
  void Execute(std::string cmd) {
    auto it = cmd_map.end();
    if ((it = cmd_map.find(cmd)) != cmd_map.end()) {
      it->second();
    }
  }
};

}

#endif
