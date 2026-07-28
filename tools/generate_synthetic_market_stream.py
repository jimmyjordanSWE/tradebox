#!/usr/bin/env python3
"""Generate deterministic Alpaca-shaped websocket market-data frames.

The output is transport input, not precomputed database state. A replay
adapter must pass each JSONL frame through the same decoder, market-data
store, bar engine, and persistence path used by the live websocket.
"""

from __future__ import annotations

import argparse
import json
import random
from datetime import datetime, timezone
from pathlib import Path


def timestamp_ns(epoch_ns: int) -> str:
    seconds, nanoseconds = divmod(epoch_ns, 1_000_000_000)
    base = datetime.fromtimestamp(seconds, tz=timezone.utc)
    return f"{base:%Y-%m-%dT%H:%M:%S}.{nanoseconds:09d}Z"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--instruments", type=int, default=10)
    parser.add_argument("--events-per-instrument", type=int, default=100_000)
    parser.add_argument("--frame-size", type=int, default=100)
    parser.add_argument("--seed", type=int, default=0x7A6D3)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.instruments <= 0 or args.events_per_instrument <= 0:
        raise SystemExit("instrument and event counts must be positive")
    if args.frame_size <= 0:
        raise SystemExit("frame size must be positive")

    random_source = random.Random(args.seed)
    symbols = [f"TST{index:03d}" for index in range(1, args.instruments + 1)]
    prices = {
        symbol: round(25.0 + index * 37.5, 4)
        for index, symbol in enumerate(symbols)
    }
    trade_ids = {symbol: 0 for symbol in symbols}
    live_trade_ids: dict[str, list[int]] = {symbol: [] for symbol in symbols}
    remaining = {
        symbol: args.events_per_instrument for symbol in symbols
    }
    start_ns = 1_783_511_400_000_000_000
    sequence = 0
    frame: list[dict[str, object]] = []

    args.output.parent.mkdir(parents=True, exist_ok=True)
    metadata_path = args.output.with_suffix(args.output.suffix + ".assets.json")
    metadata = [
        {
            "instrument_id": f"synthetic:{index:04d}",
            "provider_asset_id": f"synthetic-provider:{index:04d}",
            "symbol": symbol,
            "exchange": "SYNTH",
            "name": f"Synthetic high-rate instrument {index}",
            "isin": None,
            "cusip": None,
            "sedol": None,
        }
        for index, symbol in enumerate(symbols, start=1)
    ]
    metadata_path.write_text(
        json.dumps(metadata, indent=2), encoding="utf-8"
    )

    with args.output.open("w", encoding="utf-8", newline="\n") as output:
        output.write(
            json.dumps(
                [{"T": "success", "msg": "authenticated"}],
                separators=(",", ":"),
            )
            + "\n"
        )
        output.write(
            json.dumps(
                [{
                    "T": "subscription",
                    "trades": symbols,
                    "quotes": symbols,
                    "bars": [],
                }],
                separators=(",", ":"),
            )
            + "\n"
        )

        active = list(symbols)
        while active:
            symbol = active[random_source.randrange(len(active))]
            sequence += 1
            remaining[symbol] -= 1
            event_ns = start_ns + sequence * 250_000
            price = max(
                1.0,
                prices[symbol] +
                random_source.choice((-1, 0, 0, 0, 1)) * 0.01,
            )
            prices[symbol] = round(price, 4)
            event_type = random_source.random()

            if event_type < 0.42:
                spread = random_source.choice((0.01, 0.01, 0.02, 0.03))
                event = {
                    "T": "q",
                    "S": symbol,
                    "bx": "V",
                    "bp": round(price - spread / 2, 4),
                    "bs": random_source.choice((1, 2, 5, 10, 20, 50)),
                    "ax": "V",
                    "ap": round(price + spread / 2, 4),
                    "as": random_source.choice((1, 2, 5, 10, 20, 50)),
                    "c": ["R"],
                    "z": "C",
                    "t": timestamp_ns(event_ns),
                }
            elif event_type < 0.995 or not live_trade_ids[symbol]:
                trade_ids[symbol] += 1
                trade_id = trade_ids[symbol]
                live_trade_ids[symbol].append(trade_id)
                if len(live_trade_ids[symbol]) > 2_500:
                    live_trade_ids[symbol].pop(0)
                event = {
                    "T": "t",
                    "S": symbol,
                    "i": trade_id,
                    "x": "V",
                    "p": price,
                    "s": random_source.choice(
                        (1, 5, 10, 20, 50, 100, 200, 500)
                    ),
                    "c": ["@"],
                    "z": "C",
                    "t": timestamp_ns(event_ns),
                }
            elif event_type < 0.998:
                canceled = random_source.choice(live_trade_ids[symbol])
                event = {
                    "T": "x",
                    "S": symbol,
                    "i": canceled,
                    "x": "V",
                    "z": "C",
                    "t": timestamp_ns(event_ns),
                }
            else:
                original = random_source.choice(live_trade_ids[symbol])
                trade_ids[symbol] += 1
                corrected = trade_ids[symbol]
                live_trade_ids[symbol].append(corrected)
                event = {
                    "T": "c",
                    "S": symbol,
                    "oi": original,
                    "ci": corrected,
                    "x": "V",
                    "cp": price,
                    "cs": random_source.choice((10, 50, 100, 200)),
                    "cc": ["@"],
                    "z": "C",
                    "t": timestamp_ns(event_ns),
                }

            frame.append(event)
            if len(frame) == args.frame_size:
                output.write(
                    json.dumps(frame, separators=(",", ":")) + "\n"
                )
                frame.clear()
            if remaining[symbol] == 0:
                active.remove(symbol)

        if frame:
            output.write(json.dumps(frame, separators=(",", ":")) + "\n")

    total = args.instruments * args.events_per_instrument
    print(
        f"wrote {total:,} events for {args.instruments} instruments "
        f"to {args.output}"
    )
    print(f"wrote instrument metadata to {metadata_path}")


if __name__ == "__main__":
    main()
