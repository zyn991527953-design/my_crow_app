// ============================================================================
// 五日分时线图  five_day_chart.cpp
// ----------------------------------------------------------------------------
// 和K线蜡烛图是完全不同的图表类型：
//   - K线：每根蜡烛是一段时间聚合出的"开高低收"四个数
//   - 分时图/五日线：不聚合，每一分钟就是一个价格点，连续画成一条曲线；
//     五日线 = 连续5个交易日的分时曲线首尾拼接展示
//
// 界面特征（对齐国内行情软件的经典样式）：
//   - 每个交易日一条独立曲线，以"当天昨收价"画一条虚线基准
//   - 高于昨收部分曲线下方浅红色填充，低于昨收部分浅绿色填充
//   - 5天之间用竖线分隔；前4天是收盘定格的历史数据，第5天实时跳动更新，
//     走完一天后自动滚动、补上新的一天，循环演示"动起来"的效果
//
// 依赖：Cairo + X11（与 kline_chart.cpp 相同的技术选型，原因见随附说明）
// 编译：g++ -std=c++17 -O2 five_day_chart.cpp -o five_day_chart $(pkg-config --cflags --libs cairo x11) -lpthread
// 运行：./five_day_chart   （Q 或 Esc 退出）
// ============================================================================

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

// ----------------------------------------------------------------------------
// 配色：红涨绿跌（A股习惯）
// ----------------------------------------------------------------------------
namespace Theme {
    constexpr double BG_R = 0.07, BG_G = 0.09, BG_B = 0.13;
    constexpr double GRID_R = 0.20, GRID_G = 0.23, GRID_B = 0.28;
    constexpr double DIVIDER_R = 0.32, DIVIDER_G = 0.35, DIVIDER_B = 0.40;
    constexpr double TEXT_R = 0.65, TEXT_G = 0.68, TEXT_B = 0.75;
    constexpr double LINE_R = 0.90, LINE_G = 0.92, LINE_B = 0.96;   // 价格曲线本身
    constexpr double UP_R = 0.86, UP_G = 0.20, UP_B = 0.24;         // 红涨（填充/参考线用，透明度另调）
    constexpr double DOWN_R = 0.16, DOWN_G = 0.68, DOWN_B = 0.42;   // 绿跌
    constexpr double VOL_UP_R = 0.70, VOL_UP_G = 0.22, VOL_UP_B = 0.26;
    constexpr double VOL_DOWN_R = 0.18, VOL_DOWN_G = 0.55, VOL_DOWN_B = 0.38;
}

// ----------------------------------------------------------------------------
// 单个交易日的数据：昨收参考价 + 分钟级价格序列（+ 简化的分钟成交量，用于底部量柱）
// ----------------------------------------------------------------------------
struct DayData {
    double prevClose = 0.0;
    std::vector<double> prices;   // 已经产生的分钟价格点，随时间推进逐步增长
    std::vector<double> volumes;  // 与 prices 一一对应的简化成交量
    std::string label;            // 展示用的日期/标签，如 "Day 1"
};

// ----------------------------------------------------------------------------
// 五日分时行情模拟器：负责"生成一整天的历史数据"和"逐分钟推进当天数据"
// 两件事。实盘场景下，只需把随机游走替换成真实的分钟级行情推送即可，
// 图表绘制逻辑完全不用动。
//
// 相比最初版本，这里补上了三个让曲线更接近真实A股分时图的特征：
//   1) 波动率"微笑曲线"：开盘、收盘（含午盘前后）活跃，午间沉闷，
//      而不是全天匀速抖动；
//   2) 涨跌停限制：全天价格被限制在昨收 ±10% 以内，不会无限游走；
//   3) 更真实的开盘缺口：每天开盘都有一个小幅的集合竞价缺口（这是常态），
//      小概率再叠加一个更大幅度的缺口（模拟隔夜消息），而不是"要么严丝合缝
//      衔接、要么固定幅度跳空"这种非黑即白的处理。
// ----------------------------------------------------------------------------
class FiveDayFeedSimulator {
public:
    explicit FiveDayFeedSimulator(int minutesPerDay)
        : minutesPerDay_(minutesPerDay), rng_(std::random_device{}()),
          minuteNoise_(0.0, 0.12), dayDriftNoise_(0.0, 1.2),
          smallGapNoise_(0.0, 0.0015), bigGapNoise_(0.0, 0.010), gapChance_(0.0, 1.0) {}

    // 每天开盘时调用：返回"考虑了集合竞价缺口之后"的真实开盘价
    double planDayOpen(double prevClose) {
        double gapPct = smallGapNoise_(rng_);           // 每天都有的小缺口，接近必然发生
        if (gapChance_(rng_) < 0.12) gapPct += bigGapNoise_(rng_); // ~12%概率叠加一次更大的缺口
        double openPrice = prevClose * (1.0 + gapPct);
        return clampToLimit(openPrice, prevClose);
    }

    // 生成一整天已经走完的历史数据（用于前4天）
    DayData generateCompletedDay(double prevClose, double openPrice, const std::string& label) {
        DayData day;
        day.prevClose = prevClose;
        day.label = label;
        double target = clampToLimit(openPrice + dayDriftNoise_(rng_), prevClose);
        double momentum = 0.0;
        for (int m = 0; m < minutesPerDay_; ++m) {
            double progress = static_cast<double>(m + 1) / minutesPerDay_;
            double basePath = openPrice + (target - openPrice) * progress;
            double vol = volatilityMultiplier(m);
            momentum = momentum * 0.85 + minuteNoise_(rng_) * 0.15 * vol;
            double price = clampToLimit(basePath + momentum, prevClose);
            day.prices.push_back(price);
            day.volumes.push_back(volumeFor(vol));
        }
        return day;
    }

    // 开始新的一天（用于正在实时跳动的"今天"）
    void startLiveDay(double prevClose, double openPrice) {
        livePrevClose_ = prevClose;
        liveOpen_ = openPrice;
        liveTarget_ = clampToLimit(openPrice + dayDriftNoise_(rng_), prevClose);
        liveMomentum_ = 0.0;
        liveMinute_ = 0;
    }

    // 每帧调用：向"今天"推进一分钟
    double nextLiveMinutePrice() {
        double progress = static_cast<double>(liveMinute_ + 1) / minutesPerDay_;
        double basePath = liveOpen_ + (liveTarget_ - liveOpen_) * progress;
        double vol = volatilityMultiplier(liveMinute_);
        liveMomentum_ = liveMomentum_ * 0.85 + minuteNoise_(rng_) * 0.15 * vol;
        ++liveMinute_;
        return clampToLimit(basePath + liveMomentum_, livePrevClose_);
    }

    double liveVolatilityNow() const { return volatilityMultiplier(std::max(0, liveMinute_ - 1)); }
    bool liveDayFinished() const { return liveMinute_ >= minutesPerDay_; }

    double volumeFor(double volMultiplier) const {
        std::uniform_real_distribution<double> volNoise(0.3, 1.0);
        return volMultiplier * volNoise(const_cast<std::mt19937&>(rng_));
    }

    // 波动率微笑：早盘、午盘收尾、午后开盘、尾盘活跃，午间（每个session中段）沉闷。
    // A股一天=240分钟，前120分钟(上午)和后120分钟(下午)分别各自成一个"微笑"。
    double volatilityMultiplier(int minuteIndex) const {
        int session = minutesPerDay_ / 2;
        int m = minuteIndex % session;
        double p = static_cast<double>(m) / session;      // 0..1，session内的进度
        double u = 4.0 * (p - 0.5) * (p - 0.5);            // 两端为1，中段为0的抛物线
        return 0.5 + 1.3 * u;                              // 中段约0.5倍，两端约1.8倍
    }

private:
    double clampToLimit(double price, double prevClose) const {
        double hi = prevClose * 1.10; // 涨停：+10%
        double lo = prevClose * 0.90; // 跌停：-10%
        return std::min(hi, std::max(lo, std::max(1.0, price)));
    }

    int minutesPerDay_;
    mutable std::mt19937 rng_;
    std::normal_distribution<double> minuteNoise_;
    std::normal_distribution<double> dayDriftNoise_;
    std::normal_distribution<double> smallGapNoise_, bigGapNoise_;
    std::uniform_real_distribution<double> gapChance_;

    double livePrevClose_ = 0.0, liveOpen_ = 0.0, liveTarget_ = 0.0, liveMomentum_ = 0.0;
    int liveMinute_ = 0;
};

// ----------------------------------------------------------------------------
// 五日分时线图表：维护 5 天数据（4 天历史 + 1 天实时），并绘制到 Cairo 画布
// ----------------------------------------------------------------------------
class FiveDayChart {
public:
    explicit FiveDayChart(int minutesPerDay = 240)
        : minutesPerDay_(minutesPerDay), feed_(minutesPerDay) {
        double prevClose = 100.0;
        for (int i = 0; i < 4; ++i) {
            double openPrice = feed_.planDayOpen(prevClose);
            days_.push_back(feed_.generateCompletedDay(prevClose, openPrice, "Day " + std::to_string(i + 1)));
            prevClose = days_.back().prices.back();
        }
        double liveOpen = feed_.planDayOpen(prevClose);
        feed_.startLiveDay(prevClose, liveOpen);
        days_.push_back(DayData{prevClose, {}, {}, "Day 5 (live)"});
    }

    // 每帧调用一次；ticksPerMinute 控制"多少帧走一分钟"，用来调节动画节奏
    void update() {
        ++frameCounter_;
        if (frameCounter_ < ticksPerMinute_) return;
        frameCounter_ = 0;

        if (feed_.liveDayFinished()) {
            // 今天走完了：滚动窗口，最老的一天丢弃，开启新的一天
            double lastClose = days_.back().prices.empty() ? days_.back().prevClose
                                                             : days_.back().prices.back();
            days_.pop_front();
            for (size_t i = 0; i < days_.size(); ++i) days_[i].label = "Day " + std::to_string(i + 1);
            double newOpen = feed_.planDayOpen(lastClose);
            feed_.startLiveDay(lastClose, newOpen);
            days_.push_back(DayData{lastClose, {}, {}, "Day 5 (live)"});
            return;
        }

        double p = feed_.nextLiveMinutePrice();
        DayData& live = days_.back();
        live.prices.push_back(p);
        live.volumes.push_back(feed_.volumeFor(feed_.liveVolatilityNow()));
    }

    void draw(cairo_t* cr, int width, int height) const {
        cairo_set_source_rgb(cr, Theme::BG_R, Theme::BG_G, Theme::BG_B);
        cairo_paint(cr);

        const int marginLeft = 65, marginRight = 55, marginTop = 34, marginBottom = 18;
        const int volH = 70; // 底部成交量区域高度
        const int plotW = width - marginLeft - marginRight;
        const int plotH = height - marginTop - marginBottom - volH - 10;
        if (plotW <= 20 || plotH <= 20) return;

        // 价格范围：取5天所有已生成价格 + 各自昨收，一起决定Y轴范围
        double lo = 1e18, hi = -1e18;
        for (const auto& d : days_) {
            lo = std::min(lo, d.prevClose); hi = std::max(hi, d.prevClose);
            for (double p : d.prices) { lo = std::min(lo, p); hi = std::max(hi, p); }
        }
        double pad = (hi - lo) * 0.10 + 0.01;
        lo -= pad; hi += pad;
        auto priceToY = [&](double price) {
            return marginTop + plotH * (1.0 - (price - lo) / (hi - lo));
        };

        double dayW = static_cast<double>(plotW) / days_.size();

        drawPriceGrid(cr, marginLeft, marginTop, plotW, plotH, lo, hi, priceToY);

        double maxVol = 0.01;
        for (const auto& d : days_) for (double v : d.volumes) maxVol = std::max(maxVol, v);

        for (size_t i = 0; i < days_.size(); ++i) {
            double x0 = marginLeft + dayW * i;
            drawDay(cr, days_[i], x0, dayW, priceToY, i == days_.size() - 1);
            drawDayVolume(cr, days_[i], x0, dayW, height - marginBottom - volH, volH, maxVol);

            // 竖向分隔线
            cairo_set_source_rgb(cr, Theme::DIVIDER_R, Theme::DIVIDER_G, Theme::DIVIDER_B);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, x0, marginTop);
            cairo_line_to(cr, x0, height - marginBottom);
            cairo_stroke(cr);

            drawDayLabel(cr, days_[i], x0 + dayW / 2.0, height - marginBottom + 14);
        }
        // 最右边界线
        cairo_set_source_rgb(cr, Theme::DIVIDER_R, Theme::DIVIDER_G, Theme::DIVIDER_B);
        cairo_move_to(cr, marginLeft + plotW, marginTop);
        cairo_line_to(cr, marginLeft + plotW, height - marginBottom);
        cairo_stroke(cr);

        drawCurrentPriceTag(cr, marginLeft + plotW, priceToY, width);
        drawTitle(cr);
    }

private:
    void drawDay(cairo_t* cr, const DayData& d, double x0, double dayW,
                 const std::function<double(double)>& priceToY, bool isLive) const {
        if (d.prices.empty()) return;
        double refY = priceToY(d.prevClose);

        // 昨收参考虚线（仅当天范围内）
        cairo_save(cr);
        cairo_set_source_rgba(cr, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B, 0.45);
        cairo_set_line_width(cr, 1.0);
        double dash[] = {3.0, 3.0};
        cairo_set_dash(cr, dash, 2, 0);
        cairo_move_to(cr, x0, refY);
        cairo_line_to(cr, x0 + dayW, refY);
        cairo_stroke(cr);
        cairo_restore(cr);

        // 午间休市标记：A股上午/下午两个交易时段的分界（第120分钟处）
        double midX = x0 + dayW / 2.0;
        cairo_save(cr);
        cairo_set_source_rgba(cr, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B, 0.25);
        cairo_set_line_width(cr, 1.0);
        double lunchDash[] = {1.5, 2.5};
        cairo_set_dash(cr, lunchDash, 2, 0);
        cairo_move_to(cr, midX, refY - 40 < 0 ? 0 : refY - 40);
        cairo_line_to(cr, midX, refY + 40);
        cairo_stroke(cr);
        cairo_restore(cr);

        double stepX = dayW / minutesPerDay_;

        // 涨跌填充：以昨收为界，逐段用对应颜色的半透明色块填到参考线
        for (size_t i = 0; i + 1 < d.prices.size(); ++i) {
            double xA = x0 + stepX * i, xB = x0 + stepX * (i + 1);
            double yA = priceToY(d.prices[i]), yB = priceToY(d.prices[i + 1]);
            bool up = d.prices[i + 1] >= d.prevClose;
            if (up) cairo_set_source_rgba(cr, Theme::UP_R, Theme::UP_G, Theme::UP_B, 0.16);
            else    cairo_set_source_rgba(cr, Theme::DOWN_R, Theme::DOWN_G, Theme::DOWN_B, 0.16);
            cairo_move_to(cr, xA, refY);
            cairo_line_to(cr, xA, yA);
            cairo_line_to(cr, xB, yB);
            cairo_line_to(cr, xB, refY);
            cairo_close_path(cr);
            cairo_fill(cr);
        }

        // 价格曲线本身
        cairo_set_source_rgb(cr, Theme::LINE_R, Theme::LINE_G, Theme::LINE_B);
        cairo_set_line_width(cr, 1.6);
        cairo_move_to(cr, x0, priceToY(d.prices[0]));
        for (size_t i = 1; i < d.prices.size(); ++i) {
            cairo_line_to(cr, x0 + stepX * i, priceToY(d.prices[i]));
        }
        cairo_stroke(cr);

        if (isLive) {
            // 实时跳动的最新点，加一个高亮圆点强调"正在动"
            double lastX = x0 + stepX * (d.prices.size() - 1);
            double lastY = priceToY(d.prices.back());
            bool up = d.prices.back() >= d.prevClose;
            if (up) cairo_set_source_rgb(cr, Theme::UP_R, Theme::UP_G, Theme::UP_B);
            else    cairo_set_source_rgb(cr, Theme::DOWN_R, Theme::DOWN_G, Theme::DOWN_B);
            cairo_arc(cr, lastX, lastY, 3.2, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }

    void drawDayVolume(cairo_t* cr, const DayData& d, double x0, double dayW,
                        int volTop, int volH, double maxVol) const {
        if (d.volumes.empty()) return;
        double stepX = dayW / minutesPerDay_;
        for (size_t i = 0; i < d.volumes.size(); ++i) {
            bool up = d.prices[i] >= d.prevClose;
            if (up) cairo_set_source_rgba(cr, Theme::VOL_UP_R, Theme::VOL_UP_G, Theme::VOL_UP_B, 0.85);
            else    cairo_set_source_rgba(cr, Theme::VOL_DOWN_R, Theme::VOL_DOWN_G, Theme::VOL_DOWN_B, 0.85);
            double h = volH * (d.volumes[i] / maxVol);
            cairo_rectangle(cr, x0 + stepX * i, volTop + volH - h, std::max(1.0, stepX * 0.9), h);
            cairo_fill(cr);
        }
    }

    void drawPriceGrid(cairo_t* cr, int marginLeft, [[maybe_unused]] int marginTop, int plotW,
                        [[maybe_unused]] int plotH, double lo, double hi,
                        const std::function<double(double)>& priceToY) const {
        cairo_set_source_rgb(cr, Theme::GRID_R, Theme::GRID_G, Theme::GRID_B);
        cairo_set_line_width(cr, 1.0);
        const int hLines = 4;
        for (int i = 0; i <= hLines; ++i) {
            double price = lo + (hi - lo) * i / hLines;
            double y = priceToY(price);
            cairo_move_to(cr, marginLeft, y);
            cairo_line_to(cr, marginLeft + plotW, y);
            cairo_stroke(cr);

            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2) << price;
            cairo_set_source_rgb(cr, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B);
            cairo_set_font_size(cr, 11);
            cairo_text_extents_t ext;
            cairo_text_extents(cr, oss.str().c_str(), &ext);
            cairo_move_to(cr, marginLeft - ext.width - 8, y + ext.height / 2.0);
            cairo_show_text(cr, oss.str().c_str());
            cairo_set_source_rgb(cr, Theme::GRID_R, Theme::GRID_G, Theme::GRID_B);
        }
    }

    void drawDayLabel(cairo_t* cr, const DayData& d, double cx, double y) const {
        cairo_set_source_rgb(cr, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B);
        cairo_set_font_size(cr, 11);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, d.label.c_str(), &ext);
        cairo_move_to(cr, cx - ext.width / 2.0, y);
        cairo_show_text(cr, d.label.c_str());
    }

    void drawCurrentPriceTag(cairo_t* cr, double x, const std::function<double(double)>& priceToY,
                              int width) const {
        if (days_.back().prices.empty()) return;
        double price = days_.back().prices.back();
        double y = priceToY(price);
        bool up = price >= days_.back().prevClose;
        if (up) cairo_set_source_rgb(cr, Theme::UP_R, Theme::UP_G, Theme::UP_B);
        else    cairo_set_source_rgb(cr, Theme::DOWN_R, Theme::DOWN_G, Theme::DOWN_B);

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << price;
        cairo_set_font_size(cr, 12);
        cairo_move_to(cr, std::min(static_cast<double>(width) - 48, x + 4), y + 4);
        cairo_show_text(cr, oss.str().c_str());
    }

    void drawTitle(cairo_t* cr) const {
        cairo_set_source_rgb(cr, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B);
        cairo_set_font_size(cr, 14);
        cairo_move_to(cr, 12, 20);
        cairo_show_text(cr, "5-Day Intraday Line (simulated, red=above prev close / green=below)");
    }

    int minutesPerDay_;
    int ticksPerMinute_ = 4;   // 每4帧推进1分钟，60fps下约每15个交易分钟/秒，整天约16秒走完，可按需调节
    int frameCounter_ = 0;
    std::deque<DayData> days_;
    FiveDayFeedSimulator feed_;
};

// ----------------------------------------------------------------------------
// main：与 kline_chart.cpp 相同的 X11 建窗 + 固定帧率事件循环模式
// ----------------------------------------------------------------------------
int main() {
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        std::cerr << "无法打开 X11 Display（如在无图形环境下运行，请配合 Xvfb 使用）\n";
        return 1;
    }

    int screen = DefaultScreen(display);
    int width = 960, height = 560;
    int depth = DefaultDepth(display, screen);

    Window window = XCreateSimpleWindow(
        display, RootWindow(display, screen),
        0, 0, width, height, 0,
        BlackPixel(display, screen), BlackPixel(display, screen));

    XStoreName(display, window, "Five-Day Intraday Line Chart");
    XSelectInput(display, window, ExposureMask | KeyPressMask | StructureNotifyMask);
    XMapWindow(display, window);

    Atom wmDelete = XInternAtom(display, "WM_DELETE_WINDOW", True);
    XSetWMProtocols(display, window, &wmDelete, 1);

    GC gc = XCreateGC(display, window, 0, nullptr);

    // 离屏缓冲改用 X11 Pixmap + XCopyArea（服务端拷贝），原因与 kline_chart.cpp 相同：
    // 全屏/大分辨率下，客户端内存画布整体贴回窗口的开销会明显变高，Pixmap方案更快。
    Pixmap backPixmap = XCreatePixmap(display, window, width, height, depth);
    cairo_surface_t* backbuffer = cairo_xlib_surface_create(
        display, backPixmap, DefaultVisual(display, screen), width, height);
    cairo_t* backCr = cairo_create(backbuffer);

    FiveDayChart chart(240); // A股一个交易日≈240分钟

    const int fps = 60;
    const auto frameDuration = std::chrono::milliseconds(1000 / fps);
    bool running = true;

    while (running) {
        auto frameStart = std::chrono::steady_clock::now();
        bool resized = false;

        while (XPending(display) > 0) {
            XEvent ev;
            XNextEvent(display, &ev);
            if (ev.type == ConfigureNotify) {
                XConfigureEvent xce = ev.xconfigure;
                if (xce.width != width || xce.height != height) {
                    width = xce.width;
                    height = xce.height;
                    resized = true;
                }
            } else if (ev.type == ClientMessage &&
                       static_cast<Atom>(ev.xclient.data.l[0]) == wmDelete) {
                running = false;
            } else if (ev.type == KeyPress) {
                KeySym key = XLookupKeysym(&ev.xkey, 0);
                if (key == XK_q || key == XK_Escape) running = false;
            }
        }
        if (!running) break;

        if (resized) {
            cairo_destroy(backCr);
            cairo_surface_destroy(backbuffer);
            XFreePixmap(display, backPixmap);
            backPixmap = XCreatePixmap(display, window, width, height, depth);
            backbuffer = cairo_xlib_surface_create(
                display, backPixmap, DefaultVisual(display, screen), width, height);
            backCr = cairo_create(backbuffer);
        }

        chart.update();
        chart.draw(backCr, width, height);
        cairo_surface_flush(backbuffer);

        XCopyArea(display, backPixmap, window, gc, 0, 0, width, height, 0, 0);
        XFlush(display);

        auto elapsed = std::chrono::steady_clock::now() - frameStart;
        auto sleepTime = frameDuration - elapsed;
        if (sleepTime > std::chrono::milliseconds(0)) std::this_thread::sleep_for(sleepTime);
    }

    cairo_destroy(backCr);
    cairo_surface_destroy(backbuffer);
    XFreePixmap(display, backPixmap);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
