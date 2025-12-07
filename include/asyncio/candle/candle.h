#ifndef ASYNCIO_CANDLE_H
#define ASYNCIO_CANDLE_H

#include <cstdint>
#include <algorithm>

namespace asyncio::candle {

/**
 * OHLCV Candle structure - generic, exchange-agnostic.
 */
struct Candle {
    int64_t openTimeMs{0};
    int64_t closeTimeMs{0};
    double open{0};
    double high{0};
    double low{0};
    double close{0};
    double volume{0};
    double quoteVolume{0};
    int64_t tradeCount{0};

    /**
     * Update candle with a new trade.
     */
    void update(double price, double qty) {
        if (tradeCount == 0) {
            open = high = low = close = price;
        } else {
            high = std::max(high, price);
            low = std::min(low, price);
            close = price;
        }
        volume += qty;
        quoteVolume += price * qty;
        ++tradeCount;
    }

    /**
     * Check if candle has any trades.
     */
    [[nodiscard]] bool isEmpty() const {
        return tradeCount == 0;
    }

    /**
     * Reset candle for new interval.
     */
    void reset(int64_t newOpenTime, int64_t intervalMs) {
        openTimeMs = newOpenTime;
        closeTimeMs = newOpenTime + intervalMs;
        open = high = low = close = 0;
        volume = quoteVolume = 0;
        tradeCount = 0;
    }

    /**
     * Reset candle with last close price for continuity.
     * Sets OHLC all to lastClose for empty candle representation.
     */
    void resetWithLastClose(int64_t newOpenTime, int64_t intervalMs, double lastClose) {
        reset(newOpenTime, intervalMs);
        open = high = low = close = lastClose;
    }

    /**
     * Merge another candle into this one.
     */
    void merge(const Candle& other) {
        if (other.isEmpty()) return;
        if (isEmpty()) {
            *this = other;
            return;
        }
        high = std::max(high, other.high);
        low = std::min(low, other.low);
        close = other.close;
        volume += other.volume;
        quoteVolume += other.quoteVolume;
        tradeCount += other.tradeCount;
    }
};

} // namespace asyncio::candle

#endif // ASYNCIO_CANDLE_H
