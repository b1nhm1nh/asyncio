#ifndef ASYNCIO_CANDLE_BUILDER_H
#define ASYNCIO_CANDLE_BUILDER_H

#include <asyncio/candle/candle.h>
#include <functional>

namespace asyncio::candle {

/**
 * Builds OHLCV candles from a trade stream for a single interval.
 *
 * Supports two modes of candle closing:
 * 1. Trade-triggered: When a trade arrives for a new interval
 * 2. Heartbeat-triggered: When tick() is called and current interval has expired
 */
class CandleBuilder {
public:
    using Callback = std::function<void(const Candle&)>;

    /**
     * Construct a CandleBuilder.
     *
     * @param intervalMs  Candle interval in milliseconds (1000 = 1s, 60000 = 1m)
     * @param callback    Called when a candle is completed
     * @param emitEmpty   If true, emit candles with zero volume during gaps
     */
    explicit CandleBuilder(int64_t intervalMs, Callback callback = {}, bool emitEmpty = true)
        : intervalMs_{intervalMs}
        , callback_{std::move(callback)}
        , emitEmpty_{emitEmpty}
        , started_{false}
    {}

    /**
     * Process incoming trade - O(1).
     */
    void onTrade(double price, double qty, int64_t timestampMs) {
        const int64_t intervalStart = alignToInterval(timestampMs);

        if (!started_) {
            current_.reset(intervalStart, intervalMs_);
            started_ = true;
        } else if (intervalStart > current_.openTimeMs) {
            emitCandlesUpTo(intervalStart);
        }

        current_.update(price, qty);
        lastTradeTimeMs_ = timestampMs;
    }

    /**
     * Heartbeat tick - call periodically to close candles during inactivity.
     */
    void tick(int64_t currentTimeMs) {
        if (!started_) {
            return;
        }

        const int64_t intervalStart = alignToInterval(currentTimeMs);

        if (intervalStart > current_.openTimeMs) {
            emitCandlesUpTo(intervalStart);
        }
    }

    /**
     * Force emit current candle (for shutdown/cleanup).
     */
    void flush() {
        if (started_ && !current_.isEmpty() && callback_) {
            callback_(current_);
        }
    }

    const Candle& current() const { return current_; }
    int64_t intervalMs() const { return intervalMs_; }
    int64_t lastTradeTimeMs() const { return lastTradeTimeMs_; }
    double lastClose() const { return lastClose_; }

private:
    int64_t alignToInterval(int64_t timestampMs) const {
        return (timestampMs / intervalMs_) * intervalMs_;
    }

    void emitCandlesUpTo(int64_t targetIntervalStart) {
        if (callback_ && (!current_.isEmpty() || emitEmpty_)) {
            callback_(current_);
        }

        if (!current_.isEmpty()) {
            lastClose_ = current_.close;
        }

        int64_t nextInterval = current_.openTimeMs + intervalMs_;
        while (nextInterval < targetIntervalStart) {
            if (emitEmpty_ && callback_) {
                Candle empty;
                empty.resetWithLastClose(nextInterval, intervalMs_, lastClose_);
                callback_(empty);
            }
            nextInterval += intervalMs_;
        }

        current_.resetWithLastClose(targetIntervalStart, intervalMs_, lastClose_);
    }

    int64_t intervalMs_;
    Candle current_;
    Callback callback_;
    bool emitEmpty_;
    bool started_;
    int64_t lastTradeTimeMs_{0};
    double lastClose_{0};
};

} // namespace asyncio::candle

#endif // ASYNCIO_CANDLE_BUILDER_H
