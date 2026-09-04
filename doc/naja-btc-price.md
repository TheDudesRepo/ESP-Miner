# Large-display Bitcoin price dashboard

This feature adds a four-page dashboard to the 320x170 ST7789 display used by
Bitaxe Color boards such as the Naja Duo. It attaches to ESP-Miner's existing
large stats screen, leaving provisioning, Wi-Fi disconnect, firmware update,
self-test, identify, and overheat screens unchanged.

## Pages

1. **Bitcoin / Home** — BTC/USD, 24-hour change, price age/source, hashrate,
   power, and efficiency.
2. **Mining** — hashrate, session/all-time best difficulty, accepted/rejected
   shares, pool difficulty, and block height.
3. **Hardware** — both ASIC temperatures, fan RPM, power, input voltage,
   current, Vcore, frequency, and efficiency.
4. **Network** — current SSID, Wi-Fi RSSI/quality, IPv4 address, uptime, and
   firmware version.

SSID and IP address are read from the device at runtime. No personal network
values are stored in source code.

## Price providers

- Primary: CoinGecko keyless `/api/v3/simple/price` endpoint, including the
  24-hour USD change.
- Fallback: mempool.space `/api/v1/prices` endpoint. The fallback provides the
  USD price but not the 24-hour percentage, so the display shows `24H --`.

The background task waits for Wi-Fi, starts after the miner has settled, and
polls at most once every five minutes. Failures use exponential backoff up to
five minutes. The last successful price remains cached and is marked stale
once it is fifteen minutes old. Price-network failures do not pause mining or
block LVGL rendering.

## TLS note for ESP-IDF 6.0.2

HTTPS verification uses ESP-IDF's common certificate bundle. Cross-signed
certificate-bundle verification is disabled because ESP-IDF 6.0.2 does not yet
contain Espressif's fix for the repeated-handshake leak in that optional path.
Normal hostname and certificate verification remain enabled.
