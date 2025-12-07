"""
Binary Candle Client for Redis and Memcached

This module provides Python clients to read/write candles stored in binary format,
compatible with the C++ asyncio library's BinaryCandle struct.

Binary Format (48 bytes, little-endian):
    - timestamp: int64 (8 bytes)
    - open: double (8 bytes)
    - high: double (8 bytes)
    - low: double (8 bytes)
    - close: double (8 bytes)
    - volume: double (8 bytes)

Usage:
    # Redis
    from binary_candle_client import RedisCandleClient, BinaryCandle

    async with RedisCandleClient() as client:
        # Store candle
        candle = BinaryCandle(timestamp=1733580000000, open=100.0, high=101.0,
                              low=99.0, close=100.5, volume=1000.0)
        await client.store_candle("BTCUSDT", "1s", candle)

        # Get latest candles
        candles = await client.get_candles("BTCUSDT", "1s", count=100)

    # Memcached
    from binary_candle_client import MemcachedCandleClient

    async with MemcachedCandleClient() as client:
        await client.store_candle("BTCUSDT", "1s", candle)
        candle = await client.get_candle("BTCUSDT", "1s", timestamp)
"""

import struct
import asyncio
from dataclasses import dataclass
from typing import Optional, List, Dict, Any
from datetime import datetime

# Binary format: little-endian int64 + 5 doubles = 48 bytes
CANDLE_FORMAT = '<q5d'
CANDLE_SIZE = 48


@dataclass
class BinaryCandle:
    """Binary-compatible candle structure (48 bytes)."""
    timestamp: int      # milliseconds since epoch
    open: float
    high: float
    low: float
    close: float
    volume: float

    def pack(self) -> bytes:
        """Pack candle to binary format (48 bytes)."""
        return struct.pack(CANDLE_FORMAT,
                          self.timestamp, self.open, self.high,
                          self.low, self.close, self.volume)

    @classmethod
    def unpack(cls, data: bytes) -> 'BinaryCandle':
        """Unpack candle from binary format."""
        if len(data) != CANDLE_SIZE:
            raise ValueError(f"Expected {CANDLE_SIZE} bytes, got {len(data)}")
        ts, o, h, l, c, v = struct.unpack(CANDLE_FORMAT, data)
        return cls(timestamp=ts, open=o, high=h, low=l, close=c, volume=v)

    @property
    def datetime(self) -> datetime:
        """Convert timestamp to datetime."""
        return datetime.fromtimestamp(self.timestamp / 1000)

    def to_dict(self) -> Dict[str, Any]:
        """Convert to dictionary."""
        return {
            'time': self.timestamp,
            'open': self.open,
            'high': self.high,
            'low': self.low,
            'close': self.close,
            'volume': self.volume
        }

    def __repr__(self):
        return (f"BinaryCandle(ts={self.timestamp}, "
                f"o={self.open:.2f}, h={self.high:.2f}, "
                f"l={self.low:.2f}, c={self.close:.2f}, v={self.volume:.0f})")


class RedisCandleClient:
    """
    Redis client for binary candle storage using Sorted Sets.

    Key pattern: candle:{symbol}:{timeframe}
    Score: timestamp (for ordering)
    Value: binary candle data (48 bytes)
    """

    def __init__(self, host: str = '127.0.0.1', port: int = 6379,
                 max_candles: int = 1000, key_prefix: str = 'candle'):
        self.host = host
        self.port = port
        self.max_candles = max_candles
        self.key_prefix = key_prefix
        self._redis = None

    async def __aenter__(self):
        import redis.asyncio as aioredis
        self._redis = aioredis.Redis(host=self.host, port=self.port)
        return self

    async def __aexit__(self, exc_type, exc_val, exc_tb):
        if self._redis:
            await self._redis.close()

    def _build_key(self, symbol: str, timeframe: str) -> str:
        """Build Redis key for candle storage."""
        return f"{self.key_prefix}:{symbol}:{timeframe}"

    async def store_candle(self, symbol: str, timeframe: str,
                          candle: BinaryCandle) -> bool:
        """Store a single candle in Redis Sorted Set."""
        key = self._build_key(symbol, timeframe)

        # ZADD with score=timestamp, value=binary data
        await self._redis.zadd(key, {candle.pack(): candle.timestamp})

        # Trim to max_candles (keep newest)
        count = await self._redis.zcard(key)
        if count > self.max_candles:
            await self._redis.zremrangebyrank(key, 0, count - self.max_candles - 1)

        return True

    async def store_candles(self, symbol: str, timeframe: str,
                           candles: List[BinaryCandle]) -> int:
        """Store multiple candles efficiently."""
        key = self._build_key(symbol, timeframe)

        # Build mapping: {binary_data: score}
        mapping = {c.pack(): c.timestamp for c in candles}

        # ZADD all at once
        await self._redis.zadd(key, mapping)

        # Trim
        count = await self._redis.zcard(key)
        if count > self.max_candles:
            await self._redis.zremrangebyrank(key, 0, count - self.max_candles - 1)

        return len(candles)

    async def get_candles(self, symbol: str, timeframe: str,
                         count: int = 100) -> List[BinaryCandle]:
        """Get latest N candles from Redis."""
        key = self._build_key(symbol, timeframe)

        # ZRANGE with negative indices gets newest
        data = await self._redis.zrange(key, -count, -1)

        return [BinaryCandle.unpack(d) for d in data]

    async def get_candles_range(self, symbol: str, timeframe: str,
                               start_ts: int, end_ts: int) -> List[BinaryCandle]:
        """Get candles in timestamp range."""
        key = self._build_key(symbol, timeframe)

        # ZRANGEBYSCORE for time range
        data = await self._redis.zrangebyscore(key, start_ts, end_ts)

        return [BinaryCandle.unpack(d) for d in data]

    async def get_latest_candle(self, symbol: str, timeframe: str) -> Optional[BinaryCandle]:
        """Get the most recent candle."""
        key = self._build_key(symbol, timeframe)

        data = await self._redis.zrange(key, -1, -1)
        if data:
            return BinaryCandle.unpack(data[0])
        return None

    async def get_candle_count(self, symbol: str, timeframe: str) -> int:
        """Get number of stored candles."""
        key = self._build_key(symbol, timeframe)
        return await self._redis.zcard(key)

    async def delete_candles(self, symbol: str, timeframe: str) -> bool:
        """Delete all candles for a symbol/timeframe."""
        key = self._build_key(symbol, timeframe)
        await self._redis.delete(key)
        return True

    async def get_memory_usage(self, symbol: str, timeframe: str) -> int:
        """Get memory used by this key (requires Redis 4.0+)."""
        key = self._build_key(symbol, timeframe)
        try:
            return await self._redis.memory_usage(key) or 0
        except Exception:
            return 0


class MemcachedCandleClient:
    """
    Memcached client for binary candle storage.

    Key pattern: candle:{symbol}:{timeframe}:{timestamp}
    Value: binary candle data (48 bytes)

    For batch storage/retrieval, uses timestamp-based keys.
    """

    def __init__(self, host: str = '127.0.0.1', port: int = 11211,
                 max_candles: int = 1000, key_prefix: str = 'candle'):
        self.host = host
        self.port = port
        self.max_candles = max_candles
        self.key_prefix = key_prefix
        self._client = None

    async def __aenter__(self):
        from aiomcache import Client
        self._client = Client(self.host, self.port)
        return self

    async def __aexit__(self, exc_type, exc_val, exc_tb):
        if self._client:
            await self._client.close()

    def _build_key(self, symbol: str, timeframe: str, timestamp: int) -> bytes:
        """Build Memcached key for single candle."""
        return f"{self.key_prefix}:{symbol}:{timeframe}:{timestamp}".encode()

    def _build_index_key(self, symbol: str, timeframe: str) -> bytes:
        """Build key for timestamp index."""
        return f"{self.key_prefix}:idx:{symbol}:{timeframe}".encode()

    async def store_candle(self, symbol: str, timeframe: str,
                          candle: BinaryCandle, exptime: int = 0) -> bool:
        """Store a single candle."""
        key = self._build_key(symbol, timeframe, candle.timestamp)
        await self._client.set(key, candle.pack(), exptime=exptime)

        # Update index (store list of timestamps)
        await self._update_index(symbol, timeframe, candle.timestamp)

        return True

    async def _update_index(self, symbol: str, timeframe: str, timestamp: int):
        """Update timestamp index for a symbol/timeframe."""
        idx_key = self._build_index_key(symbol, timeframe)

        # Get existing index
        data = await self._client.get(idx_key)
        if data:
            # Index is packed as array of int64
            count = len(data) // 8
            timestamps = list(struct.unpack(f'<{count}q', data))
        else:
            timestamps = []

        # Add new timestamp if not exists
        if timestamp not in timestamps:
            timestamps.append(timestamp)
            timestamps.sort()

            # Trim to max_candles
            if len(timestamps) > self.max_candles:
                # Remove oldest
                to_remove = timestamps[:-self.max_candles]
                timestamps = timestamps[-self.max_candles:]

                # Delete old candle keys
                for ts in to_remove:
                    old_key = self._build_key(symbol, timeframe, ts)
                    await self._client.delete(old_key)

        # Store updated index
        packed = struct.pack(f'<{len(timestamps)}q', *timestamps)
        await self._client.set(idx_key, packed)

    async def get_candle(self, symbol: str, timeframe: str,
                        timestamp: int) -> Optional[BinaryCandle]:
        """Get a single candle by timestamp."""
        key = self._build_key(symbol, timeframe, timestamp)
        data = await self._client.get(key)

        if data:
            return BinaryCandle.unpack(data)
        return None

    async def get_candles(self, symbol: str, timeframe: str,
                         count: int = 100) -> List[BinaryCandle]:
        """Get latest N candles."""
        idx_key = self._build_index_key(symbol, timeframe)

        # Get index
        data = await self._client.get(idx_key)
        if not data:
            return []

        # Unpack timestamps
        ts_count = len(data) // 8
        timestamps = list(struct.unpack(f'<{ts_count}q', data))

        # Get latest N
        latest = timestamps[-count:] if count < len(timestamps) else timestamps

        # Fetch candles
        candles = []
        for ts in latest:
            candle = await self.get_candle(symbol, timeframe, ts)
            if candle:
                candles.append(candle)

        return candles

    async def get_candles_multi(self, symbol: str, timeframe: str,
                               timestamps: List[int]) -> List[BinaryCandle]:
        """Get multiple candles by timestamps (batch)."""
        keys = [self._build_key(symbol, timeframe, ts) for ts in timestamps]

        candles = []
        # Note: aiomcache doesn't have native multi-get, so we batch manually
        for key in keys:
            data = await self._client.get(key)
            if data:
                candles.append(BinaryCandle.unpack(data))

        return candles

    async def delete_candles(self, symbol: str, timeframe: str) -> int:
        """Delete all candles for a symbol/timeframe."""
        idx_key = self._build_index_key(symbol, timeframe)

        # Get index
        data = await self._client.get(idx_key)
        if not data:
            return 0

        # Delete all candle keys
        ts_count = len(data) // 8
        timestamps = struct.unpack(f'<{ts_count}q', data)

        deleted = 0
        for ts in timestamps:
            key = self._build_key(symbol, timeframe, ts)
            await self._client.delete(key)
            deleted += 1

        # Delete index
        await self._client.delete(idx_key)

        return deleted


# Convenience functions for simple usage
async def store_candle_redis(symbol: str, timeframe: str, candle: BinaryCandle,
                            host: str = '127.0.0.1', port: int = 6379) -> bool:
    """Store single candle to Redis."""
    async with RedisCandleClient(host, port) as client:
        return await client.store_candle(symbol, timeframe, candle)


async def get_candles_redis(symbol: str, timeframe: str, count: int = 100,
                           host: str = '127.0.0.1', port: int = 6379) -> List[BinaryCandle]:
    """Get candles from Redis."""
    async with RedisCandleClient(host, port) as client:
        return await client.get_candles(symbol, timeframe, count)


async def store_candle_memcached(symbol: str, timeframe: str, candle: BinaryCandle,
                                host: str = '127.0.0.1', port: int = 11211) -> bool:
    """Store single candle to Memcached."""
    async with MemcachedCandleClient(host, port) as client:
        return await client.store_candle(symbol, timeframe, candle)


async def get_candles_memcached(symbol: str, timeframe: str, count: int = 100,
                               host: str = '127.0.0.1', port: int = 11211) -> List[BinaryCandle]:
    """Get candles from Memcached."""
    async with MemcachedCandleClient(host, port) as client:
        return await client.get_candles(symbol, timeframe, count)


# Example usage and test
async def main():
    import time

    print("=" * 60)
    print("Binary Candle Client Test")
    print("=" * 60)

    # Create test candles
    base_ts = int(time.time() * 1000)
    test_candles = [
        BinaryCandle(
            timestamp=base_ts + i * 1000,
            open=100.0 + i * 0.1,
            high=101.0 + i * 0.1,
            low=99.0 + i * 0.1,
            close=100.5 + i * 0.1,
            volume=1000.0 + i * 10
        )
        for i in range(10)
    ]

    print(f"\nTest candles created: {len(test_candles)}")
    print(f"Sample: {test_candles[0]}")
    print(f"Binary size: {len(test_candles[0].pack())} bytes")

    # Test Redis
    print("\n" + "-" * 40)
    print("Testing Redis...")
    try:
        async with RedisCandleClient() as redis_client:
            # Store candles
            count = await redis_client.store_candles("BTCUSDT", "1s", test_candles)
            print(f"  Stored {count} candles")

            # Get candles
            candles = await redis_client.get_candles("BTCUSDT", "1s", count=5)
            print(f"  Retrieved {len(candles)} candles")
            for c in candles[-3:]:
                print(f"    {c}")

            # Get latest
            latest = await redis_client.get_latest_candle("BTCUSDT", "1s")
            print(f"  Latest: {latest}")

            # Memory usage
            mem = await redis_client.get_memory_usage("BTCUSDT", "1s")
            print(f"  Memory: {mem} bytes ({mem/len(test_candles):.1f} bytes/candle)")

            # Cleanup
            await redis_client.delete_candles("BTCUSDT", "1s")
            print("  Cleaned up")

    except Exception as e:
        print(f"  Redis error: {e}")

    # Test Memcached
    print("\n" + "-" * 40)
    print("Testing Memcached...")
    try:
        async with MemcachedCandleClient() as mc_client:
            # Store candles
            for candle in test_candles:
                await mc_client.store_candle("BTCUSDT", "1s", candle)
            print(f"  Stored {len(test_candles)} candles")

            # Get candles
            candles = await mc_client.get_candles("BTCUSDT", "1s", count=5)
            print(f"  Retrieved {len(candles)} candles")
            for c in candles[-3:]:
                print(f"    {c}")

            # Cleanup
            deleted = await mc_client.delete_candles("BTCUSDT", "1s")
            print(f"  Cleaned up {deleted} candles")

    except Exception as e:
        print(f"  Memcached error: {e}")

    print("\n" + "=" * 60)
    print("Test complete!")


if __name__ == "__main__":
    asyncio.run(main())
