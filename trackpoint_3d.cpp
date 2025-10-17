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
#include <fstream>
#include <filesystem>
#include <sstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <sys/stat.h>
#include <limits.h>
#include <cerrno>
#include <sys/wait.h>

namespace {
constexpr int VENDOR_ID = 0x046D;
constexpr int PRODUCT_ID = 0xC603;
constexpr int AXIS_MIN = -5000;
constexpr int AXIS_MAX = 5000;
constexpr int DEADZONE = 2;
constexpr double DEFAULT_GAIN = 60.0;
constexpr int DEFAULT_HOTKEY = KEY_F8;

constexpr const char* DEFAULT_ENV_DIR = "/etc/trackpoint-3d";
constexpr const char* DEFAULT_ENV_FILE = "trackpoint-3d.env";
constexpr const char* DEFAULT_SERVICE_NAME = "trackpoint-3d";

const int ALL_AXES[6] = {ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ};

struct Args {
    std::string tp_path;
    std::string kbd_path;
    double gain = DEFAULT_GAIN;
    int hotkey = DEFAULT_HOTKEY;
    bool install = false;
    std::string install_path = "/usr/local/bin/trackpoint-3d";
    std::string service_name = DEFAULT_SERVICE_NAME;
    std::string env_dir = DEFAULT_ENV_DIR;
};

static bool g_show_install = true;

void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --tp <trackpoint_event> --kbd <keyboard_event> [options]\n\n"
              << "Options:\n"
              << "  --gain <float>         Scale factor for deltas (default 60)\n"
              << "  --hotkey <keycode>     EV_KEY code to toggle grab (default KEY_F8)\n"
              << (g_show_install ? "\nInstall (run as root):\n  --install              Install binary + systemd service (one-time)\n  --install-path <path>  Install binary path (default /usr/local/bin/trackpoint-3d)\n  --service-name <name>  Systemd unit base name (default trackpoint-3d)\n  --env-dir <dir>        Directory for .env file (default /etc/trackpoint-3d)\n" : "")
              << std::endl;
    std::exit(EXIT_FAILURE);
}

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
        } else if (arg == "--install") {
            a.install = true;
        } else if (arg == "--install-path" && i + 1 < argc) {
            a.install_path = argv[++i];
        } else if (arg == "--service-name" && i + 1 < argc) {
            a.service_name = argv[++i];
        } else if (arg == "--env-dir" && i + 1 < argc) {
            a.env_dir = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
        } else {
            usage(argv[0]);
        }
    }
    return a;
}

std::string read_self_path();

static bool run_cmd_ok(const std::string& cmd, const char* action) {
    int rc = system(cmd.c_str());
    if (rc == -1) {
        perror(action);
        return false;
    }
    if (WIFEXITED(rc)) {
        int code = WEXITSTATUS(rc);
        if (code == 0) return true;
        std::cerr << action << " failed with exit code " << code << std::endl;
        return false;
    }
    if (WIFSIGNALED(rc)) {
        std::cerr << action << " terminated by signal " << WTERMSIG(rc) << std::endl;
        return false;
    }
    std::cerr << action << " failed with status " << rc << std::endl;
    return false;
}

static bool is_installed_copy(const std::string& service_name) {
    namespace fs = std::filesystem;
    std::string unit_path = "/etc/systemd/system/" + service_name + ".service";
    if (!fs::exists(unit_path)) return false;
    std::ifstream in(unit_path);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();
    std::string self = read_self_path();
    return content.find(self) != std::string::npos;
}

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

int clamp(int v) { return std::max(AXIS_MIN, std::min(AXIS_MAX, v)); }

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

void zero_all_axes(int ufd) {
    for (int i = 0; i < 6; ++i) {
        emit(ufd, EV_ABS, ALL_AXES[i], 0, false);
    }
    emit(ufd, EV_SYN, SYN_REPORT, 0, true);
}

std::atomic<bool> running{true};

void sigint_handler(int) { running = false; }

enum class Mode { ORBIT, TILT, PAN };

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

std::string read_self_path() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) {
        perror("readlink /proc/self/exe");
        std::exit(EXIT_FAILURE);
    }
    buf[n] = '\0';
    return std::string(buf);
}


}

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    g_show_install = !is_installed_copy(DEFAULT_SERVICE_NAME);
    Args args = parse_args(argc, argv);

    if (args.install) {
        if (!g_show_install) {
            std::cerr << "install option is not available for the installed binary" << std::endl;
            usage(argv[0]);
        }
        if (geteuid() != 0) { std::cerr << "install requires root" << std::endl; return EXIT_FAILURE; }
        if (args.tp_path.empty() || args.kbd_path.empty()) {
            std::cerr << "--install requires --tp and --kbd" << std::endl;
            return EXIT_FAILURE;
        }
        if (!fs::exists(args.tp_path)) { std::cerr << "tp path not found: " << args.tp_path << std::endl; return EXIT_FAILURE; }
        if (!fs::exists(args.kbd_path)) { std::cerr << "kbd path not found: " << args.kbd_path << std::endl; return EXIT_FAILURE; }
        if (!run_cmd_ok("command -v systemctl >/dev/null 2>&1", "systemctl availability check")) {
            std::cerr << "systemctl not available; systemd required for --install" << std::endl;
            return EXIT_FAILURE;
        }
        std::string unit_path = "/etc/systemd/system/" + args.service_name + ".service";
        if (fs::exists(unit_path)) {
            std::cerr << "already installed: " << unit_path << " exists; refusing to reinstall" << std::endl;
            std::cerr << "edit the env file and restart the service if you need changes." << std::endl;
            return EXIT_FAILURE;
        }
        std::string self = read_self_path();
        try {
            if (self != args.install_path) {
                fs::create_directories(fs::path(args.install_path).parent_path());
                fs::copy_file(self, args.install_path, fs::copy_options::overwrite_existing);
                if (chmod(args.install_path.c_str(), 0755) != 0) { perror("chmod install_path"); return EXIT_FAILURE; }
            }
            fs::create_directories(args.env_dir);
        } catch (const fs::filesystem_error& e) {
            std::cerr << "filesystem error: " << e.what() << std::endl;
            return EXIT_FAILURE;
        }
        std::string env_path = args.env_dir + std::string("/") + DEFAULT_ENV_FILE;
        {
            std::ofstream ef(env_path, std::ios::out | std::ios::trunc);
            if (!ef) { std::cerr << "failed to write env file: " << env_path << std::endl; return EXIT_FAILURE; }
            ef << "TP_EVENT=" << args.tp_path << "\n";
            ef << "KBD_EVENT=" << args.kbd_path << "\n";
            ef << "GAIN=" << args.gain << "\n";
            ef << "HOTKEY=" << args.hotkey << "\n";
            ef.flush();
            if (!ef) { std::cerr << "failed to flush env file: " << env_path << std::endl; return EXIT_FAILURE; }
        }
        if (chmod(env_path.c_str(), 0644) != 0) { perror("chmod env file"); return EXIT_FAILURE; }
        {
            fs::create_directories(fs::path(unit_path).parent_path());
            std::ofstream uf(unit_path, std::ios::out | std::ios::trunc);
            if (!uf) { std::cerr << "failed to write unit file: " << unit_path << std::endl; return EXIT_FAILURE; }
            uf << "[Unit]\n";
            uf << "Description=TrackPoint 3D mouse emulation\n";
            uf << "After=local-fs.target\n";
            uf << "ConditionPathExists=/dev/uinput\n\n";
            uf << "[Service]\n";
            uf << "Type=simple\n";
            uf << "EnvironmentFile=" << env_path << "\n";
            uf << "ExecStart=/bin/sh -c 'exec \"" << args.install_path
               << "\" --tp \"${TP_EVENT}\" --kbd \"${KBD_EVENT}\" ${GAIN:+--gain \"${GAIN}\"} ${HOTKEY:+--hotkey \"${HOTKEY}\"}'\n";
            uf << "Restart=on-failure\n";
            uf << "RestartSec=2s\n\n";
            uf << "[Install]\n";
            uf << "WantedBy=multi-user.target\n";
            uf.flush();
            if (!uf) { std::cerr << "failed to flush unit file: " << unit_path << std::endl; return EXIT_FAILURE; }
        }
        if (!run_cmd_ok("systemctl daemon-reload", "systemctl daemon-reload")) return EXIT_FAILURE;
        std::string en_cmd = "systemctl enable " + args.service_name + ".service";
        std::string rs_cmd = "systemctl restart " + args.service_name + ".service";
        if (!run_cmd_ok(en_cmd, "systemctl enable")) return EXIT_FAILURE;
        if (!run_cmd_ok(rs_cmd, "systemctl restart")) return EXIT_FAILURE;
        std::cout << "Service installed and started." << std::endl;
        return 0;
    }

    if (args.tp_path.empty() || args.kbd_path.empty()) {
        usage(argv[0]);
    }
    if (geteuid() != 0) {
        std::cerr << "run as root" << std::endl;
        return EXIT_FAILURE;
    }

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    int ufd = setup_uinput();

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
