#include <libusb.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
constexpr std::uint16_t kVendorId = 0x08f2;
constexpr std::uint16_t kProductId = 0x6811;
constexpr std::array<unsigned char, 8> kFullAreaReport{
    0x08, 0x03, 0x00, 0xff, 0xf0, 0x00, 0xff, 0xf0};
constexpr int kReportSize = 64;
// Raw pressure is inverted on this tablet: hover is around 1415-1419 and
// firmer contact moves toward 881. Use separate contact/release thresholds
// so small sensor fluctuations near the edge do not repeatedly lift the pen.
constexpr int kPressureTouchThreshold = 1400;
constexpr int kPressureReleaseThreshold = 1410;
constexpr int kPressureFullOutputRaw = 881;
constexpr int kVirtualPressureMaximum = 5000;
std::atomic_bool running{true};

void handle_signal(int) { running = false; }

void check_libusb(int result, const std::string& action) {
  if (result < 0) {
    throw std::runtime_error(action + ": " + libusb_error_name(result));
  }
}

void set_bit(int fd, unsigned long request, unsigned int bit,
             const char* description) {
  if (ioctl(fd, request, bit) < 0) {
    throw std::runtime_error(std::string("uinput: ") + description + ": " +
                             std::strerror(errno));
  }
}

class VirtualTablet {
 public:
  VirtualTablet() {
    fd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
      throw std::runtime_error(std::string("open /dev/uinput: ") +
                               std::strerror(errno));
    }

    try {
      set_bit(fd_, UI_SET_EVBIT, EV_KEY, "enable key events");
      set_bit(fd_, UI_SET_EVBIT, EV_ABS, "enable absolute events");
      set_bit(fd_, UI_SET_KEYBIT, BTN_TOOL_PEN, "enable pen proximity");
      set_bit(fd_, UI_SET_KEYBIT, BTN_TOUCH, "enable pen tip");
      set_bit(fd_, UI_SET_KEYBIT, BTN_STYLUS, "enable stylus button");
      set_bit(fd_, UI_SET_KEYBIT, BTN_STYLUS2, "enable second stylus button");
      for (int key : tablet_keys_) {
        set_bit(fd_, UI_SET_KEYBIT, static_cast<unsigned int>(key),
                "enable tablet shortcut key");
      }

      setup_axis(ABS_X, 0, 4096, 1);
      setup_axis(ABS_Y, 0, 4096, 1);
      setup_axis(ABS_PRESSURE, 0, 5000, 1);

      uinput_setup setup{};
      setup.id.bustype = BUS_USB;
      setup.id.vendor = kVendorId;
      setup.id.product = kProductId;
      setup.id.version = 1;
      std::strncpy(setup.name, "ProOne PDT 6002 Full Area Tablet",
                   UINPUT_MAX_NAME_SIZE - 1);
      if (ioctl(fd_, UI_DEV_SETUP, &setup) < 0 ||
          ioctl(fd_, UI_DEV_CREATE) < 0) {
        throw std::runtime_error(std::string("create virtual tablet: ") +
                                 std::strerror(errno));
      }
      created_ = true;
    } catch (...) {
      close(fd_);
      fd_ = -1;
      throw;
    }
  }

  ~VirtualTablet() {
    if (fd_ >= 0) {
      if (created_) ioctl(fd_, UI_DEV_DESTROY);
      close(fd_);
    }
  }

  void emit(int type, int code, int value) {
    if (event_count_ == events_.size()) {
      throw std::runtime_error("too many events in one tablet report");
    }
    input_event event{};
    event.type = static_cast<__u16>(type);
    event.code = static_cast<__u16>(code);
    event.value = value;
    events_[event_count_++] = event;
  }

  void sync() {
    emit(EV_SYN, SYN_REPORT, 0);
    const std::size_t bytes = event_count_ * sizeof(input_event);
    const ssize_t written = write(fd_, events_.data(), bytes);
    if (written < 0) {
      throw std::runtime_error(std::string("write uinput event: ") +
                               std::strerror(errno));
    }
    if (written != static_cast<ssize_t>(bytes)) {
      throw std::runtime_error("short write while sending tablet event batch");
    }
    event_count_ = 0;
  }

 private:
  static constexpr std::size_t kMaxEventsPerReport = 32;
  static constexpr std::array<int, 12> tablet_keys_{
      KEY_TAB, KEY_SPACE, KEY_LEFTALT, KEY_LEFTCTRL, KEY_SCROLLDOWN,
      KEY_SCROLLUP, KEY_LEFTBRACE, KEY_KPMINUS, KEY_KPPLUS, KEY_E,
      KEY_B, KEY_RIGHTBRACE};

  void setup_axis(int code, int minimum, int maximum, int resolution) {
    set_bit(fd_, UI_SET_ABSBIT, static_cast<unsigned int>(code),
            "enable absolute axis");
    uinput_abs_setup axis{};
    axis.code = static_cast<__u16>(code);
    axis.absinfo.minimum = minimum;
    axis.absinfo.maximum = maximum;
    axis.absinfo.resolution = resolution;
    if (ioctl(fd_, UI_ABS_SETUP, &axis) < 0) {
      throw std::runtime_error(std::string("configure absolute axis: ") +
                               std::strerror(errno));
    }
  }

  int fd_{-1};
  bool created_{false};
  bool previous_touch_{false};
  std::array<input_event, kMaxEventsPerReport> events_{};
  std::size_t event_count_{0};
};

class TabletUsb {
 public:
  TabletUsb() {
    check_libusb(libusb_init(&context_), "initialize libusb");
    handle_ = libusb_open_device_with_vid_pid(context_, kVendorId, kProductId);
    if (!handle_) {
      libusb_exit(context_);
      context_ = nullptr;
      throw std::runtime_error("could not find USB tablet 08f2:6811");
    }
    check_libusb(libusb_set_auto_detach_kernel_driver(handle_, 1),
                 "enable automatic kernel-driver detach");
    claim_hid_interfaces_and_find_endpoint();

    const int sent = libusb_control_transfer(
        handle_, 0x21, 0x09, 0x0308, 2,
        const_cast<unsigned char*>(kFullAreaReport.data()),
        static_cast<std::uint16_t>(kFullAreaReport.size()), 250);
    check_libusb(sent, "send full-area mode report");
    if (sent != static_cast<int>(kFullAreaReport.size())) {
      throw std::runtime_error("tablet accepted only part of the mode report");
    }
  }

  ~TabletUsb() {
    if (handle_) {
      for (int interface_number : claimed_interfaces_) {
        libusb_release_interface(handle_, interface_number);
      }
      libusb_close(handle_);
    }
    if (context_) libusb_exit(context_);
  }

  bool read_report(std::array<unsigned char, kReportSize>& report) {
    int transferred = 0;
    const int result = libusb_interrupt_transfer(
        handle_, endpoint_, report.data(), static_cast<int>(report.size()),
        &transferred, 1000);
    if (result == LIBUSB_ERROR_TIMEOUT) return false;
    check_libusb(result, "read tablet report");
    if (transferred < 13) {
      throw std::runtime_error("tablet sent a short report");
    }
    return true;
  }

 private:
  void claim_hid_interfaces_and_find_endpoint() {
    libusb_device* device = libusb_get_device(handle_);
    libusb_config_descriptor* config = nullptr;
    check_libusb(libusb_get_active_config_descriptor(device, &config),
                 "read USB configuration");
    bool found_endpoint = false;
    try {
      for (std::uint8_t i = 0; i < config->bNumInterfaces; ++i) {
        const libusb_interface& interface = config->interface[i];
        for (int alternate = 0; alternate < interface.num_altsetting;
             ++alternate) {
          const libusb_interface_descriptor& descriptor =
              interface.altsetting[alternate];
          if (descriptor.bInterfaceClass != LIBUSB_CLASS_HID) continue;
          const int claim = libusb_claim_interface(handle_,
                                                   descriptor.bInterfaceNumber);
          check_libusb(claim, "claim tablet HID interface");
          claimed_interfaces_.push_back(descriptor.bInterfaceNumber);
          for (std::uint8_t e = 0; e < descriptor.bNumEndpoints; ++e) {
            const libusb_endpoint_descriptor& endpoint = descriptor.endpoint[e];
            if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) ==
                    LIBUSB_ENDPOINT_IN &&
                (endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) ==
                    LIBUSB_TRANSFER_TYPE_INTERRUPT &&
                endpoint.wMaxPacketSize >= kReportSize) {
              endpoint_ = endpoint.bEndpointAddress;
              found_endpoint = true;
            }
          }
        }
      }
      libusb_free_config_descriptor(config);
    } catch (...) {
      libusb_free_config_descriptor(config);
      throw;
    }
    if (!found_endpoint) {
      throw std::runtime_error("no 64-byte interrupt IN endpoint found");
    }
  }

  libusb_context* context_{nullptr};
  libusb_device_handle* handle_{nullptr};
  unsigned char endpoint_{0};
  std::vector<int> claimed_interfaces_;
};

class Driver {
 public:
  explicit Driver(bool debug) : debug_(debug) {}

  void run() {
    TabletUsb tablet;
    VirtualTablet virtual_tablet;
    std::cout << "Full-area mode enabled for USB tablet 08f2:6811.\n"
              << "Reading pen reports; press Ctrl+C to stop.\n";
    if (debug_) {
      std::cout << "Debug packet logging enabled.\n";
    }

    std::array<unsigned char, kReportSize> report{};
    std::size_t packet_count = 0;
    while (running) {
      if (tablet.read_report(report)) {
        ++packet_count;
        if (debug_ && (packet_count <= 5 || packet_count % 200 == 0)) {
          std::cerr << "report " << packet_count << ":";
          for (std::size_t i = 0; i < 16; ++i) {
            std::cerr << ' ' << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(report[i]);
          }
          std::cerr << std::dec << std::setfill(' ') << "  x="
                    << word(report, 1, 2) << " y=" << word(report, 3, 4)
                    << " raw_pressure=" << word(report, 5, 6)
                    << " pen_buttons=" << static_cast<unsigned int>(report[9])
                    << '\n';
        }
        dispatch(report, virtual_tablet);
      } else if (debug_ && packet_count == 0) {
        std::cerr << "No report received yet; move the pen over the tablet.\n";
      }
    }
    std::cout << "Stopped after receiving " << packet_count << " reports.\n";
  }

 private:
  static int word(const std::array<unsigned char, kReportSize>& report,
                  std::size_t high, std::size_t low) {
    return (static_cast<int>(report[high]) << 8) | report[low];
  }

  static int pressure(int raw) {
    const int raw_range = kPressureTouchThreshold - kPressureFullOutputRaw;
    const double normalized = std::clamp(
        static_cast<double>(kPressureTouchThreshold - raw) / raw_range, 0.0,
        1.0);
    const double curve = normalized;
    const int scaled = static_cast<int>(curve * kVirtualPressureMaximum);
    return std::clamp(scaled, 0, kVirtualPressureMaximum);
  }

  void dispatch(const std::array<unsigned char, kReportSize>& report,
                VirtualTablet& output) {
    const int x = word(report, 1, 2);
    const int y = word(report, 3, 4);
    const int raw_pressure = word(report, 5, 6);

    if (in_contact_) {
      if (raw_pressure >= kPressureReleaseThreshold) in_contact_ = false;
    } else if (raw_pressure <= kPressureTouchThreshold) {
      in_contact_ = true;
    }

    int normalized_pressure = 0;
    if (in_contact_) {
      const int target_pressure = std::max(1, pressure(raw_pressure));
      // A light one-pole filter smooths raw sensor jitter while preserving
      // quick response. Keep at least 1 while in contact so BTN_TOUCH agrees.
      filtered_pressure_ += (target_pressure - filtered_pressure_) / 2;
      normalized_pressure = std::max(1, filtered_pressure_);
    } else {
      filtered_pressure_ = 0;
    }

    output.emit(EV_ABS, ABS_X, std::clamp(x, 0, 4096));
    output.emit(EV_ABS, ABS_Y, std::clamp(y, 0, 4096));
    output.emit(EV_ABS, ABS_PRESSURE, normalized_pressure);

    const bool touching = in_contact_;
    if (touching != previous_touch_) {
      output.emit(EV_KEY, BTN_TOUCH, touching ? 1 : 0);
      previous_touch_ = touching;
    }

    const bool in_proximity = touching || report[9] != 0;
    if (in_proximity != previous_proximity_) {
      output.emit(EV_KEY, BTN_TOOL_PEN, in_proximity ? 1 : 0);
      previous_proximity_ = in_proximity;
    }

    emit_pen_button(output, BTN_STYLUS, report[9] == 4 || report[9] == 6,
                    previous_stylus_);
    emit_pen_button(output, BTN_STYLUS2, report[9] == 6, previous_stylus2_);
    emit_tablet_buttons(report, output);
    output.sync();
  }

  static void emit_pen_button(VirtualTablet& output, int key, bool pressed,
                              bool& previous) {
    if (pressed != previous) output.emit(EV_KEY, key, pressed ? 1 : 0);
    previous = pressed;
  }

  void emit_tablet_buttons(
      const std::array<unsigned char, kReportSize>& report,
      VirtualTablet& output) {
    const unsigned int flags =
        static_cast<unsigned int>(word(report, 12, 11)) | 0xcc00u;
    constexpr std::array<int, 14> keys{
        KEY_TAB, KEY_SPACE, KEY_LEFTALT, KEY_LEFTCTRL, KEY_SCROLLDOWN,
        KEY_SCROLLUP, KEY_LEFTBRACE, KEY_KPMINUS, KEY_KPPLUS, KEY_E,
        KEY_RESERVED, KEY_RESERVED, KEY_B, KEY_RIGHTBRACE};
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (keys[i] == KEY_RESERVED) continue;
      const bool pressed = (flags & (1u << i)) == 0;
      if (pressed != previous_buttons_[i]) {
        output.emit(EV_KEY, keys[i], pressed ? 1 : 0);
        previous_buttons_[i] = pressed;
      }
    }
  }

  std::array<bool, 14> previous_buttons_{};
  bool previous_touch_{false};
  bool in_contact_{false};
  int filtered_pressure_{0};
  bool previous_proximity_{false};
  bool previous_stylus_{false};
  bool previous_stylus2_{false};
  bool debug_{false};
};
}  // namespace

int main(int argc, char* argv[]) {
  bool debug = false;
  if (argc == 2 && std::string(argv[1]) == "--debug") {
    debug = true;
  } else if (argc != 1) {
    std::cerr << "Usage: proone-tablet-daemon [--debug]\n";
    return 2;
  }
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  try {
    Driver{debug}.run();
  } catch (const std::exception& error) {
    std::cerr << "proone-tablet-daemon: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
