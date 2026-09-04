#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "btc_price_data.h"

#define NOW 1800000000LL
#define CG_FRESH "{\"bitcoin\":{\"usd\":100000.25,\"usd_24h_change\":2.5,\"last_updated_at\":1800000000}}"
#define MP_FRESH "{\"USD\":100000,\"time\":1800000000}"

TEST_CASE("BTC accepts a dated CoinGecko quote and change", "[btc-price]")
{
    btc_price_quote_t q;
    TEST_ASSERT_TRUE(btc_price_parse_coingecko(CG_FRESH, NOW, &q));
    TEST_ASSERT_TRUE(fabs(q.usd - 100000.25) < 0.001);
    TEST_ASSERT_TRUE(q.has_change_24h);
    TEST_ASSERT_TRUE(fabs(q.change_24h - 2.5) < 0.001);
    TEST_ASSERT_EQUAL_INT64(NOW, q.provider_timestamp);
    TEST_ASSERT_EQUAL_UINT32(0, q.age_at_fetch_seconds);
}

TEST_CASE("BTC accepts a price with null or missing change", "[btc-price]")
{
    btc_price_quote_t q;
    TEST_ASSERT_TRUE(btc_price_parse_coingecko(
        "{\"bitcoin\":{\"usd\":1,\"usd_24h_change\":null,\"last_updated_at\":1800000000}}", NOW, &q));
    TEST_ASSERT_FALSE(q.has_change_24h);
    TEST_ASSERT_TRUE(btc_price_parse_coingecko(
        "{\"bitcoin\":{\"usd\":1,\"last_updated_at\":1800000000}}", NOW, &q));
    TEST_ASSERT_FALSE(q.has_change_24h);
}

TEST_CASE("BTC mempool fallback never carries the prior change", "[btc-price]")
{
    btc_price_quote_t q;
    TEST_ASSERT_TRUE(btc_price_parse_coingecko(CG_FRESH, NOW, &q));
    TEST_ASSERT_TRUE(q.has_change_24h);
    TEST_ASSERT_TRUE(btc_price_parse_mempool(MP_FRESH, NOW, &q));
    TEST_ASSERT_FALSE(q.has_change_24h);
    TEST_ASSERT_TRUE(q.change_24h == 0.0);
    TEST_ASSERT_EQUAL_INT64(NOW, q.provider_timestamp);
}

TEST_CASE("BTC rejects old CoinGecko quotes despite successful downloads", "[btc-price]")
{
    btc_price_quote_t q;
    TEST_ASSERT_FALSE(btc_price_parse_coingecko(CG_FRESH, NOW + 900, &q));
    TEST_ASSERT_FALSE(btc_price_parse_coingecko(CG_FRESH, NOW + 86400, &q));
}

TEST_CASE("BTC rejects old mempool quotes despite successful downloads", "[btc-price]")
{
    btc_price_quote_t q;
    TEST_ASSERT_FALSE(btc_price_parse_mempool(MP_FRESH, NOW + 900, &q));
    TEST_ASSERT_FALSE(btc_price_parse_mempool(MP_FRESH, NOW + 86400, &q));
}

TEST_CASE("BTC preserves the provider age at the freshness boundary", "[btc-price]")
{
    btc_price_quote_t q;
    TEST_ASSERT_TRUE(btc_price_parse_coingecko(CG_FRESH, NOW + 899, &q));
    TEST_ASSERT_EQUAL_UINT32(899, q.age_at_fetch_seconds);
    TEST_ASSERT_TRUE(btc_price_parse_mempool(MP_FRESH, NOW + 899, &q));
    TEST_ASSERT_EQUAL_UINT32(899, q.age_at_fetch_seconds);
}

TEST_CASE("BTC rejects quotes without a usable timestamp", "[btc-price]")
{
    btc_price_quote_t q;
    const char *bad[] = {"null", "0", "\"1800000000\"", "true"};
    char cg[180], mp[100];
    TEST_ASSERT_FALSE(btc_price_parse_coingecko("{\"bitcoin\":{\"usd\":1}}", NOW, &q));
    TEST_ASSERT_FALSE(btc_price_parse_mempool("{\"USD\":1}", NOW, &q));
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        snprintf(cg, sizeof(cg), "{\"bitcoin\":{\"usd\":1,\"last_updated_at\":%s}}", bad[i]);
        snprintf(mp, sizeof(mp), "{\"USD\":1,\"time\":%s}", bad[i]);
        TEST_ASSERT_FALSE(btc_price_parse_coingecko(cg, NOW, &q));
        TEST_ASSERT_FALSE(btc_price_parse_mempool(mp, NOW, &q));
    }
}

TEST_CASE("BTC rejects implausibly future-dated quotes", "[btc-price]")
{
    btc_price_quote_t q;
    TEST_ASSERT_FALSE(btc_price_parse_coingecko(CG_FRESH, NOW - 121, &q));
    TEST_ASSERT_FALSE(btc_price_parse_mempool(MP_FRESH, NOW - 121, &q));
}

TEST_CASE("BTC tolerates bounded clock skew without a negative age", "[btc-price]")
{
    btc_price_quote_t q;
    TEST_ASSERT_TRUE(btc_price_parse_coingecko(CG_FRESH, NOW - 120, &q));
    TEST_ASSERT_EQUAL_UINT32(0, q.age_at_fetch_seconds);
    TEST_ASSERT_TRUE(btc_price_parse_mempool(MP_FRESH, NOW - 120, &q));
    TEST_ASSERT_EQUAL_UINT32(0, q.age_at_fetch_seconds);
}

TEST_CASE("BTC rejects fractional and millisecond timestamps", "[btc-price]")
{
    btc_price_quote_t q;
    TEST_ASSERT_FALSE(btc_price_parse_coingecko(
        "{\"bitcoin\":{\"usd\":1,\"last_updated_at\":1800000000.5}}", NOW, &q));
    TEST_ASSERT_FALSE(btc_price_parse_mempool("{\"USD\":1,\"time\":1800000000000}", NOW, &q));
    TEST_ASSERT_FALSE(btc_price_parse_mempool("{\"USD\":1,\"time\":1e309}", NOW, &q));
}

TEST_CASE("BTC rejects malformed prices and data shapes", "[btc-price]")
{
    btc_price_quote_t q;
    const char *bad[] = {"0", "-1", "null", "true", "\"100\"", "1e309", "1000000001", "{}", "[]"};
    char cg[180], mp[100];
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        snprintf(cg, sizeof(cg), "{\"bitcoin\":{\"usd\":%s,\"last_updated_at\":1800000000}}", bad[i]);
        snprintf(mp, sizeof(mp), "{\"USD\":%s,\"time\":1800000000}", bad[i]);
        TEST_ASSERT_FALSE(btc_price_parse_coingecko(cg, NOW, &q));
        TEST_ASSERT_FALSE(btc_price_parse_mempool(mp, NOW, &q));
    }
    TEST_ASSERT_FALSE(btc_price_parse_coingecko("[]", NOW, &q));
    TEST_ASSERT_FALSE(btc_price_parse_coingecko("{\"bitcoin\":[]}", NOW, &q));
    TEST_ASSERT_FALSE(btc_price_parse_mempool("garbage", NOW, &q));
}

TEST_CASE("BTC refuses trailing non-JSON payloads", "[btc-price]")
{
    btc_price_quote_t q;
    TEST_ASSERT_FALSE(btc_price_parse_coingecko(CG_FRESH " garbage", NOW, &q));
    TEST_ASSERT_FALSE(btc_price_parse_mempool(MP_FRESH " {}", NOW, &q));
    TEST_ASSERT_TRUE(btc_price_parse_mempool(MP_FRESH " \n", NOW, &q));
}

TEST_CASE("BTC will not accept freshness with an unset clock", "[btc-price]")
{
    btc_price_quote_t q;
    TEST_ASSERT_FALSE(btc_price_parse_coingecko(CG_FRESH, 0, &q));
    TEST_ASSERT_FALSE(btc_price_parse_mempool(MP_FRESH, BTC_PRICE_MIN_VALID_EPOCH - 1, &q));
    TEST_ASSERT_FALSE(btc_price_clock_valid(BTC_PRICE_MAX_VALID_EPOCH + 1));
    TEST_ASSERT_TRUE(btc_price_clock_valid(NOW));
}

TEST_CASE("BTC cache ages from quote time instead of request time", "[btc-price]")
{
    TEST_ASSERT_EQUAL_UINT32(120, btc_price_age_seconds(120, 1000000, 1000000));
    TEST_ASSERT_EQUAL_UINT32(420, btc_price_age_seconds(120, 1000000, 301000000));
    TEST_ASSERT_EQUAL_UINT32(900, btc_price_age_seconds(899, 1000000, 2000000));
    TEST_ASSERT_EQUAL_UINT32(300, btc_price_age_seconds(0, 1000000, 301000000));
}

TEST_CASE("BTC age calculation saturates and handles invalid timer inputs", "[btc-price]")
{
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, btc_price_age_seconds(UINT32_MAX, 0, INT64_MAX));
    TEST_ASSERT_EQUAL_UINT32(120, btc_price_age_seconds(120, 1000000, 0));
    TEST_ASSERT_EQUAL_UINT32(120, btc_price_age_seconds(120, -1, INT64_MAX));
}

TEST_CASE("BTC parsers handle null arguments without stale output", "[btc-price]")
{
    btc_price_quote_t q;
    memset(&q, 0xAA, sizeof(q));
    TEST_ASSERT_FALSE(btc_price_parse_coingecko(NULL, NOW, &q));
    TEST_ASSERT_FALSE(q.has_change_24h);
    TEST_ASSERT_TRUE(q.usd == 0.0);
    TEST_ASSERT_FALSE(btc_price_parse_coingecko(CG_FRESH, NOW, NULL));
    TEST_ASSERT_FALSE(btc_price_parse_mempool(MP_FRESH, NOW, NULL));
}

TEST_CASE("BTC mempool accepts both documented and lowercase USD keys", "[btc-price]")
{
    btc_price_quote_t q;
    TEST_ASSERT_TRUE(btc_price_parse_mempool("{\"usd\":100,\"time\":1800000000}", NOW, &q));
    TEST_ASSERT_FALSE(q.has_change_24h);
}

TEST_CASE("BTC rejects an impossible percentage without discarding a valid price", "[btc-price]")
{
    btc_price_quote_t q;
    TEST_ASSERT_TRUE(btc_price_parse_coingecko(
        "{\"bitcoin\":{\"usd\":1,\"usd_24h_change\":-101,\"last_updated_at\":1800000000}}", NOW, &q));
    TEST_ASSERT_FALSE(q.has_change_24h);
}

TEST_CASE("BTC bounds JSON depth and size before invoking cJSON", "[btc-price]")
{
    btc_price_quote_t q;
    TEST_ASSERT_FALSE(btc_price_parse_mempool("[[[[[[[[[0]]]]]]]]]", NOW, &q));
    char large[BTC_PRICE_JSON_MAX_BYTES + 1];
    memset(large, ' ', sizeof(large) - 1);
    large[sizeof(large) - 1] = '\0';
    TEST_ASSERT_FALSE(btc_price_parse_mempool(large, NOW, &q));
    TEST_ASSERT_TRUE(btc_price_parse_mempool(
        "{\"USD\":1,\"time\":1800000000,\"note\":\"[{}]\"}", NOW, &q));
}
