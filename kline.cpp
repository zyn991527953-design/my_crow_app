// ============================================================================
// K线图动画演示  kline_chart.cpp
// ----------------------------------------------------------------------------
// 功能：
//   - 定义 K 线数据结构（开盘/最高/最低/收盘 = OHLC）
//   - 模拟"当天多组数据"：连续生成多根 K 线（可替换为真实行情数据）
//   - 让 K 线动起来：当前这一根 K 线随"实时报价"逐帧变化（影线伸缩、
//     实体涨跌变色），满一个周期后定格收盘，滚动出下一根新蜡烛，
//     整条图表随之向左平移，与真实交易软件的行情动画效果一致
//
// 依赖：Cairo（2D 矢量绘图） + X11（窗口与事件循环）
// 编译：g++ -std=c++17 -O2 kline_chart.cpp -o kline_chart $(pkg-config --cflags --libs cairo x11) -lpthread
// 运行：./kline_chart
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
// 数据结构：一根 K 线 = 开盘价 / 最高价 / 最低价 / 收盘价
// ----------------------------------------------------------------------------
struct Candle {
    double open  = 0.0;
    double high  = 0.0;
    double low   = 0.0;
    double close = 0.0;

    bool isRising() const { return close >= open; }
};

// ----------------------------------------------------------------------------
// 配色：按中国 A 股习惯"红涨绿跌"（欧美市场通常相反，如需切换改这里即可）
// ----------------------------------------------------------------------------
namespace Theme {
    constexpr double BG_R = 0.07, BG_G = 0.09, BG_B = 0.13;         // 深色背景
    constexpr double GRID_R = 0.20, GRID_G = 0.23, GRID_B = 0.28;   // 网格线
    constexpr double TEXT_R = 0.65, TEXT_G = 0.68, TEXT_B = 0.75;   // 文字
    constexpr double UP_R = 0.86, UP_G = 0.20, UP_B = 0.24;         // 红涨
    constexpr double DOWN_R = 0.16, DOWN_G = 0.68, DOWN_B = 0.42;   // 绿跌
    constexpr double CUR_R = 0.95, CUR_G = 0.76, CUR_B = 0.20;      // 当前价参考线
}

// ----------------------------------------------------------------------------
// 行情模拟器：用随机游走模拟"当天多组数据"持续到来的效果。
// 实盘场景下，只需把 tick() 换成真实行情回调喂入的最新价即可，
// 其余动画 / 绘制逻辑完全不用改动。
// ----------------------------------------------------------------------------
class MarketFeedSimulator {
public:
    explicit MarketFeedSimulator(double startPrice)
        : price_(startPrice), rng_(std::random_device{}()), noise_(0.0, 0.35) {}

    double tick() {
        price_ += noise_(rng_);
        price_ = std::max(1.0, price_);
        return price_;
    }

private:
    double price_;
    std::mt19937 rng_;
    std::normal_distribution<double> noise_;
};

// ----------------------------------------------------------------------------
// K 线图表：负责维护"历史已收盘 K 线 + 当前正在形成的 K 线"，
// 并把二者绘制到 Cairo 画布上。
// ----------------------------------------------------------------------------
class KLineChart {
public:
    KLineChart(int ticksPerCandle, size_t maxVisibleCandles)
        : ticksPerCandle_(ticksPerCandle), maxVisible_(maxVisibleCandles), feed_(100.0) {
        startNewCandle();
    }

    // 每一帧调用一次：喂入一个新报价，更新"正在形成的 K 线"
    void update() {
        double p = feed_.tick();
        current_.high  = std::max(current_.high, p);
        current_.low   = std::min(current_.low, p);
        current_.close = p;
        ++tickInCandle_;

        if (tickInCandle_ >= ticksPerCandle_) {
            history_.push_back(current_);
            if (history_.size() > maxVisible_ * 3) history_.pop_front(); // 避免无限增长
            startNewCandle();
        }
    }

    void draw(cairo_t* cr, int width, int height) const {
        // 背景
        cairo_set_source_rgb(cr, Theme::BG_R, Theme::BG_G, Theme::BG_B);
        cairo_paint(cr);

        const int marginLeft = 70, marginRight = 20, marginTop = 30, marginBottom = 30;
        const int plotW = width - marginLeft - marginRight;
        const int plotH = height - marginTop - marginBottom;
        if (plotW <= 10 || plotH <= 10) return;

        // 取最近 maxVisible_ 根（含当前这根未收盘的）用于绘制
        std::vector<Candle> visible;
        size_t startIdx = history_.size() > maxVisible_ - 1 ? history_.size() - (maxVisible_ - 1) : 0;
        for (size_t i = startIdx; i < history_.size(); ++i) visible.push_back(history_[i]);
        visible.push_back(current_); // 最后一根是"正在跳动"的

        // 计算价格范围（留 5% 边距，避免最高/最低贴边）
        double lo = visible.front().low, hi = visible.front().high;
        for (const auto& c : visible) { lo = std::min(lo, c.low); hi = std::max(hi, c.high); }
        double pad = (hi - lo) * 0.08 + 0.01;
        lo -= pad; hi += pad;

        auto priceToY = [&](double price) {
            return marginTop + plotH * (1.0 - (price - lo) / (hi - lo));
        };

        drawGrid(cr, marginLeft, marginTop, plotW, plotH, lo, hi, priceToY);

        // 每根蜡烛的宽度与间距
        double slot = static_cast<double>(plotW) / maxVisible_;
        double bodyW = slot * 0.6;

        for (size_t i = 0; i < visible.size(); ++i) {
            const Candle& c = visible[i];
            double cx = marginLeft + slot * i + slot / 2.0;
            bool rising = c.isRising();
            bool isForming = (i == visible.size() - 1);
            drawCandle(cr, c, cx, bodyW, priceToY, rising, isForming);
        }

        // 当前价参考虚线 + 数值
        double curY = priceToY(current_.close);
        cairo_save(cr);
        cairo_set_source_rgba(cr, Theme::CUR_R, Theme::CUR_G, Theme::CUR_B, 0.85);
        cairo_set_line_width(cr, 1.0);
        double dash[] = {4.0, 4.0};
        cairo_set_dash(cr, dash, 2, 0);
        cairo_move_to(cr, marginLeft, curY);
        cairo_line_to(cr, marginLeft + plotW, curY);
        cairo_stroke(cr);
        cairo_restore(cr);

        drawPriceLabel(cr, marginLeft + plotW + 2, curY, current_.close, true);
        drawTitle(cr, width);
    }

private:
    void startNewCandle() {
        double openPrice = history_.empty() ? feed_.tick() : history_.back().close;
        current_ = Candle{openPrice, openPrice, openPrice, openPrice};
        tickInCandle_ = 0;
    }

    void drawCandle(cairo_t* cr, const Candle& c, double cx, double bodyW,
                     const std::function<double(double)>& priceToY,
                     bool rising, bool isForming) const {
        double r = rising ? Theme::UP_R : Theme::DOWN_R;
        double g = rising ? Theme::UP_G : Theme::DOWN_G;
        double b = rising ? Theme::UP_B : Theme::DOWN_B;
        double alpha = isForming ? 0.55 : 1.0; // 正在形成的蜡烛半透明，收盘后变实心，视觉上有"定格"的感觉

        double yHigh = priceToY(c.high);
        double yLow  = priceToY(c.low);
        double yOpen = priceToY(c.open);
        double yClose = priceToY(c.close);
        double yTop = std::min(yOpen, yClose);
        double yBot = std::max(yOpen, yClose);
        if (yBot - yTop < 1.5) { yTop -= 0.75; yBot += 0.75; } // 十字星最小可见高度

        cairo_set_source_rgba(cr, r, g, b, alpha);

        // 上下影线
        cairo_set_line_width(cr, 1.4);
        cairo_move_to(cr, cx, yHigh);
        cairo_line_to(cr, cx, yTop);
        cairo_stroke(cr);
        cairo_move_to(cr, cx, yBot);
        cairo_line_to(cr, cx, yLow);
        cairo_stroke(cr);

        // 实体
        cairo_rectangle(cr, cx - bodyW / 2.0, yTop, bodyW, yBot - yTop);
        cairo_fill(cr);

        if (isForming) {
            // 给正在跳动的这一根加一个描边，提示"未收盘"
            cairo_set_source_rgba(cr, r, g, b, 1.0);
            cairo_set_line_width(cr, 1.2);
            cairo_rectangle(cr, cx - bodyW / 2.0, yTop, bodyW, yBot - yTop);
            cairo_stroke(cr);
        }
    }

    void drawGrid(cairo_t* cr, int marginLeft, [[maybe_unused]] int marginTop, int plotW,
                  [[maybe_unused]] int plotH, double lo, double hi,
                  const std::function<double(double)>& priceToY) const {
        cairo_set_source_rgb(cr, Theme::GRID_R, Theme::GRID_G, Theme::GRID_B);
        cairo_set_line_width(cr, 1.0);
        const int hLines = 5;
        for (int i = 0; i <= hLines; ++i) {
            double price = lo + (hi - lo) * i / hLines;
            double y = priceToY(price);
            cairo_move_to(cr, marginLeft, y);
            cairo_line_to(cr, marginLeft + plotW, y);
            cairo_stroke(cr);
            drawPriceLabel(cr, marginLeft - 6, y, price, false);
        }
    }

    void drawPriceLabel(cairo_t* cr, double x, double y, double price, bool rightAligned) const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << price;
        std::string s = oss.str();

        cairo_set_source_rgb(cr, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B);
        cairo_set_font_size(cr, 12);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, s.c_str(), &ext);
        double tx = rightAligned ? x : x - ext.width;
        cairo_move_to(cr, tx, y + ext.height / 2.0);
        cairo_show_text(cr, s.c_str());
    }

    void drawTitle(cairo_t* cr, [[maybe_unused]] int width) const {
        cairo_set_source_rgb(cr, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B);
        cairo_set_font_size(cr, 14);
        std::string title = "K Line Demo  (simulated intraday feed, red=up / green=down)";
        cairo_move_to(cr, 12, 20);
        cairo_show_text(cr, title.c_str());
    }

    int ticksPerCandle_;
    size_t maxVisible_;
    int tickInCandle_ = 0;
    std::deque<Candle> history_;
    Candle current_;
    MarketFeedSimulator feed_;
};

// ----------------------------------------------------------------------------
// main：X11 建窗 + 固定帧率的事件循环。每帧推进一次行情、重绘一次。
// ----------------------------------------------------------------------------
int main() {
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        std::cerr << "无法打开 X11 Display（如在无图形环境下运行，请配合 Xvfb 使用）\n";
        return 1;
    }

    int screen = DefaultScreen(display);
    int width = 900, height = 520;

    Window window = XCreateSimpleWindow(
        display, RootWindow(display, screen),
        0, 0, width, height, 0,
        BlackPixel(display, screen), BlackPixel(display, screen));

    XStoreName(display, window, "K Line Chart Demo");
    XSelectInput(display, window, ExposureMask | KeyPressMask | StructureNotifyMask);
    XMapWindow(display, window);

    Atom wmDelete = XInternAtom(display, "WM_DELETE_WINDOW", True);
    XSetWMProtocols(display, window, &wmDelete, 1);

    cairo_surface_t* surface = cairo_xlib_surface_create(
        display, window, DefaultVisual(display, screen), width, height);
    cairo_t* cr = cairo_create(surface);

    KLineChart chart(/*ticksPerCandle=*/90, /*maxVisibleCandles=*/40);

    const int fps = 60;
    const auto frameDuration = std::chrono::milliseconds(1000 / fps);
    bool running = true;

    while (running) {
        auto frameStart = std::chrono::steady_clock::now();

        while (XPending(display) > 0) {
            XEvent ev;
            XNextEvent(display, &ev);
            if (ev.type == ConfigureNotify) {
                XConfigureEvent xce = ev.xconfigure;
                if (xce.width != width || xce.height != height) {
                    width = xce.width;
                    height = xce.height;
                    cairo_xlib_surface_set_size(surface, width, height);
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

        chart.update();
        chart.draw(cr, width, height);
        cairo_surface_flush(surface);
        XFlush(display);

        auto elapsed = std::chrono::steady_clock::now() - frameStart;
        auto sleepTime = frameDuration - elapsed;
        if (sleepTime > std::chrono::milliseconds(0)) std::this_thread::sleep_for(sleepTime);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
