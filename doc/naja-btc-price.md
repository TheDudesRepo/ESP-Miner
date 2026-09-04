# Naja Duo Bitcoin price dashboard

This experimental feature targets **Naja Duo board 1201** with a logical
320x170 ST7789 display. The carousel is enabled only in landscape geometry.
Other boards and portrait geometry retain ESP-Miner's original display.
Physical-device validation is still pending.

## Pages

1. **Bitcoin / Home**: BTC/USD, 24-hour change, provider, quote age, fetch age,
   hashrate, power, and efficiency.
2. **Mining**: hashrate, session/all-time best difficulty, accepted/rejected
   shares, pool difficulty, and block height.
3. **Hardware**: both ASIC temperatures, fan RPM, power, input voltage,
   current, Vcore, frequency, and efficiency.
4. **Network**: runtime SSID, Wi-Fi RSSI/quality, IPv4 address, uptime, and
   firmware version.

The header remains visible across carousel pages. Actual short-click events
advance pages; automatic rotation is every ten seconds. A block-found event
holds the Home page with a banner. The dashboard targets the existing stats
screen explicitly and yields to provisioning, disconnection, firmware update,
self-test, identify mode, overheat and hardware/ASIC status screens.

SSID and IP address are read from the device at runtime and are not sent to
price providers. No personal network values, credentials or API keys are
stored in the feature source.

## Market data and transport

CoinGecko's keyless `/api/v3/simple/price` endpoint is primary, including
`include_24hr_change=true` and `include_last_updated_at=true`. mempool.space's
`/api/v1/prices` is the fallback, using `USD` and `time`; it has no 24-hour
percentage, so that field is unavailable on fallback. These are best-effort
public services, not guaranteed production feeds.

Normal polling is every five minutes; failures back off from 30 seconds to
five minutes. Missing, malformed, old or implausibly future-dated timestamps
are rejected. Prices at least fifteen minutes old are stale. Quote age starts
at the provider's timestamp and continues aging with the monotonic clock.
`Q` is quote age in minutes; `F` is time since the successful fetch. A stale
price is retained with a marker, but its 24-hour change is hidden.

A separate low-priority task uses bounded response buffers and JSON depth,
checks complete responses, verifies HTTPS certificates/hostnames/dates, and
does not follow redirects. Normal application startup initializes SNTP using
`pool.ntp.org` independently of the board, orientation or display state. The
price worker waits for a usable clock in the background and can retry failed
initialization. No synchronized clock means no new price, not a stalled LCD.
Certificate-validated TLS pools also require a correct clock; ordinary TCP pool
connections do not have that dependency. Price requests defer during OTA,
faults, setup, and low-PSRAM conditions.

The common ESP-IDF certificate bundle is enabled. Optional cross-signed-bundle
verification stays disabled. Mining control, pool/wallet configuration,
frequency, voltage, and thermal protections are not modified by this feature.

## Regression coverage

The ESP-IDF/QEMU test project compiles the production price parser and its
freshness/age tests. A separate host regression job compiles production
button, rotation, safety-gate, formatting, cache and response-buffer functions
with narrow platform stand-ins and address/undefined-behavior sanitizers.
These checks do not emulate a physical LCD, Wi-Fi radio, or mining hardware.
