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
// 核心生成方式是"风向切换的连续随机游走"：不预设任何目标点，每一分钟的
// 价格 = 上一分钟价格 + (当前风向力度 + 随机噪声)。"风向"代表某个时间段
// 内多头或空头占优，持续 6~22 分钟不等后随机重新抽一次——可能延续、可能
// 反手、可能转入整理，全部由随机数自己决定。此外：
//   1) 开盘剧烈波动：每个交易时段刚开始的几分钟，风向力度与噪声一起放大；
//   2) 上午/下午独立：风向状态在午间重新开盘时清空重抽；
//   3) 涨跌停限制 + 更真实的开盘/午间缺口；
//   4) 涨跌停封板（小概率）：每天开盘前先掷一次骰子，小概率判定"今天涨停
//      封死"或"今天跌停封死"——一旦命中，会用持续单向的强力推动把价格推
//      向对应的涨跌停板，摸到板后价格锁死走平（真实封板后基本没有像样的
//      成交波动），并有一定概率中途"炸板"重新打开、恢复自由波动。
// ----------------------------------------------------------------------------
class FiveDayFeedSimulator {
public:
    explicit FiveDayFeedSimulator(int minutesPerDay)
        : minutesPerDay_(minutesPerDay), sessionLen_(minutesPerDay / 2),
          rng_(std::random_device{}()),
          minuteNoise_(0.0, 0.16), biasMagnitudeDist_(0.0, 0.05),
          regimeDurationDist_(6, 22), coinFlip_(0.0, 1.0),
          smallGapNoise_(0.0, 0.0015), bigGapNoise_(0.0, 0.010),
          lunchGapNoise_(0.0, 0.0008), gapChance_(0.0, 1.0) {}

    // 每天开盘时调用：返回"考虑了集合竞价缺口之后"的真实开盘价
    double planDayOpen(double prevClose) {
        double gapPct = smallGapNoise_(rng_);
        if (gapChance_(rng_) < 0.12) gapPct += bigGapNoise_(rng_);
        return clampToLimit(prevClose * (1.0 + gapPct), prevClose);
    }

    // 生成一整天已经走完的历史数据（用于前4天）
    DayData generateCompletedDay(double prevClose, double dayOpen, const std::string& label) {
        DayData day;
        day.prevClose = prevClose;
        day.label = label;

        DayEvent event = planDayEvent();
        double price = dayOpen;
        double momentum = 0.0;
        Regime regime{0.0, 0};
        bool sealed = false;
        bool eventExhausted = false;

        for (int s = 0; s < 2; ++s) {
            if (s == 1) price = clampToLimit(price + price * lunchGapNoise_(rng_), prevClose);
            for (int m = 0; m < sessionLen_; ++m) {
                int dayMinute = s * sessionLen_ + m;
                double vol = phaseVolatility(m);
                stepOneMinute(price, momentum, regime, sealed, eventExhausted, event, dayMinute, vol, prevClose);
                day.prices.push_back(price);
                day.volumes.push_back(sealed ? volumeFor(vol) * 0.15 : volumeFor(vol)); // 封板后成交明显萎缩
            }
        }
        return day;
    }

    // 开始新的一天（用于正在实时跳动的"今天"）
    void startLiveDay(double prevClose, double dayOpen) {
        livePrevClose_ = prevClose;
        livePrice_ = dayOpen;
        liveMomentum_ = 0.0;
        liveRegime_ = Regime{0.0, 0};
        liveSealed_ = false;
        liveEventExhausted_ = false;
        liveEvent_ = planDayEvent();
        liveSessionIdx_ = 0;
        liveMinuteInSession_ = 0;
        liveMinuteTotal_ = 0;   // 【重要】必须在这里归零，否则第一天结束后
                                // liveDayFinished() 会永远为 true，导致画面
                                // 在第一天走完后不断被清空重建、始终没有数据。
    }

    // 每帧调用：向"今天"推进一分钟
    double nextLiveMinutePrice() {
        if (liveMinuteInSession_ >= sessionLen_) {
            liveSessionIdx_ = 1;
            liveMinuteInSession_ = 0;
            livePrice_ = clampToLimit(livePrice_ + livePrice_ * lunchGapNoise_(rng_), livePrevClose_);
            liveMomentum_ = 0.0;
            liveRegime_ = Regime{0.0, 0};
        }
        int dayMinute = liveSessionIdx_ * sessionLen_ + liveMinuteInSession_;
        double vol = phaseVolatility(liveMinuteInSession_);
        stepOneMinute(livePrice_, liveMomentum_, liveRegime_, liveSealed_, liveEventExhausted_,
                      liveEvent_, dayMinute, vol, livePrevClose_);
        ++liveMinuteInSession_;
        ++liveMinuteTotal_;
        return livePrice_;
    }

    double liveVolatilityNow() const { return phaseVolatility(std::max(0, liveMinuteInSession_ - 1)); }
    bool liveDayFinished() const { return liveMinuteTotal_ >= minutesPerDay_; }
    bool liveIsSealed() const { return liveSealed_; }

    double volumeFor(double volMultiplier) const {
        std::uniform_real_distribution<double> volNoise(0.3, 1.0);
        return volMultiplier * volNoise(const_cast<std::mt19937&>(rng_));
    }

    double phaseVolatility(int minuteInSession) const {
        double openZoneEnd = sessionLen_ * 0.07;
        double closeZoneStart = sessionLen_ * 0.85;
        if (minuteInSession < openZoneEnd) {
            double t = minuteInSession / openZoneEnd;
            return 3.0 - 1.6 * t;
        } else if (minuteInSession > closeZoneStart) {
            double t = (minuteInSession - closeZoneStart) / (sessionLen_ - closeZoneStart);
            return 1.0 + 0.8 * t;
        }
        return 0.45;
    }

private:
    struct Regime { double biasPerMinute; int remaining; };
    enum class DayEventType { NONE, LIMIT_UP, LIMIT_DOWN };
    struct DayEvent {
        DayEventType type = DayEventType::NONE;
        int unsealMinute = -1; // -1 表示一旦封板就锁到收盘；否则表示炸板重新打开的分钟数
    };

    // 每天开盘前掷一次骰子：小概率判定今天涨停/跌停封板
    DayEvent planDayEvent() {
        DayEvent ev;
        double roll = coinFlip_(rng_);
        if (roll < 0.035) ev.type = DayEventType::LIMIT_UP;         // 3.5% 概率涨停
        else if (roll < 0.06) ev.type = DayEventType::LIMIT_DOWN;   // 再 2.5% 概率跌停
        if (ev.type != DayEventType::NONE && coinFlip_(rng_) < 0.35) { // 35%概率中途炸板重新打开
            std::uniform_int_distribution<int> unsealDist(
                static_cast<int>(minutesPerDay_ * 0.5), static_cast<int>(minutesPerDay_ * 0.92));
            ev.unsealMinute = unsealDist(rng_);
        }
        return ev;
    }

    Regime rollRegime() {
        Regime r;
        r.remaining = regimeDurationDist_(rng_);
        double mag = std::abs(biasMagnitudeDist_(rng_));
        double sign = (coinFlip_(rng_) < 0.5) ? -1.0 : 1.0;
        r.biasPerMinute = sign * mag;
        return r;
    }

    // 推进一分钟的核心逻辑：正常随机游走 / 封板前的单向强推 / 封板后走平，
    // 三种状态统一在这里处理，历史数据生成和实时推进都调用同一份实现，
    // 避免两边逻辑分叉出差异。eventExhausted 标记"这次涨跌停剧本是否已经
    // 用完"——炸板打开之后必须置为 true，否则解封瞬间价格还停在板上，
    // 强推逻辑会立刻把它推回板上，导致"炸板"一帧都看不见就又封死了。
    void stepOneMinute(double& price, double& momentum, Regime& regime, bool& sealed,
                        bool& eventExhausted, const DayEvent& event, int dayMinute, double vol,
                        double prevClose) {
        double hi = prevClose * 1.10, lo = prevClose * 0.90;

        if (sealed) {
            if (event.unsealMinute >= 0 && dayMinute >= event.unsealMinute) {
                sealed = false;          // 炸板：重新恢复自由波动
                eventExhausted = true;   // 这次涨跌停剧本到此结束，之后不再朝板上强推
                momentum = 0.0;
                regime = Regime{0.0, 0};
            } else {
                price = (event.type == DayEventType::LIMIT_UP) ? hi : lo; // 封死在板上，完全走平
                return;
            }
        }

        if (regime.remaining <= 0) regime = rollRegime();
        --regime.remaining;
        double bias = regime.biasPerMinute;

        bool stillPushingToSeal = (event.type != DayEventType::NONE) && !eventExhausted;
        if (stillPushingToSeal) {
            // 处于"今天要封板"的剧本里，且尚未摸到板：用持续的强力单向推动
            // 取代普通的双向随机风向，让价格有把握地朝板上走，而不是随机游走
            double pushSign = (event.type == DayEventType::LIMIT_UP) ? 1.0 : -1.0;
            bias = pushSign * 0.22;
        }

        momentum = momentum * 0.72 + minuteNoise_(rng_) * 0.28 * vol;
        double step = bias * vol + momentum;
        price = std::min(hi, std::max(lo, std::max(1.0, price + step)));

        if (stillPushingToSeal &&
            ((event.type == DayEventType::LIMIT_UP && price >= hi - 1e-6) ||
             (event.type == DayEventType::LIMIT_DOWN && price <= lo + 1e-6))) {
            sealed = true; // 摸到板了，锁死
        }
    }

    double clampToLimit(double price, double prevClose) const {
        double hi = prevClose * 1.10, lo = prevClose * 0.90;
        return std::min(hi, std::max(lo, std::max(1.0, price)));
    }

    int minutesPerDay_, sessionLen_;
    mutable std::mt19937 rng_;
    std::normal_distribution<double> minuteNoise_, biasMagnitudeDist_;
    std::uniform_int_distribution<int> regimeDurationDist_;
    std::uniform_real_distribution<double> coinFlip_;
    std::normal_distribution<double> smallGapNoise_, bigGapNoise_, lunchGapNoise_;
    std::uniform_real_distribution<double> gapChance_;

    double livePrevClose_ = 0.0, livePrice_ = 0.0, liveMomentum_ = 0.0;
    Regime liveRegime_{0.0, 0};
    bool liveSealed_ = false;
    bool liveEventExhausted_ = false;
    DayEvent liveEvent_;
    int liveSessionIdx_ = 0, liveMinuteInSession_ = 0, liveMinuteTotal_ = 0;
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

