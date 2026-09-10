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
// 行情模拟器：用于生成"实体涨跌"与"影线抖动"两层独立的随机性，并支持
// A股涨跌停规则：
//   1) 涨跌停限制：每根蜡烛的价格被限制在"上一根收盘价" ±10% 以内；
//   2) 封板：小概率（约2%涨停/1.5%跌停）判定某根蜡烛强力冲板并锁死走平，
//      形成"光头光脚大阳线（阴线）"式的标志性形态；
//   3) 连板：涨停封板的蜡烛结束后，下一根有一定概率高开直接续封（"连板"），
//      且这个概率随连续天数指数衰减——连得越多，继续连板越难，符合真实
//      市场里长连板远比首次涨停罕见的分布。
//
// 纯对称随机游走有个数学上的通病：N 步随机游走的期望振幅（最高-最低）
// 大约是期望净涨跌（收-开）的 1.6~2 倍左右，且这个比例不随步数 N 变化——
// 简单调参（改 tick 数、改噪声大小）治标不治本。这里改成两层叠加：
//   1) "趋势路径"：开盘时就预先抽一个目标收盘价，K线全程从开盘价向目标
//      收盘价直线过渡（封板剧本下则替换为持续单向强推）；
//   2) "噪声抖动"：AR(1) 平滑噪声叠加在趋势路径上，形成影线。
// ----------------------------------------------------------------------------
class MarketFeedSimulator {
public:
    explicit MarketFeedSimulator(double startPrice)
        : price_(startPrice), limitRef_(startPrice), rng_(std::random_device{}()),
          wickNoise_(0.0, 0.45), trendNoise_(0.0, 1.0), gapChance_(0.0, 1.0),
          bigGapUpDist_(0.055, 0.018) {}

    struct CandlePlan {
        double open = 0.0;
        double target = 0.0;      // 仅普通（非封板）蜡烛使用
        bool isSealAttempt = false;
        bool sealUp = false;      // isSealAttempt 时才有意义：true=冲涨停, false=冲跌停
    };

    // 开新蜡烛时调用
    CandlePlan planCandle(bool isFirst) {
        double limitRef = price_; // 上一根蜡烛的收盘价，即本根蜡烛涨跌停的参照
        double openPrice = limitRef;
        CandlePlan plan;

        if (consecutiveUpStreak_ > 0) {
            // 上一根刚封了涨停：抽一次"是否继续连板"，概率随连续天数指数衰减
            double contProb = 0.42 * std::pow(0.55, consecutiveUpStreak_ - 1);
            if (coinFlip_(rng_) < contProb) {
                double gapPct = std::min(0.099, std::max(0.02, bigGapUpDist_(rng_))); // 明显高开，但不超过新涨停
                openPrice = limitRef * (1.0 + gapPct);
                plan.isSealAttempt = true;
                plan.sealUp = true;
            } else {
                consecutiveUpStreak_ = 0; // 连板中断，之后按正常逻辑走
            }
        }

        if (!isFirst && !plan.isSealAttempt) {
            constexpr double kGapProbability = 0.10;
            if (gapChance_(rng_) < kGapProbability) {
                std::normal_distribution<double> gapDist(0.0, limitRef * 0.006);
                openPrice = std::max(1.0, limitRef + gapDist(rng_));
            }
        }

        if (!plan.isSealAttempt) {
            double roll = coinFlip_(rng_);
            if (roll < 0.02) { plan.isSealAttempt = true; plan.sealUp = true; }        // ~2% 涨停封板
            else if (roll < 0.035) { plan.isSealAttempt = true; plan.sealUp = false; }  // ~1.5% 跌停封板
        }

        openPrice = std::min(limitRef * 1.10, std::max(limitRef * 0.90, openPrice));
        plan.open = openPrice;
        plan.target = plan.isSealAttempt ? openPrice
                                          : std::min(limitRef * 1.10, std::max(limitRef * 0.90,
                                                std::max(1.0, openPrice + trendNoise_(rng_))));

        price_ = openPrice;
        limitRef_ = limitRef;
        momentum_ = 0.0;
        sealAttempt_ = plan.isSealAttempt;
        sealUp_ = plan.sealUp;
        sealed_ = false;
        return plan;
    }

    // 每一帧调用：正常蜡烛在 basePath 附近叠加噪声；封板蜡烛走"强推→锁死"逻辑
    double jitterAround(double basePath) {
        double hi = limitRef_ * 1.10, lo = limitRef_ * 0.90;

        if (sealAttempt_) {
            if (sealed_) { price_ = sealUp_ ? hi : lo; return price_; } // 已摸板：锁死走平

            // 推力必须按参照价的比例计算，不能用固定绝对值：价格经过长期漂移后
            // 绝对数值的推力相对于"10%的距离"会变得微不足道，导致摸不到板
            // （这是写统计测试时才发现的真实bug：固定用0.38时，90个tick内的
            // 摸板成功率不到1%，换成比例后才稳定摸到板）。
            double pushSign = sealUp_ ? 1.0 : -1.0;
            double pushMag = limitRef_ * 0.0042;
            momentum_ = momentum_ * 0.75 + wickNoise_(rng_) * 0.25 * (limitRef_ * 0.0045);
            price_ = std::min(hi, std::max(lo, price_ + pushSign * pushMag + momentum_));

            if ((sealUp_ && price_ >= hi - 1e-6) || (!sealUp_ && price_ <= lo + 1e-6)) sealed_ = true;
            return price_;
        }

        momentum_ = momentum_ * 0.80 + wickNoise_(rng_) * 0.20;
        price_ = std::min(hi, std::max(lo, basePath + momentum_));
        return price_;
    }

    // 蜡烛收盘时调用：更新连板计数，返回这根蜡烛是否封了涨停（供外部判断/显示用）
    bool finalizeCandle() {
        bool sealedUp = sealAttempt_ && sealed_ && sealUp_;
        consecutiveUpStreak_ = sealedUp ? consecutiveUpStreak_ + 1 : 0;
        return sealedUp;
    }

    bool isSealedNow() const { return sealAttempt_ && sealed_; }
    bool isSealedUpNow() const { return sealAttempt_ && sealed_ && sealUp_; }
    int consecutiveUpStreak() const { return consecutiveUpStreak_; }

private:
    double price_;
    double limitRef_;
    double momentum_ = 0.0;
    bool sealAttempt_ = false, sealUp_ = false, sealed_ = false;
    int consecutiveUpStreak_ = 0;

    std::mt19937 rng_;
    std::normal_distribution<double> wickNoise_;
    std::normal_distribution<double> trendNoise_;
    std::uniform_real_distribution<double> gapChance_;
    std::normal_distribution<double> bigGapUpDist_;
    std::uniform_real_distribution<double> coinFlip_{0.0, 1.0};
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

    // 每一帧调用一次：沿"趋势路径"推进一步并叠加噪声抖动，更新"正在形成的 K 线"
    void update() {
        double progress = static_cast<double>(tickInCandle_ + 1) / ticksPerCandle_;
        double basePath = candleOpen_ + (candleTarget_ - candleOpen_) * progress;
        double p = feed_.jitterAround(basePath);

        current_.high  = std::max(current_.high, p);
        current_.low   = std::min(current_.low, p);
        current_.close = p;
        ++tickInCandle_;

        if (tickInCandle_ >= ticksPerCandle_) {
            feed_.finalizeCandle();
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

        int streak = feed_.consecutiveUpStreak();
        if (streak >= 2) {
            cairo_set_source_rgb(cr, Theme::UP_R, Theme::UP_G, Theme::UP_B);
            cairo_set_font_size(cr, 13);
            std::ostringstream oss;
            oss << streak << " 连板";
            cairo_text_extents_t ext;
            cairo_text_extents(cr, oss.str().c_str(), &ext);
            cairo_move_to(cr, width - ext.width - 14, 22);
            cairo_show_text(cr, oss.str().c_str());
        }
    }

private:
    void startNewCandle() {
        bool isFirst = history_.empty();
        auto plan = feed_.planCandle(isFirst);
        candleOpen_ = plan.open;
        candleTarget_ = plan.target;
        current_ = Candle{plan.open, plan.open, plan.open, plan.open};
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
    double candleOpen_ = 0.0;
    double candleTarget_ = 0.0;
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
    int depth = DefaultDepth(display, screen);

    Window window = XCreateSimpleWindow(
        display, RootWindow(display, screen),
        0, 0, width, height, 0,
        BlackPixel(display, screen), BlackPixel(display, screen));

    XStoreName(display, window, "K Line Chart Demo");
    XSelectInput(display, window, ExposureMask | KeyPressMask | StructureNotifyMask);
    XMapWindow(display, window);

    Atom wmDelete = XInternAtom(display, "WM_DELETE_WINDOW", True);
    XSetWMProtocols(display, window, &wmDelete, 1);

    GC gc = XCreateGC(display, window, 0, nullptr);

    // 离屏缓冲改用 X11 Pixmap（服务端资源），配合 XCopyArea 做双缓冲：
    // 每帧先把完整画面画到这块 Pixmap 上（Cairo 通过 xlib 后端直接对接 X11 绘图原语），
    // 画完后用 XCopyArea 一次性拷到窗口——这一步是 X Server 内部的服务端拷贝，
    // 比之前"客户端内存画布再整体贴回"的方式开销更低，窗口越大（比如全屏/4K）差距越明显。
    Pixmap backPixmap = XCreatePixmap(display, window, width, height, depth);
    cairo_surface_t* backbuffer = cairo_xlib_surface_create(
        display, backPixmap, DefaultVisual(display, screen), width, height);
    cairo_t* backCr = cairo_create(backbuffer);

    KLineChart chart(/*ticksPerCandle=*/90, /*maxVisibleCandles=*/40);

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
            // 窗口尺寸变了，Pixmap 必须按新尺寸重建（X11 Pixmap 大小不可变）
            cairo_destroy(backCr);
            cairo_surface_destroy(backbuffer);
            XFreePixmap(display, backPixmap);
            backPixmap = XCreatePixmap(display, window, width, height, depth);
            backbuffer = cairo_xlib_surface_create(
                display, backPixmap, DefaultVisual(display, screen), width, height);
            backCr = cairo_create(backbuffer);
        }

        chart.update();
        chart.draw(backCr, width, height);      // 完整画一帧到 Pixmap（不上屏）
        cairo_surface_flush(backbuffer);

        XCopyArea(display, backPixmap, window, gc, 0, 0, width, height, 0, 0); // 服务端整帧拷贝上屏
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
