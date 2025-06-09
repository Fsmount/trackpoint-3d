// emulate a 3d mouse using a trackpoint and keyboard
// feeds synthetic input to spacenavd via uinput

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <libevdev/libevdev.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {
constexpr int VENDOR_ID = 0x046D; // id from spacenavd source code
constexpr int PRODUCT_ID = 0xC603; // id from spacenavd source code
constexpr int AXIS_MIN = -5000;
constexpr int AXIS_MAX = 5000;
constexpr int DEADZONE = 2; // small motion ignored
constexpr double DEFAULT_GAIN = 60.0; // speed multiplier
constexpr int DEFAULT_HOTKEY = KEY_F8; // default key to toggle grabbing

const int ALL_AXES[6] = {ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ};

// struct to hold input args
struct Args {
    std::string tp_path; // trackpoint event file
    std::string kbd_path; // keyboard event file
    double gain = DEFAULT_GAIN;
    int hotkey = DEFAULT_HOTKEY;
};

// print usage and exit
void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --tp <trackpoint_event> --kbd <keyboard_event> [options]\n\n"
              << "Options:\n"
              << "  --gain <float>         Scale factor for deltas (default 60)\n"
              << "  --hotkey <keycode>     EV_KEY code to toggle grab (default KEY_F8)\n"
              << std::endl;
    std::exit(EXIT_FAILURE);
}

// parse command line args
Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--tp" && i + 1 < argc) {
            a.tp_path = argv[++i];
        } else if (arg == "--kbd" && i + 1 < argc) {
            a.kbd_path = argv[++i];
        } else if (arg == "--gain" && i + 1 < argc) {
            a.gain = std::stod(argv[++i]);
        } else if (arg == "--hotkey" && i + 1 < argc) {
            a.hotkey = std::stoi(argv[++i]);
        } else {
            usage(argv[0]);
        }
    }
    if (a.tp_path.empty() || a.kbd_path.empty()) {
        usage(argv[0]);
    }
    return a;
}

// emit a single input event to uinput
void emit(int fd, uint16_t type, uint16_t code, int32_t value, bool syn = true) {
    input_event ev{};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) < 0) {
        perror("write event");
    }
    if (syn) {
        input_event se{};
        se.type = EV_SYN;
        se.code = SYN_REPORT;
        se.value = 0;
        if (write(fd, &se, sizeof(se)) < 0) {
            perror("write syn");
        }
    }
}

// clamp value to axis bounds
int clamp(int v) { return std::max(AXIS_MIN, std::min(AXIS_MAX, v)); }

// setup the fake input device
int setup_uinput() {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open /dev/uinput");
        std::exit(EXIT_FAILURE);
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_0);
    ioctl(fd, UI_SET_KEYBIT, BTN_1);

    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    for (int axis : ALL_AXES) {
        ioctl(fd, UI_SET_ABSBIT, axis);
    }

    uinput_user_dev uidev{};
    std::snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "TrackPoint-3DMouse");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor = VENDOR_ID;
    uidev.id.product = PRODUCT_ID;
    uidev.id.version = 1;

    for (int axis : ALL_AXES) {
        uidev.absmin[axis] = AXIS_MIN;
        uidev.absmax[axis] = AXIS_MAX;
        uidev.absfuzz[axis] = 0;
        uidev.absflat[axis] = 0;
    }

    if (write(fd, &uidev, sizeof(uidev)) < 0) {
        perror("write uidev");
        std::exit(EXIT_FAILURE);
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE");
        std::exit(EXIT_FAILURE);
    }
    return fd;
}

// reset all axes to 0
void zero_all_axes(int ufd) {
    for (int i = 0; i < 6; ++i) {
        emit(ufd, EV_ABS, ALL_AXES[i], 0, false);
    }
    emit(ufd, EV_SYN, SYN_REPORT, 0, true);
}

std::atomic<bool> running{true}; // stops threads on exit

// signal handler to stop
void sigint_handler(int) { running = false; }

// control modes
enum class Mode { ORBIT, TILT, PAN };

// turn mode enum into name
std::string mode_name(Mode m) {
    switch (m) {
        case Mode::TILT:
            return "tilt";
        case Mode::PAN:
            return "pan";
        default:
            return "orbit";
    }
}

} // end namespace

int main(int argc, char** argv) {
    if (geteuid() != 0) {
        std::cerr << "run as root" << std::endl;
        return EXIT_FAILURE;
    }

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    Args args = parse_args(argc, argv);
    int ufd = setup_uinput();

    // open a device for reading
    auto open_evdev = [](const std::string& path) {
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            perror("open evdev");
            std::exit(EXIT_FAILURE);
        }
        libevdev* dev = nullptr;
        if (libevdev_new_from_fd(fd, &dev) < 0) {
            std::cerr << "failed to init libevdev on " << path << std::endl;
            std::exit(EXIT_FAILURE);
        }
        return dev;
    };

    libevdev* tp_dev = open_evdev(args.tp_path);
    libevdev* kbd_dev = open_evdev(args.kbd_path);

    std::unordered_set<int> keys_down;
    std::mutex keys_mtx;
    bool grabbed = false;
    Mode last_mode = Mode::ORBIT;

    // thread to track keyboard input and toggle grabbing
    std::thread kbd_thread([&] {
        input_event ev;
        while (running) {
            int rc = libevdev_next_event(kbd_dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            if (rc == 0 && ev.type == EV_KEY) {
                std::lock_guard<std::mutex> lock(keys_mtx);
                if (ev.value)
                    keys_down.insert(ev.code);
                else
                    keys_down.erase(ev.code);

                if (ev.code == args.hotkey && ev.value == 1) {
                    if (grabbed) {
                        libevdev_grab(tp_dev, LIBEVDEV_UNGRAB);
                        grabbed = false;
                        zero_all_axes(ufd);
                        std::cout << "[toggle] OFF" << std::endl;
                    } else {
                        libevdev_grab(tp_dev, LIBEVDEV_GRAB);
                        grabbed = true;
                        std::cout << "[toggle] ON" << std::endl;
                    }
                }
            } else if (rc == -EAGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    });

    input_event ev;
    while (running) {
        int rc = libevdev_next_event(tp_dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        if (rc == 0 && grabbed && ev.type == EV_REL) {
            int dx = (ev.code == REL_X) ? ev.value : 0;
            int dy = (ev.code == REL_Y) ? ev.value : 0;

            if (std::abs(dx) < DEADZONE) dx = 0;
            if (std::abs(dy) < DEADZONE) dy = 0;
            if (dx == 0 && dy == 0) continue;

            // diagonal smoothing
            if (dx && dy) {
                double scale = std::max(std::abs(dx), std::abs(dy)) /
                                ((std::abs(dx) + std::abs(dy)) / std::sqrt(2.0));
                dx = static_cast<int>(dx * scale);
                dy = static_cast<int>(dy * scale);
            }

            int sx = static_cast<int>(dx * args.gain);
            int sy = static_cast<int>(dy * args.gain);

            bool shift = false, ctrl = false;
            {
                std::lock_guard<std::mutex> lock(keys_mtx);
                shift = keys_down.count(KEY_LEFTSHIFT) || keys_down.count(KEY_RIGHTSHIFT);
                ctrl = keys_down.count(KEY_LEFTCTRL) || keys_down.count(KEY_RIGHTCTRL);
            }

            Mode mode = Mode::ORBIT;
            if (shift)
                mode = Mode::TILT;
            else if (ctrl)
                mode = Mode::PAN;

            if (mode != last_mode) {
                zero_all_axes(ufd);
                std::cout << "[mode]: " << mode_name(mode) << std::endl;
                last_mode = mode;
            }

            if (mode == Mode::TILT) {
                emit(ufd, EV_ABS, ABS_RY, clamp(-sx), false);
                emit(ufd, EV_ABS, ABS_Y, clamp(-sy));
            } else if (mode == Mode::PAN) {
                emit(ufd, EV_ABS, ABS_X, clamp(sx), false);
                emit(ufd, EV_ABS, ABS_Z, clamp(-sy));
            } else { // ORBIT
                emit(ufd, EV_ABS, ABS_RZ, clamp(-sx), false);
                emit(ufd, EV_ABS, ABS_RX, clamp(-sy));
            }
        } else if (rc == -EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    libevdev_grab(tp_dev, LIBEVDEV_UNGRAB);
    zero_all_axes(ufd);

    ioctl(ufd, UI_DEV_DESTROY);
    close(ufd);

    kbd_thread.join();
    libevdev_free(tp_dev);
    libevdev_free(kbd_dev);

    return 0;
}
