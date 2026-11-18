# Data

Nothing here is committed except this file.

## Real days

Nasdaq publishes full-day TotalView-ITCH 5.0 sample files at
<https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/>. As of November 2026 the listing
holds about twenty full days, including:

| File | Day | Size (gz) |
| --- | --- | --- |
| `01302019.NASDAQ_ITCH50.gz` | 2019-01-30 | 4.8 GB |
| `03272019.NASDAQ_ITCH50.gz` | 2019-03-27 | 5.5 GB |
| `07302019.NASDAQ_ITCH50.gz` | 2019-07-30 | 3.7 GB |
| `08302019.NASDAQ_ITCH50.gz` | 2019-08-30 | 4.1 GB |
| `10302019.NASDAQ_ITCH50.gz` | 2019-10-30 | 3.9 GB |
| `12302019.NASDAQ_ITCH50.gz` | 2019-12-30 | 3.5 GB |
| `01302020.NASDAQ_ITCH50.gz` | 2020-01-30 | 5.6 GB |
| `S101819-v50.txt.gz`, `S071321-v50.txt.gz`, `S081321-v50.txt.gz` | 2019-10-18, 2021-07-13, 2021-08-13 | 4-6 GB |
| `S112825-v50.txt.gz`, `S120825` to `S121225-v50.txt.gz` | 2025-11-28, 2025-12-08 to 12 | 5-13 GB |
| `S061226-v50.txt.gz` | 2026-06-12 | 18 GB |

Several older dates have only an `.md5sum` left. Despite the `.txt` name the
`S*-v50` files are expected to be the same binary format; check the first
bytes (`xxd file | head`: a 2-byte length then `S`) before relying on it.

```sh
curl -O "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302020.NASDAQ_ITCH50.gz"
gunzip 01302020.NASDAQ_ITCH50.gz            # ~12 GB uncompressed
make extract IN=data/01302020.NASDAQ_ITCH50 OUT=data/extract5.itch SYMBOLS=AAPL,MSFT,SPY,QQQ,F
```

Official closing prices for the independent close check go in
`data/<file>.closes.csv` with columns `symbol,close`, taken from a source
outside the feed.

## Synthetic days

`scripts/reproduce.sh` writes `synth1-3.itch` with ground truth
(`truth*.jsonl`), official closes, a corrupted copy for the negative control,
and a 3.2M-message day for benchmarks. See `sim/itch_sim.py`.
