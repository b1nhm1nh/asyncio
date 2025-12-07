#ifndef ASYNCIO_CANDLE_MANAGER_H
#define ASYNCIO_CANDLE_MANAGER_H

#include <asyncio/candle/candle_builder.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace asyncio::candle {

/**
 * Manages multiple CandleBuilders for multi-symbol, multi-interval candle generation.
 */
class CandleManager {
public:
    using Callback = std::function<void(const std::string& symbol, int64_t intervalMs, const Candle&)>;

    explicit CandleManager(Callback callback = {}, bool emitEmpty = true)
        : callback_{std::move(callback)}
        , emitEmpty_{emitEmpty}
    {}

    /**
     * Subscribe to candle generation for a symbol with given intervals.
     */
    void subscribe(const std::string& symbol, std::vector<int64_t> intervals) {
        auto& builders = builders_[symbol];
        for (int64_t intervalMs : intervals) {
            builders.push_back(std::make_unique<CandleBuilder>(
                intervalMs,
                [this, symbol, intervalMs](const Candle& c) {
                    if (callback_) {
                        callback_(symbol, intervalMs, c);
                    }
                },
                emitEmpty_
            ));
        }
    }

    /**
     * Unsubscribe from a symbol, flushing any pending candles.
     */
    void unsubscribe(const std::string& symbol) {
        auto it = builders_.find(symbol);
        if (it != builders_.end()) {
            for (auto& builder : it->second) {
                builder->flush();
            }
            builders_.erase(it);
        }
    }

    /**
     * Process incoming trade.
     */
    void onTrade(const std::string& symbol, double price, double qty, int64_t timestampMs) {
        auto it = builders_.find(symbol);
        if (it != builders_.end()) {
            for (auto& builder : it->second) {
                builder->onTrade(price, qty, timestampMs);
            }
        }
    }

    /**
     * Heartbeat tick for all builders.
     */
    void tick(int64_t currentTimeMs) {
        for (auto& [symbol, builders] : builders_) {
            for (auto& builder : builders) {
                builder->tick(currentTimeMs);
            }
        }
    }

    /**
     * Flush all pending candles.
     */
    void flush() {
        for (auto& [symbol, builders] : builders_) {
            for (auto& builder : builders) {
                builder->flush();
            }
        }
    }

private:
    Callback callback_;
    bool emitEmpty_;
    std::unordered_map<std::string, std::vector<std::unique_ptr<CandleBuilder>>> builders_;
};

} // namespace asyncio::candle

#endif // ASYNCIO_CANDLE_MANAGER_H
