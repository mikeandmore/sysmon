#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <climits>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include <X11/extensions/Xrandr.h>
#include <X11/Xatom.h>

#include "monitor.h"

namespace sysmon {

double RenderContext::g_dpi_scale = 1.0;

long RenderContext::Translate(Widget *w, long offset)
{
  if (w->align.type == AlignmentType::Left) {
    return std::lrint((w->align.pos + offset) * g_dpi_scale);
  } else if (w->align.type == AlignmentType::Right) {
    return std::lrint(window_length - g_dpi_scale * (w->align.pos + w->Width()) + g_dpi_scale * offset);
  } else {
    abort();
  }
}

RenderContext *RenderContext::DrawText(Widget *w, std::string str, long offset)
{
  XftDrawStringUtf8(draw, &color, font,
                    Translate(w, offset), std::lrint(0.75 * Bar::g_height),
                    (const FcChar8 *) str.c_str(), str.length());
  return this;
}

RenderContext *RenderContext::DrawBlock(Widget *w, long offset, size_t length)
{
  XftDrawRect(draw, &color, Translate(w, offset), std::lrint(0.25 * Bar::g_height),
              std::lrint(g_dpi_scale * length), std::lrint(0.5 * Bar::g_height));
  return this;
}

RenderContext *RenderContext::DrawBitmap(Widget *w, Pixmap bitmap, size_t width, size_t height, long offset)
{
  XCopyPlane(dpy, bitmap, win, XDefaultGC(dpy, 0), 0, 0, width, height,
             Translate(w, offset), (Bar::g_height - height) / 2, 1);
  return this;
}

Bar::Bar(Display *dpy)
    : dpy(dpy)
{
  pos.fill(0);
  font = XftFontOpen(dpy, XDefaultScreen(dpy),
                     XFT_FAMILY, XftTypeString, "Sans",
                     XFT_SIZE, XftTypeDouble, 10.0,
                     nullptr);
}

Pixmap Bar::LoadBitmap(const uint8_t *data, unsigned int width, unsigned int height)
{
  Window w = XRootWindow(dpy, 0);
  return XCreateBitmapFromData(dpy, w, (char *) data, width, height);
}

Window Bar::CreateWindow(int x, int y, int width, int height)
{
  XSetWindowAttributes attr;
  attr.background_pixel = 0;
  attr.event_mask = ExposureMask;
  attr.override_redirect = 1;

  auto w = XCreateWindow(
      dpy, XDefaultRootWindow(dpy), x, y, width, height, 0,
      CopyFromParent, CopyFromParent, CopyFromParent,
      CWBackPixel | CWEventMask, &attr);

  Atom props[] = {
    XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", 0),
  };

  XChangeProperty(
      dpy, w,
      XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", 0), XA_ATOM, 32,
      PropModeAppend, (unsigned char *) props, sizeof(props) / sizeof(Atom));

  unsigned long mwm_decor[] = {
    0x02, 0, 0, 0, 0
  };

  Atom mwm = XInternAtom(dpy, "_MOTIF_WM_HINTS", 0);

  XChangeProperty(
      dpy, w,
      mwm, mwm, 32,
      PropModeReplace, (unsigned char *) mwm_decor, 5);

  unsigned long struts[] = {
    0, 0, 0, 0,
  };

  if (g_screen_top)
    struts[2] = height;
  else
    struts[3] = height;

  XChangeProperty(
      dpy, w,
      XInternAtom(dpy, "_NET_WM_STRUT", 0), XA_CARDINAL, 32,
      PropModeReplace, (unsigned char *) struts, 4);

  XSizeHints hints;
  memset(&hints, 0, sizeof(XSizeHints));
  hints.flags |= PPosition | PSize | PBaseSize;
  XSetWMNormalHints(dpy, w, &hints);

  return w;
}

void Bar::Configure()
{
  for (auto c: ctxs) {
    auto w = c->win;
    delete c;
    XUnmapWindow(dpy, w);
    XDestroyWindow(dpy, w);
  }
  ctxs.clear();

  XRRScreenResources *sres =
      XRRGetScreenResources(dpy, XDefaultRootWindow(dpy));

  XSetForeground(dpy, XDefaultGC(dpy, 0),
                 std::numeric_limits<unsigned long>::max());
  XSetBackground(dpy, XDefaultGC(dpy, 0), 0);

  std::vector<XRRCrtcInfo *> all_sinfo;

  int max_h = 0, max_w = 0;
  for (int i = 0; i < sres->ncrtc; i++) {
    XRRCrtcInfo *sinfo = XRRGetCrtcInfo(dpy, sres, sres->crtcs[i]);
    if (sinfo == nullptr)
      continue;
    if (sinfo->noutput > 0) {
      max_w = std::max(max_w, (int) sinfo->width + sinfo->x);
      max_h = std::max(max_h, (int) sinfo->height + sinfo->y);
      all_sinfo.push_back(sinfo);
    } else {
      XRRFreeCrtcInfo(sinfo);
    }
  }

  for (auto sinfo: all_sinfo) {
    if (g_all_screens
        || (g_screen_top && sinfo->y == 0)
        || (!g_screen_top && sinfo->y + sinfo->height == max_h)) {
      int y = g_screen_top ? 0 : max_h - g_height;
      auto w = CreateWindow(sinfo->x, y, sinfo->width, g_height);
      XMapWindow(dpy, w);
      ctxs.push_back(new RenderContext(dpy, font, w, sinfo->width));
    }
    XRRFreeCrtcInfo(sinfo);
  }
  XRRFreeScreenResources(sres);
  puts("configured");
}

void Bar::Add(Widget *wid, AlignmentType type)
{
  widgets.push_back(wid);
  wid->align.type = type;
  wid->align.pos = pos[type];
  pos[type] += wid->Width();
  wid->OnAdd(this);
}

void Bar::RefreshPerSecond()
{
  for (auto func: per_second_funcs) {
    func();
  }
  Refresh();
}

void Bar::Refresh()
{
  for (auto w: widgets) {
    w->Refresh();
  }
  for (auto ctx: ctxs) {
    XClearWindow(dpy, ctx->win);
    for (auto w: widgets) {
      w->Render(ctx);
    }
  }
}

class MainLoop {
  std::string fifo_path;
  Display *dpy;
 public:
  MainLoop();

  void Run(Bar *bar);

  Display *display() const { return dpy; }
 private:
  void OpenFifo(struct pollfd *pfd);
  void OpenXDisplay(struct pollfd *pfd);
};

MainLoop::MainLoop()
{
  char path[PATH_MAX];
  snprintf(path, PATH_MAX, "%s/.sys-monitor.fifo", getenv("HOME"));
  fifo_path = path;

  dpy = XOpenDisplay(nullptr);
  if (dpy == nullptr) {
    perror("XOpenDisplay");
    std::abort();
  }
}

void MainLoop::OpenFifo(struct pollfd *pfd)
{
  if ((pfd->fd = open(fifo_path.c_str(), O_RDONLY | O_NONBLOCK)) < 0) {
    perror("open");
    std::abort();
  }
  pfd->events = POLLIN;
}

void MainLoop::OpenXDisplay(struct pollfd *pfd)
{
  pfd->fd = ConnectionNumber(dpy);
  pfd->events = POLLIN;

  XRRSelectInput(dpy, XDefaultRootWindow(dpy), RRScreenChangeNotifyMask);
}

void MainLoop::Run(Bar *bar)
{
  struct pollfd fds[2];
  struct pollfd *cfd = &fds[0];
  struct pollfd *xfd = &fds[1];

  int err_base, xrr_base;
  if (!XRRQueryExtension(dpy, &xrr_base, &err_base)) {
    std::abort();
  }

  printf("xrr_event_base %d\n", xrr_base);

  char command[PATH_MAX];
  int timeout = 1000;
  struct timeval last;
  gettimeofday(&last, NULL);

  OpenFifo(cfd);
  OpenXDisplay(xfd);

  bar->Refresh();

  while (true) {
    XFlush(dpy);
    int ret = poll(fds, 2, timeout);
    if (ret < 0) {
      if (errno == EINTR) continue;
      perror("poll");
      std::abort();
    }

    if (ret == 0) {
      bar->RefreshPerSecond();
      gettimeofday(&last, NULL);
      timeout = 1000;
      continue;
    }

    if (xfd->revents & POLLIN) {
      while (XPending(dpy)) {
        XEvent evt;
        XNextEvent(dpy, &evt);
        if (evt.type == xrr_base + RRScreenChangeNotify) {
          bar->Configure();
        } else if (evt.type == Expose) {
          bar->Refresh();
        }
      }
    }

    if (xfd->revents & POLLHUP) {
      // Exiting the entire program
      return;
    }

    if (cfd->revents & POLLIN) {
      char *p = command;
      int len = PATH_MAX;
      memset(command, 0, PATH_MAX);

      while (p < command + PATH_MAX - 1) {
        off_t rr = read(cfd->fd, p, len);
        if (rr <= 0) {
          if (rr == 0 || errno == EAGAIN || errno == EWOULDBLOCK)
            break;
          perror("read");
          std::abort();
        }
        len -= rr;
        p += rr;
      }

      len = PATH_MAX - len;
      if (len > 0 && command[len - 1] == '\n') command[len - 1] = 0;

      bar->Execute(std::string(command));

      struct timeval now;
      int passed;
      gettimeofday(&now, NULL);
      passed = (now.tv_sec - last.tv_sec) * 1000
               + (now.tv_usec - last.tv_usec) / 1000;
      if (passed >= 1000) {
        bar->RefreshPerSecond();
        last = now;
        timeout = 2000 - passed;
      } else {
        bar->Refresh();
        timeout = 1000 - passed;
      }
    }

    if (cfd->revents & POLLHUP) {
      close(cfd->fd);
      OpenFifo(cfd);
    }
  }
}


bool Bar::g_all_screens = false;
bool Bar::g_screen_top = true;
int Bar::g_height = 16;

}

using namespace sysmon;

int main(int argc, char *argv[])
{
  int opt;
  while ((opt = getopt(argc, argv, "ab")) != -1) {
    switch(opt) {
      case 'a':
        Bar::g_all_screens = true;
        break;
      case 'b':
        Bar::g_screen_top = false;
        break;
      default:
        std::exit(-1);
        break;
    }
  }

  {
    std::stringstream ss;
    ss << getenv("HOME") << "/.sys-monitor.pid";
    std::ofstream fout(ss.str());
    fout << getpid();
  }

  MainLoop _;

  const char *dpi_res = XGetDefault(_.display(), "Xft", "dpi");
  if (dpi_res) {
    int dpi = std::atoi(dpi_res);
    if (dpi > 0) {
      RenderContext::g_dpi_scale = dpi / 96.;
      Bar::g_height *= RenderContext::g_dpi_scale;
    }
  }
  Bar *bar = new Bar(_.display());

  bar->Add(Factory<Widget, CpuKind>::Construct(), AlignmentType::Left);
  bar->Add(Factory<Widget, TimeKind>::Construct(), AlignmentType::Right);
  bar->Add(Factory<Widget, VolumeKind>::Construct(), AlignmentType::Right);
  bar->Add(Factory<Widget, BacklightKind>::Construct(), AlignmentType::Right);
  bar->Add(Factory<Widget, MemoryKind>::Construct(), AlignmentType::Right);
  bar->Add(Factory<Widget, BatteryKind>::Construct(), AlignmentType::Right);
  bar->Add(Factory<Widget, NetworkKind>::Construct(), AlignmentType::Right);
  bar->Add(Factory<Widget, StorageKind>::Construct(), AlignmentType::Right);
  bar->Configure();

  _.Run(bar);

  return 0;
}
