#include "envoy/event/timer.h"

#include "extensions/compression/gzip/decompressor/zlib_decompressor_impl.h"

#include "test/integration/http_integration.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {

class GzipIntegrationTest : public testing::TestWithParam<Network::Address::IpVersion>,
                            public Event::SimulatedTimeSystem,
                            public HttpIntegrationTest {
public:
  GzipIntegrationTest() : HttpIntegrationTest(Http::CodecType::HTTP1, GetParam()) {}

  void SetUp() override { decompressor_.init(window_bits); }
  void TearDown() override { cleanupUpstreamAndDownstream(); }

  void initializeFilter(const std::string& config) {
    config_helper_.addFilter(config);
    config_helper_.addRuntimeOverride("envoy.deprecated_features.allow_deprecated_gzip_http_filter",
                                      "true");
    config_helper_.enableDeprecatedV2Api();
    initialize();
    codec_client_ = makeHttpConnection(makeClientConnection((lookupPort("http"))));
  }

  void doRequestAndCompression(Http::TestRequestHeaderMapImpl&& request_headers,
                               Http::TestResponseHeaderMapImpl&& response_headers) {
    uint64_t content_length;
    ASSERT_TRUE(absl::SimpleAtoi(response_headers.get_("content-length"), &content_length));
    const Buffer::OwnedImpl expected_response{std::string(content_length, 'a')};
    auto response =
        sendRequestAndWaitForResponse(request_headers, 0, response_headers, content_length);
    EXPECT_TRUE(upstream_request_->complete());
    EXPECT_EQ(0U, upstream_request_->bodyLength());
    EXPECT_TRUE(response->complete());
    EXPECT_EQ("200", response->headers().getStatusValue());
    ASSERT_FALSE(response->headers().get(Http::CustomHeaders::get().ContentEncoding).empty());
    EXPECT_EQ(Http::CustomHeaders::get().ContentEncodingValues.Gzip,
              response->headers()
                  .get(Http::CustomHeaders::get().ContentEncoding)[0]
                  ->value()
                  .getStringView());
    ASSERT_TRUE(response->headers().TransferEncoding() != nullptr);
    EXPECT_EQ(Http::Headers::get().TransferEncodingValues.Chunked,
              response->headers().getTransferEncodingValue());

    Buffer::OwnedImpl decompressed_response{};
    const Buffer::OwnedImpl compressed_response{response->body()};
    decompressor_.decompress(compressed_response, decompressed_response);
    ASSERT_EQ(content_length, decompressed_response.length());
    EXPECT_TRUE(TestUtility::buffersEqual(expected_response, decompressed_response));
  }

  void doRequestAndNoCompression(Http::TestRequestHeaderMapImpl&& request_headers,
                                 Http::TestResponseHeaderMapImpl&& response_headers) {
    uint64_t content_length;
    ASSERT_TRUE(absl::SimpleAtoi(response_headers.get_("content-length"), &content_length));
    auto response =
        sendRequestAndWaitForResponse(request_headers, 0, response_headers, content_length);
    EXPECT_TRUE(upstream_request_->complete());
    EXPECT_EQ(0U, upstream_request_->bodyLength());
    EXPECT_TRUE(response->complete());
    EXPECT_EQ("200", response->headers().getStatusValue());
    ASSERT_TRUE(response->headers().get(Http::CustomHeaders::get().ContentEncoding).empty());
    ASSERT_EQ(content_length, response->body().size());
    EXPECT_EQ(response->body(), std::string(content_length, 'a'));
  }

  const std::string deprecated_full_config{R"EOF(
      name: gzip
      typed_config:
        "@type": type.googleapis.com/envoy.config.filter.http.gzip.v2.Gzip
        memory_level: 3
        window_bits: 10
        compression_level: best
        compression_strategy: rle
        disable_on_etag_header: true
        content_length: 100
        content_type:
          - text/html
          - application/json
    )EOF"};

  const std::string full_config{R"EOF(
      name: gzip
      typed_config:
        "@type": type.googleapis.com/envoy.extensions.filters.http.gzip.v3.Gzip
        memory_level: 3
        window_bits: 10
        compression_level: best
        compression_strategy: rle
        compressor:
          disable_on_etag_header: true
          content_length: 100
          content_type:
            - text/html
            - application/json
    )EOF"};

  const std::string default_config{"name: envoy.filters.http.gzip"};

  const uint64_t window_bits{15 | 16};

  Stats::IsolatedStoreImpl stats_store_;
  Extensions::Compression::Gzip::Decompressor::ZlibDecompressorImpl decompressor_{stats_store_,
                                                                                  "test"};
};

INSTANTIATE_TEST_SUITE_P(IpVersions, GzipIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                         TestUtility::ipTestParamsToString);

/**
 * Exercises gzip compression with default configuration.
 */
TEST_P(GzipIntegrationTest, DEPRECATED_FEATURE_TEST(AcceptanceDefaultConfigTest)) {
  initializeFilter(default_config);
  doRequestAndCompression(Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                                         {":path", "/test/long/url"},
                                                         {":scheme", "http"},
                                                         {":authority", "host"},
                                                         {"accept-encoding", "deflate, gzip"}},
                          Http::TestResponseHeaderMapImpl{{":status", "200"},
                                                          {"content-length", "4400"},
                                                          {"content-type", "text/xml"}});
}

/**
 * Exercises gzip compression with deprecated full configuration.
 */
TEST_P(GzipIntegrationTest, DEPRECATED_FEATURE_TEST(AcceptanceDeprecatedFullConfigTest)) {
  initializeFilter(deprecated_full_config);
  doRequestAndCompression(Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                                         {":path", "/test/long/url"},
                                                         {":scheme", "http"},
                                                         {":authority", "host"},
                                                         {"accept-encoding", "deflate, gzip"}},
                          Http::TestResponseHeaderMapImpl{{":status", "200"},
                                                          {"content-length", "4400"},
                                                          {"content-type", "application/json"}});
}

/**
 * Exercises gzip compression with full configuration.
 */
TEST_P(GzipIntegrationTest, DEPRECATED_FEATURE_TEST(AcceptanceFullConfigTest)) {
  initializeFilter(full_config);
  doRequestAndCompression(Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                                         {":path", "/test/long/url"},
                                                         {":scheme", "http"},
                                                         {":authority", "host"},
                                                         {"accept-encoding", "deflate, gzip"}},
                          Http::TestResponseHeaderMapImpl{{":status", "200"},
                                                          {"content-length", "4400"},
                                                          {"content-type", "application/json"}});
}

/**
 * Exercises filter when client request contains 'identity' type.
 */
TEST_P(GzipIntegrationTest, DEPRECATED_FEATURE_TEST(IdentityAcceptEncoding)) {
  initializeFilter(default_config);
  doRequestAndNoCompression(Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                                           {":path", "/test/long/url"},
                                                           {":scheme", "http"},
                                                           {":authority", "host"},
                                                           {"accept-encoding", "identity"}},
                            Http::TestResponseHeaderMapImpl{{":status", "200"},
                                                            {"content-length", "128"},
                                                            {"content-type", "text/plain"}});
}

/**
 * Exercises filter when client request contains unsupported 'accept-encoding' type.
 */
TEST_P(GzipIntegrationTest, DEPRECATED_FEATURE_TEST(NotSupportedAcceptEncoding)) {
  initializeFilter(default_config);
  doRequestAndNoCompression(Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                                           {":path", "/test/long/url"},
                                                           {":scheme", "http"},
                                                           {":authority", "host"},
                                                           {"accept-encoding", "deflate, br"}},
                            Http::TestResponseHeaderMapImpl{{":status", "200"},
                                                            {"content-length", "128"},
                                                            {"content-type", "text/plain"}});
}

/**
 * Exercises filter when upstream response is already encoded.
 */
TEST_P(GzipIntegrationTest, DEPRECATED_FEATURE_TEST(UpstreamResponseAlreadyEncoded)) {
  initializeFilter(default_config);
  Http::TestRequestHeaderMapImpl request_headers{{":method", "GET"},
                                                 {":path", "/test/long/url"},
                                                 {":scheme", "http"},
                                                 {":authority", "host"},
                                                 {"accept-encoding", "deflate, gzip"}};

  Http::TestResponseHeaderMapImpl response_headers{{":status", "200"},
                                                   {"content-encoding", "br"},
                                                   {"content-length", "128"},
                                                   {"content-type", "application/json"}};

  auto response = sendRequestAndWaitForResponse(request_headers, 0, response_headers, 128);

  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_EQ(0U, upstream_request_->bodyLength());
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().getStatusValue());
  ASSERT_EQ("br", response->headers()
                      .get(Http::CustomHeaders::get().ContentEncoding)[0]
                      ->value()
                      .getStringView());
  EXPECT_EQ(128U, response->body().size());
}

/**
 * Exercises filter when upstream responds with content length below the default threshold.
 */
TEST_P(GzipIntegrationTest, DEPRECATED_FEATURE_TEST(NotEnoughContentLength)) {
  initializeFilter(default_config);
  Http::TestRequestHeaderMapImpl request_headers{{":method", "GET"},
                                                 {":path", "/test/long/url"},
                                                 {":scheme", "http"},
                                                 {":authority", "host"},
                                                 {"accept-encoding", "deflate, gzip"}};

  Http::TestResponseHeaderMapImpl response_headers{
      {":status", "200"}, {"content-length", "10"}, {"content-type", "application/json"}};

  auto response = sendRequestAndWaitForResponse(request_headers, 0, response_headers, 10);

  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_EQ(0U, upstream_request_->bodyLength());
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().getStatusValue());
  ASSERT_TRUE(response->headers().get(Http::CustomHeaders::get().ContentEncoding).empty());
  EXPECT_EQ(10U, response->body().size());
}

/**
 * Exercises filter when response from upstream service is empty.
 */
TEST_P(GzipIntegrationTest, DEPRECATED_FEATURE_TEST(EmptyResponse)) {
  initializeFilter(default_config);
  Http::TestRequestHeaderMapImpl request_headers{{":method", "GET"},
                                                 {":path", "/test/long/url"},
                                                 {":scheme", "http"},
                                                 {":authority", "host"},
                                                 {"accept-encoding", "deflate, gzip"}};

  Http::TestResponseHeaderMapImpl response_headers{{":status", "204"}, {"content-length", "0"}};

  auto response = sendRequestAndWaitForResponse(request_headers, 0, response_headers, 0);

  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_EQ(0U, upstream_request_->bodyLength());
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("204", response->headers().getStatusValue());
  ASSERT_TRUE(response->headers().get(Http::CustomHeaders::get().ContentEncoding).empty());
  EXPECT_EQ(0U, response->body().size());
}

/**
 * Exercises filter when upstream responds with restricted content-type value.
 */
TEST_P(GzipIntegrationTest, DEPRECATED_FEATURE_TEST(SkipOnContentType)) {
  initializeFilter(full_config);
  doRequestAndNoCompression(Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                                           {":path", "/test/long/url"},
                                                           {":scheme", "http"},
                                                           {":authority", "host"},
                                                           {"accept-encoding", "deflate, gzip"}},
                            Http::TestResponseHeaderMapImpl{{":status", "200"},
                                                            {"content-length", "128"},
                                                            {"content-type", "application/xml"}});
}

/**
 * Exercises filter when upstream responds with restricted cache-control value.
 */
TEST_P(GzipIntegrationTest, DEPRECATED_FEATURE_TEST(SkipOnCacheControl)) {
  initializeFilter(full_config);
  doRequestAndNoCompression(Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                                           {":path", "/test/long/url"},
                                                           {":scheme", "http"},
                                                           {":authority", "host"},
                                                           {"accept-encoding", "deflate, gzip"}},
                            Http::TestResponseHeaderMapImpl{{":status", "200"},
                                                            {"content-length", "128"},
                                                            {"cache-control", "no-transform"},
                                                            {"content-type", "application/json"}});
}

/**
 * Exercises gzip compression when upstream returns a chunked response.
 */
TEST_P(GzipIntegrationTest, DEPRECATED_FEATURE_TEST(AcceptanceFullConfigChunkedResponse)) {
  initializeFilter(full_config);
  Http::TestRequestHeaderMapImpl request_headers{{":method", "GET"},
                                                 {":path", "/test/long/url"},
                                                 {":scheme", "http"},
                                                 {":authority", "host"},
                                                 {"accept-encoding", "deflate, gzip"}};

  Http::TestResponseHeaderMapImpl response_headers{{":status", "200"},
                                                   {"content-type", "application/json"}};

  auto response = sendRequestAndWaitForResponse(request_headers, 0, response_headers, 1024);

  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_EQ(0U, upstream_request_->bodyLength());
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().getStatusValue());
  ASSERT_EQ("gzip", response->headers()
                        .get(Http::CustomHeaders::get().ContentEncoding)[0]
                        ->value()
                        .getStringView());
  ASSERT_EQ("chunked", response->headers().getTransferEncodingValue());
}

/**
 * Verify Vary header values are preserved.
 */
TEST_P(GzipIntegrationTest, DEPRECATED_FEATURE_TEST(AcceptanceFullConfigVaryHeader)) {
  initializeFilter(default_config);
  Http::TestRequestHeaderMapImpl request_headers{{":method", "GET"},
                                                 {":path", "/test/long/url"},
                                                 {":scheme", "http"},
                                                 {":authority", "host"},
                                                 {"accept-encoding", "deflate, gzip"}};

  Http::TestResponseHeaderMapImpl response_headers{
      {":status", "200"}, {"content-type", "application/json"}, {"vary", "Cookie"}};

  auto response = sendRequestAndWaitForResponse(request_headers, 0, response_headers, 1024);

  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_EQ(0U, upstream_request_->bodyLength());
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().getStatusValue());
  ASSERT_EQ("gzip", response->headers()
                        .get(Http::CustomHeaders::get().ContentEncoding)[0]
                        ->value()
                        .getStringView());
  ASSERT_EQ("Cookie, Accept-Encoding",
            response->headers().get(Http::CustomHeaders::get().Vary)[0]->value().getStringView());
}
} // namespace Envoy
