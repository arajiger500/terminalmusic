#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <ncurses.h>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

constexpr const char* IPC_SOCKET = "/tmp/terminalmusic-mpv.sock";
constexpr int DEFAULT_VOLUME = 70;
constexpr int VOLUME_STEP = 5;
constexpr int SEEK_STEP = 5;

struct PlayerState {
    bool connected = false;
    bool paused = false;
    double position = 0.0;
    double duration = 0.0;
    int volume = DEFAULT_VOLUME;
    std::string title;
};

bool file_exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

std::vector<std::string> get_songs(const fs::path& folder) {
    std::vector<std::string> songs;
    std::error_code ec;

    if (!fs::is_directory(folder, ec)) {
        return songs;
    }

    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        const auto ext = entry.path().extension().string();
        std::string lower = ext;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (lower == ".mp3" || lower == ".m4a" || lower == ".flac" ||
            lower == ".ogg" || lower == ".wav" || lower == ".opus") {
            songs.push_back(entry.path().filename().string());
        }
    }

    std::sort(songs.begin(), songs.end(), [](const std::string& a, const std::string& b) {
        return std::lexicographical_compare(
            a.begin(), a.end(), b.begin(), b.end(),
            [](unsigned char x, unsigned char y) {
                return std::tolower(x) < std::tolower(y);
            });
    });

    return songs;
}

class MpvPlayer {
public:
    ~MpvPlayer() {
        stop();
    }

    bool start() {
        stop();

        ::unlink(IPC_SOCKET);

        const std::string command =
            "mpv --no-video --idle=yes --quiet --no-terminal "
            "--input-ipc-server=" + std::string(IPC_SOCKET) + " >/dev/null 2>&1 &";

        if (std::system(command.c_str()) != 0) {
            return false;
        }

        for (int attempt = 0; attempt < 30; ++attempt) {
            if (connect_socket()) {
                send_command("set_property", "volume", std::to_string(DEFAULT_VOLUME));
                return true;
            }
            std::this_thread::sleep_for(50ms);
        }

        return false;
    }

    void stop() {
        if (socket_fd_ != -1) {
            send_raw("{\"command\":[\"quit\"]}\n");
            ::close(socket_fd_);
            socket_fd_ = -1;
        }

        // Only remove our IPC socket. Do not kill arbitrary mpv processes.
        ::unlink(IPC_SOCKET);
    }

    bool is_connected() const {
        return socket_fd_ != -1;
    }

    bool load(const fs::path& path) {
        if (!is_connected()) return false;

        const std::string escaped = json_escape(path.string());
        return send_raw("{\"command\":[\"loadfile\",\"" + escaped + "\",\"replace\"]}\n");
    }

    bool toggle_pause() {
        return send_raw("{\"command\":[\"cycle\",\"pause\"]}\n");
    }

    bool seek(double seconds) {
        return send_raw("{\"command\":[\"seek\"," + std::to_string(seconds) + ",\"relative\"]}\n");
    }

    bool set_volume(int volume) {
        volume = std::clamp(volume, 0, 150);
        return send_raw("{\"command\":[\"set_property\",\"volume\"," + std::to_string(volume) + "]}\n");
    }

    PlayerState query_state(PlayerState state) {
        if (!is_connected()) {
            state.connected = false;
            return state;
        }

        state.connected = true;
        state.position = query_number("time-pos").value_or(0.0);
        state.duration = query_number("duration").value_or(0.0);
        state.volume = static_cast<int>(query_number("volume").value_or(state.volume));
        state.paused = query_bool("pause").value_or(state.paused);

        return state;
    }

private:
    int socket_fd_ = -1;

    static std::string json_escape(const std::string& input) {
        std::string out;
        out.reserve(input.size() + 16);

        for (unsigned char c : input) {
            switch (c) {
                case '\\': out += "\\\\"; break;
                case '"':  out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buffer[7];
                        std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                        out += buffer;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }

        return out;
    }

    bool connect_socket() {
        socket_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (socket_fd_ == -1) return false;

        sockaddr_un address{};
        address.sun_family = AF_UNIX;

        if (std::strlen(IPC_SOCKET) >= sizeof(address.sun_path)) {
            ::close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }

        std::strncpy(address.sun_path, IPC_SOCKET, sizeof(address.sun_path) - 1);

        if (::connect(socket_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
            ::close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }

        return true;
    }

    bool send_raw(const std::string& data) {
        if (socket_fd_ == -1) return false;

        const char* ptr = data.data();
        std::size_t remaining = data.size();

        while (remaining > 0) {
            const ssize_t written = ::send(socket_fd_, ptr, remaining, MSG_NOSIGNAL);
            if (written < 0) {
                if (errno == EINTR) continue;
                ::close(socket_fd_);
                socket_fd_ = -1;
                return false;
            }

            ptr += written;
            remaining -= static_cast<std::size_t>(written);
        }

        return true;
    }

    void send_command(const std::string& name, const std::string& property, const std::string& value) {
        const std::string command =
            "{\"command\":[\"" + name + "\",\"" + property + "\"," + value + "]}\n";
        send_raw(command);
    }

    std::optional<std::string> request_property(const std::string& property) {
        if (!is_connected()) return std::nullopt;

        const std::string request =
            "{\"command\":[\"get_property\",\"" + json_escape(property) + "\"]}\n";

        if (!send_raw(request)) return std::nullopt;

        std::string response;
        char buffer[1024];

        for (int attempt = 0; attempt < 10; ++attempt) {
            const ssize_t count = ::recv(socket_fd_, buffer, sizeof(buffer), MSG_DONTWAIT);
            if (count > 0) {
                response.append(buffer, static_cast<std::size_t>(count));
                if (response.find('\n') != std::string::npos) break;
            } else if (count == 0) {
                ::close(socket_fd_);
                socket_fd_ = -1;
                return std::nullopt;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                return std::nullopt;
            }

            std::this_thread::sleep_for(2ms);
        }

        const auto data_pos = response.find("\"data\":");
        if (data_pos == std::string::npos) return std::nullopt;

        const std::size_t value_start = data_pos + 7;
        const std::size_t value_end = response.find_first_of(",}", value_start);
        if (value_end == std::string::npos) return std::nullopt;

        return response.substr(value_start, value_end - value_start);
    }

    std::optional<double> query_number(const std::string& property) {
        const auto value = request_property(property);
        if (!value) return std::nullopt;

        try {
            return std::stod(*value);
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<bool> query_bool(const std::string& property) {
        const auto value = request_property(property);
        if (!value) return std::nullopt;

        if (*value == "true") return true;
        if (*value == "false") return false;
        return std::nullopt;
    }
};

void draw_box(int y, int x, int height, int width) {
    if (height < 2 || width < 2) return;

    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + width - 1, ACS_URCORNER);
    mvaddch(y + height - 1, x, ACS_LLCORNER);
    mvaddch(y + height - 1, x + width - 1, ACS_LRCORNER);

    for (int i = 1; i < width - 1; ++i) {
        mvaddch(y, x + i, ACS_HLINE);
        mvaddch(y + height - 1, x + i, ACS_HLINE);
    }

    for (int i = 1; i < height - 1; ++i) {
        mvaddch(y + i, x, ACS_VLINE);
        mvaddch(y + i, x + width - 1, ACS_VLINE);
    }
}

std::string format_time(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) seconds = 0;

    const int total = static_cast<int>(seconds);
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int secs = total % 60;

    char buffer[32];
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", hours, minutes, secs);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes, secs);
    }

    return buffer;
}

std::string truncate_text(const std::string& text, int max_width) {
    if (max_width <= 0) return {};
    if (static_cast<int>(text.size()) <= max_width) return text;
    if (max_width <= 3) return text.substr(0, max_width);
    return text.substr(0, static_cast<std::size_t>(max_width - 3)) + "...";
}

void draw_ui(const std::vector<std::string>& songs,
             int selected,
             const fs::path& folder,
             const PlayerState& state) {
    erase();

    int height = 0;
    int width = 0;
    getmaxyx(stdscr, height, width);

    if (height < 18 || width < 60) {
        mvprintw(0, 0, "Terminal window too small. Resize to at least 60x18.");
        refresh();
        return;
    }

    draw_box(0, 0, height - 2, width);

    attron(COLOR_PAIR(2) | A_BOLD);
    mvprintw(1, 2, "TERMINAL MUSIC");
    attroff(COLOR_PAIR(2) | A_BOLD);

    attron(COLOR_PAIR(1));
    mvprintw(1, 18, "%s", truncate_text(folder.string(), width - 21).c_str());
    attroff(COLOR_PAIR(1));

    const int now_y = 3;
    const int now_h = 7;
    const int panel_x = 2;
    const int panel_w = width - 4;
    draw_box(now_y, panel_x, now_h, panel_w);

    attron(COLOR_PAIR(3) | A_BOLD);
    mvprintw(now_y + 1, 4, "NOW PLAYING");
    attroff(COLOR_PAIR(3) | A_BOLD);

    if (!state.title.empty()) {
        mvprintw(now_y + 2, 4, "%s", truncate_text(state.title, panel_w - 8).c_str());

        const std::string time = format_time(state.position) + " / " + format_time(state.duration);
        mvprintw(now_y + 4, 4, "%s", time.c_str());

        int bar_width = panel_w - 30;
        if (bar_width > 5) {
            const double ratio = (state.duration > 0.0)
                ? std::clamp(state.position / state.duration, 0.0, 1.0)
                : 0.0;
            const int filled = static_cast<int>(ratio * bar_width);

            mvaddch(now_y + 5, 4, '[');
            for (int i = 0; i < bar_width; ++i) {
                addch(i < filled ? '=' : ' ');
            }
            addch(']');
        }

        mvprintw(now_y + 4, panel_w - 12, "Vol %3d%%", state.volume);
        mvprintw(now_y + 5, panel_w - 16, "%s", state.paused ? "[PAUSED]" : "[PLAYING]");
    } else {
        mvprintw(now_y + 2, 4, "Nothing playing");
        mvprintw(now_y + 4, 4, "Press Enter to play a song.");
    }

    const int list_y = now_y + now_h + 1;
    const int list_h = height - list_y - 3;
    draw_box(list_y, panel_x, list_h, panel_w);

    attron(COLOR_PAIR(2) | A_BOLD);
    mvprintw(list_y, 4, "PLAYLIST (%zu)", songs.size());
    attroff(COLOR_PAIR(2) | A_BOLD);

    const int visible = list_h - 2;
    int first = 0;
    if (selected >= visible) {
        first = selected - visible + 1;
    }

    for (int row = 0; row < visible; ++row) {
        const int index = first + row;
        if (index >= static_cast<int>(songs.size())) break;

        if (index == selected) attron(A_REVERSE | A_BOLD);

        mvprintw(list_y + 1 + row, 4, "%s",
                 truncate_text(songs[index], panel_w - 8).c_str());

        if (index == selected) attroff(A_REVERSE | A_BOLD);
    }

    attron(COLOR_PAIR(1));
    mvprintw(height - 2, 2,
             "↑↓ select  Enter play  Space pause  ←→ seek  +/- volume  n next  q quit");
    attroff(COLOR_PAIR(1));

    refresh();
}

} // namespace

int main(int argc, char* argv[]) {
    fs::path folder;

    if (argc >= 2) {
        folder = argv[1];
    } else {
        std::cout << "Music folder: ";
        std::getline(std::cin, folder.native());
    }

    std::error_code ec;
    folder = fs::weakly_canonical(folder, ec);

    if (!fs::is_directory(folder, ec)) {
        std::cerr << "Error: not a valid music directory: " << folder << '\n';
        return 1;
    }

    const auto songs = get_songs(folder);
    if (songs.empty()) {
        std::cerr << "No supported audio files found in: " << folder << '\n';
        return 1;
    }

    MpvPlayer player;
    if (!player.start()) {
        std::cerr << "Error: could not start mpv. Make sure mpv is installed and in PATH.\n";
        return 1;
    }

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(100);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_WHITE, -1);
        init_pair(2, COLOR_YELLOW, -1);
        init_pair(3, COLOR_CYAN, -1);
    }

    int selected = 0;
    PlayerState state;
    state.volume = DEFAULT_VOLUME;

    auto play_selected = [&]() {
        if (selected < 0 || selected >= static_cast<int>(songs.size())) return;

        const fs::path song_path = folder / songs[selected];
        if (!file_exists(song_path.string())) return;

        if (player.load(song_path)) {
            state.title = songs[selected];
            state.position = 0.0;
            state.duration = 0.0;
            state.paused = false;
        }
    };

    bool running = true;

    while (running) {
        state = player.query_state(state);
        draw_ui(songs, selected, folder, state);

        const int ch = getch();

        switch (ch) {
            case KEY_UP:
                selected = (selected == 0)
                    ? static_cast<int>(songs.size()) - 1
                    : selected - 1;
                break;

            case KEY_DOWN:
                selected = (selected + 1) % static_cast<int>(songs.size());
                break;

            case '\n':
            case KEY_ENTER:
                play_selected();
                break;

            case ' ':
            case 'p':
                player.toggle_pause();
                break;

            case KEY_LEFT:
                player.seek(-SEEK_STEP);
                break;

            case KEY_RIGHT:
                player.seek(SEEK_STEP);
                break;

            case '+':
            case '=':
                state.volume = std::clamp(state.volume + VOLUME_STEP, 0, 150);
                player.set_volume(state.volume);
                break;

            case '-':
            case '_':
                state.volume = std::clamp(state.volume - VOLUME_STEP, 0, 150);
                player.set_volume(state.volume);
                break;

            case 'n':
            case 'N':
                selected = (selected + 1) % static_cast<int>(songs.size());
                play_selected();
                break;

            case 'q':
            case 'Q':
            case 27: // Escape
                running = false;
                break;

            default:
                break;
        }
    }

    endwin();
    return 0;
}
