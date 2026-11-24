// whale_song —— 一只在终端里游泳的小鲸鱼
//
// 这是个"玩"出来的项目，但技术点是实的：
//   * C++20：std::format / std::span / std::chrono 字面量 / std::numbers / <charconv>
//   * Windows 下探测控制台尺寸、开启 VT（真彩 ANSI）序列支持
//   * 逐帧渲染 + 游程编码上色（不是每格一个转义码，不然终端会卡）
//   * 非 tty（重定向到文件）时自动退化成静态图，方便截图与测试
//
// 用法：
//   whale_song                 动画，默认 300 帧 @ 30fps
//   whale_song --frames 600    自定义帧数
//   whale_song --fps 60        自定义帧率
//   whale_song --once          只画一帧（静态）
//   whale_song --plain         不输出颜色（纯 ASCII）
//   whale_song --help          看帮助

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <iostream>
#include <numbers>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>
#else
#  include <sys/ioctl.h>
#  include <unistd.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// 小鲸鱼的像素画（6 行，朝右）
// ---------------------------------------------------------------------------
constexpr std::array<std::string_view, 6> kWhale{{
    R"(           ___)",
    R"(       .-''    \__)",
    R"(      /   o   o   \_)",
    R"(     |     ^        >)",
    R"(      \____________/ )",
    R"(         \/    \/    )",
}};

constexpr int kWhaleHeight = static_cast<int>(kWhale.size());

// 单元格类别：决定用什么颜色
enum class Cell : std::uint8_t {
    Empty  = 0,
    Whale  = 1,
    Wave   = 2,
    Bubble = 3,
};

using Rgb = std::array<int, 3>;

struct Size {
    int w = 100;
    int h = 30;
};

struct Options {
    int  frames = 300;
    int  fps    = 30;
    bool once   = false;
    bool plain  = false;
    bool dump   = false;
    bool ansi   = false;   // 强制输出 ANSI（非 tty 时用于测试/录屏）
};

struct Bubble {
    int x     = 0;
    int y     = 0;
    int speed = 1;
};

// ---------------------------------------------------------------------------
// 终端能力
// ---------------------------------------------------------------------------

bool isTty() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return ::isatty(STDOUT_FILENO) != 0;
#endif
}

bool enableVirtualTerminal() {
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(out, &mode)) {
        return false;
    }
    return SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    return true;
#endif
}

Size terminalSize() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        return Size{
            info.srWindow.Right - info.srWindow.Left + 1,
            info.srWindow.Bottom - info.srWindow.Top + 1,
        };
    }
#else
    winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        return Size{static_cast<int>(ws.ws_col), static_cast<int>(ws.ws_row)};
    }
#endif
    return Size{};  // 兜底 100x30
}

// 动画结束时把光标和颜色都还原，别把用户终端搞坏
struct ScreenGuard {
    bool active = false;
    ScreenGuard() = default;
    ScreenGuard(const ScreenGuard&)            = delete;
    ScreenGuard& operator=(const ScreenGuard&) = delete;
    ~ScreenGuard() {
        if (active) {
            std::cout << "\x1b[?25h" << "\x1b[0m" << std::flush;
        }
    }
};

// ---------------------------------------------------------------------------
// 参数解析
// ---------------------------------------------------------------------------

void printHelp() {
    std::cout
        << "whale_song —— 终端小鲸鱼\n"
        << "\n"
        << "  --frames <n>  帧数，默认 300\n"
        << "  --fps <n>     帧率，默认 30\n"
        << "  --once        只画一帧\n"
        << "  --plain       不要颜色（纯 ASCII）\n"
        << "  --dump        导出单帧的着色分段（给外部渲染器用）\n"
        << "  --ansi        即使不是终端也输出 ANSI 序列（测试用）\n"
        << "  --help        这份帮助\n";
}

int toInt(std::string_view text, int fallback) {
    int value = fallback;
    const char* first = text.data();
    const char* last  = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last) {
        return fallback;
    }
    return value;
}

Options parseOptions(std::span<const char* const> argv) {
    Options opt;
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            printHelp();
            std::exit(0);
        } else if (arg == "--once") {
            opt.once = true;
        } else if (arg == "--plain") {
            opt.plain = true;
        } else if (arg == "--dump") {
            opt.dump = true;
        } else if (arg == "--ansi") {
            opt.ansi = true;
        } else if (arg == "--frames" && i + 1 < argv.size()) {
            opt.frames = toInt(std::string_view{argv[++i]}, opt.frames);
        } else if (arg == "--fps" && i + 1 < argv.size()) {
            opt.fps = toInt(std::string_view{argv[++i]}, opt.fps);
        }
    }
    opt.frames = std::clamp(opt.frames, 1, 100000);
    opt.fps    = std::clamp(opt.fps, 1, 240);
    return opt;
}

// ---------------------------------------------------------------------------
// 画面
// ---------------------------------------------------------------------------

// 水面上的波纹：三个不同频率的正弦叠在一起，看起来才不像正弦波
char waveChar(int x, int waveRow, int frame) {
    const double phase = static_cast<double>(frame) * 0.22 + static_cast<double>(waveRow) * 1.9;
    const double v     = std::sin(static_cast<double>(x) * 0.17 + phase);
    switch (waveRow) {
    case 0:  return v > 0.45 ? '~' : ' ';
    case 1:  return v > 0.10 ? '~' : (v > -0.40 ? '.' : ' ');
    default: return v > -0.25 ? '-' : ' ';
    }
}

int whaleX(int frame, int cols) {
    const double phase = static_cast<double>(frame) * 0.045;
    const double t     = std::sin(phase) * 0.5 + 0.5;      // 0..1
    const double span  = static_cast<double>(cols + 24);
    return static_cast<int>(t * span) - 24;
}

int whaleY(int frame, int rows) {
    // 鲸鱼不会直挺挺地平移，要有一点点上下起伏
    const int base = rows - kWhaleHeight - 3;
    const int bob  = static_cast<int>(std::sin(static_cast<double>(frame) * 0.11) * 1.2);
    return base + bob;
}

Rgb colorFor(Cell cell, int row, int waveRow, int rows) {
    switch (cell) {
    case Cell::Whale: {
        // 青色系，越靠下越深，像有体积
        const int k = std::clamp(row, 0, rows);
        return Rgb{80 + k * 3, 190 + k, 220 + k};
    }
    case Cell::Wave: {
        // 越深的水越蓝
        const int k = std::clamp(waveRow * 2, 0, 6);
        return Rgb{30, 105 + k * 10, 190 + k * 8};
    }
    case Cell::Bubble:
        return Rgb{200, 225, 255};
    case Cell::Empty:
    default:
        return Rgb{90, 150, 205};
    }
}

struct Scene {
    int cols = 0;
    int rows = 0;
    std::vector<std::string> glyphs;               // 每格一个字符
    std::vector<std::vector<Cell>> cells;          // 每格的类别
    std::vector<Bubble> bubbles;

    Scene(int c, int r)
        : cols(c),
          rows(r),
          glyphs(static_cast<std::size_t>(r), std::string(static_cast<std::size_t>(c), ' ')),
          cells(static_cast<std::size_t>(r), std::vector<Cell>(static_cast<std::size_t>(c), Cell::Empty)) {
    }

    void put(int x, int y, char ch, Cell cell) {
        if (x < 0 || x >= cols || y < 0 || y >= rows) {
            return;
        }
        glyphs[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = ch;
        cells[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)]  = cell;
    }

    void drawWaves(int frame) {
        const int waterTop = rows - 3;
        for (int waveRow = 0; waveRow < 3; ++waveRow) {
            const int y = waterTop + waveRow;
            if (y < 0 || y >= rows) {
                continue;
            }
            for (int x = 0; x < cols; ++x) {
                const char ch = waveChar(x, waveRow, frame);
                if (ch != ' ') {
                    put(x, y, ch, Cell::Wave);
                }
            }
        }
    }

    void drawWhale(int frame) {
        const int x0 = whaleX(frame, cols);
        const int y0 = whaleY(frame, rows);
        for (int i = 0; i < kWhaleHeight; ++i) {
            const std::string_view line = kWhale[static_cast<std::size_t>(i)];
            for (int j = 0; j < static_cast<int>(line.size()); ++j) {
                const char ch = line[static_cast<std::size_t>(j)];
                if (ch != ' ') {
                    put(x0 + j, y0 + i, ch, Cell::Whale);
                }
            }
        }
    }

    void drawBubbles() {
        for (const Bubble& b : bubbles) {
            put(b.x, b.y, 'o', Cell::Bubble);
        }
    }

    // 头顶的水柱：三档高度循环，看起来就是它在喷气
    void drawSpout(int frame) {
        const int headX = whaleX(frame, cols) + 8;
        const int headY = whaleY(frame, rows) - 2;
        if (headY < 0) {
            return;
        }
        const int phase = (frame / 3) % 4;
        if (phase >= 1) {
            put(headX, headY + 1, '|', Cell::Bubble);
        }
        if (phase >= 2) {
            put(headX, headY, ':', Cell::Bubble);
        }
        if (phase >= 3) {
            put(headX - 1, headY, '.', Cell::Bubble);
            put(headX + 1, headY, '.', Cell::Bubble);
        }
    }

    // 游程编码上色：同类格子只发一次颜色码，不然一帧几千个转义序列终端会卡
    std::string render(bool color) const {
        std::string out;
        out.reserve(static_cast<std::size_t>(cols * rows + rows * 24));
        const int waterTop = rows - 3;
        for (int y = 0; y < rows; ++y) {
            out += std::format("\x1b[{};1H", y + 1);   // 光标定位，不用整屏清空
            const int waveRow = y - waterTop;
            int x = 0;
            while (x < cols) {
                const Cell cell = cells[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
                int end = x + 1;
                while (end < cols && cells[static_cast<std::size_t>(y)][static_cast<std::size_t>(end)] == cell) {
                    ++end;
                }
                if (color) {
                    const Rgb rgb = colorFor(cell, y, waveRow, rows);
                    out += std::format("\x1b[38;2;{};{};{}m", rgb[0], rgb[1], rgb[2]);
                }
                out.append(glyphs[static_cast<std::size_t>(y)], static_cast<std::size_t>(x),
                           static_cast<std::size_t>(end - x));
                x = end;
            }
        }
        out += "\x1b[0m";
        return out;
    }

    // 把一帧拆成"同色连续段"导出，方便外部工具（比如 PowerShell + GDI+）
    // 原样画成图片。格式：首行 cols\rows，其余每行 y x len cat text
    std::string dumpRuns() const {
        std::string out = std::format("{}\t{}\n", cols, rows);
        for (int y = 0; y < rows; ++y) {
            int x = 0;
            while (x < cols) {
                const Cell cell = cells[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
                int end = x + 1;
                while (end < cols && cells[static_cast<std::size_t>(y)][static_cast<std::size_t>(end)] == cell) {
                    ++end;
                }
                const std::string_view text{glyphs[static_cast<std::size_t>(y)]};
                out += std::format("{}\t{}\t{}\t{}\t{}\n", y, x, end - x, static_cast<int>(cell),
                                   text.substr(static_cast<std::size_t>(x), static_cast<std::size_t>(end - x)));
                x = end;
            }
        }
        return out;
    }

    // 无颜色版本：给重定向和 --once 用，末尾空格裁掉，看着干净
    std::string renderPlain() const {
        std::string out;
        bool started = false;
        for (int y = 0; y < rows; ++y) {
            const std::string& line = glyphs[static_cast<std::size_t>(y)];
            const auto last = line.find_last_not_of(' ');
            if (last == std::string::npos) {
                if (started) {
                    out += '\n';
                }
                continue;
            }
            started = true;
            out.append(line, 0, last + 1);
            out += '\n';
        }
        return out;
    }
};

// ---------------------------------------------------------------------------
// 主循环
// ---------------------------------------------------------------------------

int run(const Options& opt) {
    const Size size = terminalSize();
    const int cols  = std::clamp(size.w, 40, 240);
    const int rows  = std::clamp(size.h, 14, 80);

    const bool tty   = isTty();
    const bool color = !opt.plain && (opt.ansi || (tty && enableVirtualTerminal()));
    const bool animate = !opt.once && !opt.plain && (opt.ansi || tty);

    Scene scene{cols, rows};

    std::mt19937 rng{20261123u};  // 固定种子：每次跑出来的气泡轨迹都一样，方便对照
    std::uniform_int_distribution<int> xs{0, cols - 1};
    std::uniform_int_distribution<int> ys{0, rows - 1};
    std::uniform_int_distribution<int> speeds{1, 2};

    const int bubbleCount = std::clamp(cols / 8, 4, 24);
    scene.bubbles.reserve(static_cast<std::size_t>(bubbleCount));
    for (int i = 0; i < bubbleCount; ++i) {
        scene.bubbles.push_back(Bubble{xs(rng), ys(rng), speeds(rng)});
    }

    if (opt.dump) {
        // 只导出一帧，给外部渲染器（比如画成 PNG）用
        scene.drawWaves(9);
        scene.drawWhale(9);
        scene.drawSpout(9);
        scene.drawBubbles();
        std::cout << scene.dumpRuns();
        return 0;
    }

    ScreenGuard guard;
    if (animate) {
        guard.active = true;
        std::cout << "\x1b[2J" << "\x1b[?25l" << std::flush;  // 清屏 + 藏光标
    }

    const auto frameTime = std::chrono::milliseconds(1000 / opt.fps);
    const int totalFrames = (animate ? opt.frames : 1);

    for (int frame = 0; frame < totalFrames; ++frame) {
        // 清格子重新画（气泡是覆盖式，不重画会和残影叠在一起）
        for (auto& rowCells : scene.cells) {
            std::fill(rowCells.begin(), rowCells.end(), Cell::Empty);
        }
        for (auto& rowGlyphs : scene.glyphs) {
            std::fill(rowGlyphs.begin(), rowGlyphs.end(), ' ');
        }

        scene.drawWaves(frame);
        scene.drawWhale(frame);
        scene.drawSpout(frame);
        scene.drawBubbles();

        if (animate) {
            std::cout << scene.render(color) << std::flush;
            std::this_thread::sleep_for(frameTime);
            // 气泡上浮，冒到顶就回水底重新开始
            for (Bubble& b : scene.bubbles) {
                b.y -= b.speed;
                if (b.y < 0) {
                    b.y = rows - 1;
                    b.x = xs(rng);
                    b.speed = speeds(rng);
                }
            }
        } else {
            std::cout << scene.renderPlain();
        }
    }

    if (animate) {
        // 动画收尾：把光标挪到屏幕下方，别盖在鲸鱼身上
        std::cout << std::format("\x1b[{};1H", rows) << "\x1b[0m" << std::flush;
    }
    guard.active = false;
    std::cout << std::flush;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<const char* const> args{argv, static_cast<std::size_t>(argc)};
    const Options opt = parseOptions(args);
    return run(opt);
}
