#include <fstream>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <cstring>

#include <pulse/pulseaudio.h>

#include "monitor.h"
#include "icons.h"

namespace sysmon {

class DeviceUtil {
 protected:
  bool IsPhysicalDevice(std::string device_class, std::string device_name) {
    std::stringstream path;
    path << "/sys/class/" << device_class << "/" << device_name << "/device";
    if (access(path.str().c_str(), F_OK) < 0) {
      return false;
    }
    return true;
  }
  std::vector<std::string> ListDevices(std::string device_class) {
    std::string path = "/sys/class/" + device_class;
    std::vector<std::string> res;
    DIR *dir = opendir(path.c_str());
    if (!dir) return res;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
      if (ent->d_name[0] == '.') continue;
      res.push_back(std::string(ent->d_name));
    }
    closedir(dir);
    return res;
  }
  std::vector<uint64_t> ReadStat(std::string device_class, std::string device_name, std::string node) {
    std::stringstream path;
    path << "/sys/class/" << device_class << "/" << device_name << "/" << node;
    std::ifstream fin(path.str());
    std::vector<uint64_t> res;
    if (!fin) {
      std::cerr << "Failed reading " << path.str() << std::endl;
      return res;
    }
    uint64_t n;
    while (!fin.eof() && !fin.fail()) {
      fin >> n;
      res.push_back(n);
    }
    return res;
  }

  void WriteStat(std::string device_class, std::string device_name, std::string node, int64_t value) {
    std::stringstream path;
    path << "/sys/class/" << device_class << "/" << device_name << "/" << node;
    std::ofstream fout(path.str());
    fout << value << std::endl;
  }
};

class StringUtils {
 protected:
  std::vector<std::string> Split(std::string str, char sep) {
    size_t start = 0;
    std::vector<std::string> res;
    for (size_t i = 0; i < str.length() + 1; i++) {
      if (i == str.length() || str[i] == sep) {
        if (i > start) res.push_back(str.substr(start, i - start));
        start = i + 1;
      }
    }
    return res;
  }
  std::string Trim(std::string str) {
    int i, j;
    for (i = 0; i < str.length() && str[i] == ' '; i++);
    for (j = str.length() - 1; j >= i && str[j] == ' '; j--);
    return str.substr(i, j - i + 1);
  }

  std::string RemoveUseless(std::string str, std::string useless) {
    std::stringstream ss;
    int start = 0;
    while (true) {
      int pos = str.find(useless, start);
      if (pos != str.npos) {
        ss << str.substr(start, pos - start);
        start = pos + useless.length();
      } else {
        ss << str.substr(start);
        return ss.str();
      }
    }
  }

  bool StartsWith(std::string str, std::string prefix) {
    if (str.length() < prefix.length())
      return false;
    return strncmp(str.c_str(), prefix.c_str(), prefix.length()) == 0;
  }
};

class BaseRateWidget : public Widget {
  std::vector<uint64_t> sums;
 protected:
  std::vector<int64_t> rates;
 public:
  BaseRateWidget(std::vector<uint64_t> sums) : sums(sums) {
    rates.resize(sums.size());
  }

  virtual std::vector<uint64_t> Count() = 0;

  void Refresh() override {};
  void OnAdd(Bar *bar) override {
    bar->RegisterPerSecondRefresh([=]() {
        auto s = Count();
        for (int i = 0; i < s.size(); i++) {
          rates[i] = s[i] - sums[i];
        }
        sums = s;
      });
  }
};


class CpuWidget : public BaseRateWidget, public StringUtils {
  int nr_socks;
  std::string model;
  Pixmap cpu_icon;
 public:
  CpuWidget() : BaseRateWidget(Count()) {
    std::ifstream fin("/proc/cpuinfo");
    for (std::string line; std::getline(fin, line); ) {
      auto arr = Split(line, ':');
      if (arr.size() < 2) continue;
      if (StartsWith(arr[0], "model name"))
        set_model(arr[1]);
      if (StartsWith(arr[0], "physical id"))
        nr_socks = 1 + std::stoi(arr[1]);
    }
  }

  void set_model(std::string str) {
    str = RemoveUseless(str, "Intel(R) Core(TM)");
    str = RemoveUseless(str, "CPU @ ");
    model = Trim(str);
  }

  std::vector<uint64_t> Count() override final {
    std::ifstream fin("/proc/stat");
    std::vector<uint64_t> cnts;
    for (std::string line; std::getline(fin, line); ) {
      if (StartsWith(line, "cpu ")) continue; // skip the overall cpu
      if (!StartsWith(line, "cpu")) continue; // skip non-cpu line
      auto arr = Split(line, ' ');
      size_t cnt = 0;
      for (int i = 1; i < arr.size(); i++) {
        if (i == 4) continue;
        cnt += std::stoll(arr[i]);
      }
      cnts.push_back(cnt);
    }
    return cnts;
  }

  size_t Width() final override {
    return 100 + 50 * rates.size();
  }
  void Render(RenderContext *ctx) final override {
    std::stringstream str;
    str << "CPU: " << nr_socks << "x " << model << "  ";
    for (auto pct: rates) {
     str << pct << "% ";
    }
    ctx->DrawBitmap(this, cpu_icon, 8, 8, 4);
    ctx->DrawText(this, str.str(), 16);
  }
  void OnAdd(Bar *bar) final override {
    BaseRateWidget::OnAdd(bar);
    cpu_icon = bar->LoadBitmap(icons::cpu_bits, 8, 8);
  }
};

template <> Widget *Factory<Widget, CpuKind>::Construct() { return new CpuWidget(); }

class MemoryWidget : public Widget, public StringUtils {
  uint64_t total = 0, free = 0, buffer_cache = 0;
  Pixmap memory_icon;
 public:
  void Refresh() final override {
    std::ifstream fin("/proc/meminfo");
    buffer_cache = 0;
    for (std::string line; std::getline(fin, line); ) {
      if (StartsWith(line, "MemTotal:")) {
        auto vec = Split(line, ' ');
        total = std::stoll(vec[1]);
      } else if (StartsWith(line, "MemFree:")) {
        auto vec = Split(line, ' ');
        free = std::stoll(vec[1]);
      } else if (StartsWith(line, "Cached:") || StartsWith(line, "Buffers:")) {
        auto vec = Split(line, ' ');
        buffer_cache += std::stoll(vec[1]);
      }
    }
  }

  size_t Width() final override { return 120; }
  void Render(RenderContext *ctx) final override {
    int p = (total - free - buffer_cache) * 100 / total;
    int q = buffer_cache * 100 / total;
    int r = free * 100 / total;
    ctx->DrawBitmap(this, memory_icon, 8, 8, 4);

    ctx
        ->SetColor(0x89 << 8, 0x71 << 8, 0xC1 << 8)
        ->DrawBlock(this, 16, p)
        ->SetColor(0x74 << 8, 0xD3 << 8, 0x71 << 8)
        ->DrawBlock(this, 16 + p, q)
        ->SetColor(0x99 << 8, 0x99 << 8, 0x99 << 8)
        ->DrawBlock(this, 16 + p + q, r);

    ctx->ResetColor();
  }
  void OnAdd(Bar *bar) final override {
    memory_icon = bar->LoadBitmap(icons::mem_bits, 8, 8);
  }
};

template <> Widget *Factory<Widget, MemoryKind>::Construct() { return new MemoryWidget(); }

class StorageWidget : public BaseRateWidget, public DeviceUtil {
 public:
  StorageWidget() : BaseRateWidget(Count()) {}
  std::vector<uint64_t> Count() override final {
    auto devices = ListDevices("block");
    std::vector<uint64_t> io(2);

    for (auto dev: devices) {
      if (!IsPhysicalDevice("block", dev)) continue;
      auto vec = ReadStat("block", dev, "stat");
      io[0] += vec[2] / 2;
      io[1] += vec[6] / 2;
    }
    return io;
  }

  size_t Width() final override {
    return 150;
  }
  void Render(RenderContext *ctx) final override {
    {
      std::stringstream str;
      str << "R: " << rates[0] / 1024 << "MB/s";
      ctx->DrawText(this, str.str());
    }
    {
      std::stringstream str;
      str << "W: " << rates[1] / 1024 << "MB/s";
      ctx->DrawText(this, str.str(), 75);
    }
  }
};

template <> Widget *Factory<Widget, StorageKind>::Construct() { return new StorageWidget(); }

class NetworkWidget : public BaseRateWidget, public DeviceUtil {
  Pixmap net_up_icon, net_down_icon;
 public:
  NetworkWidget() : BaseRateWidget(Count()) {}
  std::vector<uint64_t> Count() override final {
    auto devices = ListDevices("net");
    std::vector<uint64_t> net(2);
    for (auto dev: devices) {
      if (!IsPhysicalDevice("net", dev)) continue;
      net[0] += ReadStat("net", dev, "statistics/rx_bytes")[0];
      net[1] += ReadStat("net", dev, "statistics/tx_bytes")[0];
    }
    return net;
  }

  size_t Width() final override {
    return 140;
  }
  void Render(RenderContext *ctx) final override {
    ctx
        ->DrawBitmap(this, net_down_icon, 8, 8, 4)
        ->DrawBitmap(this, net_up_icon, 8, 8, 4 + 70);
    {
      std::stringstream str;
      str << rates[0] / 1024 << "KB/s";
      ctx->DrawText(this, str.str(), 16);
    }
    {
      std::stringstream str;
      str << rates[1] / 1024 << "KB/s";
      ctx->DrawText(this, str.str(), 16 + 70);
    }
  }
  void OnAdd(Bar *bar) final override {
    BaseRateWidget::OnAdd(bar);
    net_up_icon = bar->LoadBitmap(icons::net_up_03_bits, 8, 8);
    net_down_icon = bar->LoadBitmap(icons::net_down_03_bits, 8, 8);
  }
};

template <> Widget *Factory<Widget, NetworkKind>::Construct() { return new NetworkWidget(); }

class BacklightWidget : public Widget, public DeviceUtil, public StringUtils {
  bool enabled;
  bool use_acpi;
  std::string device;
  uint64_t max, value;
  Pixmap backlight_icon;
 public:
  BacklightWidget() {
    auto devices = ListDevices("backlight");
    enabled = devices.size() > 0;
    use_acpi = false;
    if (enabled) {
      for (auto dev: devices) {
        device = dev;
        if (StartsWith(dev, "acpi_video")) {
          use_acpi = true;
          break;
        }
      }
      Refresh();
    }
  }
  void Refresh() override final {
    if (!enabled) return;
    max = ReadStat("backlight", device, "max_brightness")[0];
    value = ReadStat("backlight", device, "brightness")[0];
  }
  size_t Width() final override {
    if (!enabled) return 0;
    return 120;
  }
  void Render(RenderContext *ctx) override final {
    if (!enabled) return;
    int pct = value * 100 / max;
    ctx
        ->DrawBitmap(this, backlight_icon, 9, 9, 4)
        ->SetColor(0xFF << 8, 0xFF << 8, 0xFF << 8)
        ->DrawBlock(this, 16, pct)
        ->SetColor(0x99 << 8, 0x99 << 8, 0x99 << 8)
        ->DrawBlock(this, 16 + pct, 100 - pct)
        ->ResetColor();
  }

  void OnAdd(Bar *bar) override final {
    backlight_icon = bar->LoadBitmap(icons::brightness_bits, 9, 9);
    bar->RegisterCommand(
        "brightness-up",
        [=]() {
          if (!enabled) return;
          if (!use_acpi) {
            WriteStat("backlight", device, "brightness",
                      std::min(max, value + max / 10));
          }
          bar->Refresh();
        });
    bar->RegisterCommand(
        "brightness-down",
        [=]() {
          if (!enabled || use_acpi) return;
          if (!use_acpi) {
            WriteStat("backlight", device, "brightness",
                      std::max((int64_t) 0, (int64_t) (value - max / 10)));
          }
          bar->Refresh();
        });
  }
};

template <> Widget *Factory<Widget, BacklightKind>::Construct() { return new BacklightWidget(); }

class TimeWidget : public Widget {
  struct tm *local;
  Pixmap clock_icon;
 public:
  TimeWidget() {
    Refresh();
  }
  void Refresh() override final {
    time_t t = time(NULL);
    local = localtime(&t);
  }
  size_t Width() override final {
    return 130;
  }
  void Render(RenderContext *ctx) override final {
    char fmt[128];
    strftime(fmt, 128, "%b-%d %a %H:%M", local);
    auto m = std::numeric_limits<unsigned short>::max();
    ctx->SetColor(0, 0, 0);
    ctx->DrawBitmap(this, clock_icon, 8, 8, 4);
    ctx->ResetColor();
    ctx->DrawText(this, std::string(fmt), 16);
  }
  void OnAdd(Bar *bar) final override {
    clock_icon = bar->LoadBitmap(icons::clock_bits, 8, 8);
  }
};

template <> Widget *Factory<Widget, TimeKind>::Construct() { return new TimeWidget(); }

class VolumeWidget : public Widget {
  bool enabled = false;
  pa_cvolume volume;
  pa_mainloop *loop;
  pa_context *ctx;
  Pixmap speaker_icon;
  std::vector<int> sinks;

 public:
  VolumeWidget() {
    loop = pa_mainloop_new();
    auto api = pa_mainloop_get_api(loop);
    ctx = pa_context_new(api, "");
    bool ready = false;

    pa_context_set_state_callback(ctx, [](pa_context *ctx, void *ptr) {
        if (pa_context_get_state(ctx) == PA_CONTEXT_READY) {
          auto ready = (bool *) ptr;
          *ready = true;
        }
      }, &ready);

    if (pa_context_connect(ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
      return;
    }

    while (!ready) {
      int ret;
      pa_mainloop_iterate(loop, 1, &ret);
    }

    Refresh();
  }
  void Refresh() override final {
    // TODO: What to display if there are multiple sinks?
    sinks.clear();
    auto o = pa_context_get_sink_info_list(
        ctx,
        [](pa_context *c, const pa_sink_info *sink, int eol, void *ptr) {
          if (sink == nullptr) {
            return;
          }
          auto w = (VolumeWidget *) ptr;
          w->volume = sink->volume;
          w->enabled = true;
          // printf("%s\n", sink->name);
          w->sinks.push_back(sink->index);
        }, this);

    if (!o) {
      puts("Error");
      return;
    }
    while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
      int ret;
      pa_mainloop_iterate(loop, 1, &ret);
    }
    pa_operation_unref(o);
  }
  void SetVolume() {
    if (!enabled) return;

    for (auto sink_id: sinks) {
      auto o = pa_context_set_sink_volume_by_index(
          ctx, sink_id, &volume,
          [](pa_context *ctx, int success, void *ptr) {}, this);

      if (!o)
        return;
      while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
        int ret;
        pa_mainloop_iterate(loop, 1, &ret);
      }
      pa_operation_unref(o);
    }
  }

  void OnAdd(Bar *bar) override final {
    speaker_icon = bar->LoadBitmap(icons::spkr_01_bits, 8, 8);
    bar->RegisterCommand(
        "vol-up",
        [=]() {
          pa_cvolume_inc_clamp(&volume, PA_VOLUME_NORM / 10, PA_VOLUME_NORM);
          SetVolume();
        });
    bar->RegisterCommand(
        "vol-down",
        [=]() {
          pa_cvolume_dec(&volume, PA_VOLUME_NORM / 10);
          SetVolume();
        });
  }

  size_t Width() override final {
    return enabled ? 120 : 0;
  }
  void Render(RenderContext *ctx) override final {
    if (!enabled) return;
    uint64_t s = 0;
    for (int i = 0; i < volume.channels; i++) {
      s += volume.values[i];
    }
    int pct = s * 100 / volume.channels / PA_VOLUME_NORM;
    ctx
        ->DrawBitmap(this, speaker_icon, 8, 8, 4)
        ->SetColor(0xFF << 8, 0xFF << 8, 0xFF << 8)
        ->DrawBlock(this, 16, pct)
        ->SetColor(0x99 << 8, 0x99 << 8, 0x99 << 8)
        ->DrawBlock(this, 16 + pct, 100 - pct)
        ->ResetColor();
  }
};

template <> Widget *Factory<Widget, VolumeKind>::Construct() { return new VolumeWidget(); }

}
