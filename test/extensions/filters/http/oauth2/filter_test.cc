#include <memory>
#include <string>

#include "envoy/extensions/filters/http/oauth2/v3/oauth.pb.h"
#include "envoy/extensions/filters/http/oauth2/v3/oauth.pb.validate.h"
#include "envoy/http/async_client.h"
#include "envoy/http/message.h"

#include "source/common/common/macros.h"
#include "source/common/http/message_impl.h"
#include "source/common/protobuf/message_validator_impl.h"
#include "source/common/protobuf/utility.h"
#include "source/common/runtime/runtime_protos.h"
#include "source/common/secret/secret_manager_impl.h"
#include "source/extensions/filters/http/oauth2/filter.h"

#include "test/mocks/http/mocks.h"
#include "test/mocks/server/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/test_runtime.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Oauth2 {

using testing::_;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;

static const std::string TEST_CALLBACK = "/_oauth";
static const std::string TEST_CLIENT_ID = "1";
static const std::string TEST_DEFAULT_SCOPE = "user";
static const std::string TEST_ENCODED_AUTH_SCOPES = "user%20openid%20email";
static const std::string TEST_CSRF_TOKEN =
    "00000000075bcd15.na6kru4x1pHgocSIeU/mdtHYn58Gh1bqweS4XXoiqVg=";
// {"url":"https://traffic.example.com/original_path?var1=1&var2=2","csrf_token":"${extracted}"}
static const std::string TEST_ENCODED_STATE =
    "eyJ1cmwiOiJodHRwczovL3RyYWZmaWMuZXhhbXBsZS5jb20vb3JpZ2luYWxfcGF0aD92YXIxPTEmdmFyMj0yIiwiY3NyZl"
    "90b2tlbiI6IjAwMDAwMDAwMDc1YmNkMTUubmE2a3J1NHgxcEhnb2NTSWVVL21kdEhZbjU4R2gxYnF3ZVM0WFhvaXFWZz0i"
    "fQ";
static const std::string TEST_CODE_VERIFIER = "Fc1bBwAAAAAVzVsHAAAAABXNWwcAAAAAFc1bBwAAAAA";
static const std::string TEST_ENCRYPTED_CODE_VERIFIER =
    "Fc1bBwAAAAAVzVsHAAAAABjf6i_Hvf8T2dEuEhPhhDNMlp16az-0dxisL-TzJKaZjOMF8nov_pG377FHmpKcsA";
static const std::string TEST_CODE_CHALLENGE = "YRQaBq_UpkWzfr6JvtNnh7LMfmPVcIKVYdV98ugwmLY";
static const std::string TEST_ENCRYPTED_ACCESS_TOKEN =
    "Fc1bBwAAAAAVzVsHAAAAAHDCo6XWwdgw5IYsxjfymIQ"; //"access_code"
static const std::string TEST_ENCRYPTED_ID_TOKEN =
    "Fc1bBwAAAAAVzVsHAAAAAJohQ-XDfnYLdgIQ2yJfRZQ"; //"some-id-token"
static const std::string TEST_ENCRYPTED_REFRESH_TOKEN =
    "Fc1bBwAAAAAVzVsHAAAAAERBBlyQ3ASXvDHzyIRDhLwvl1w07AKhjwBz1s4wJGX8"; //"some-refresh-token"
static const std::string TEST_HMAC_SECRET = "asdf_token_secret_fdsa";

namespace {
Http::RegisterCustomInlineHeader<Http::CustomInlineHeaderRegistry::Type::RequestHeaders>
    authorization_handle(Http::CustomHeaders::get().Authorization);
}

class MockSecretReader : public SecretReader {
public:
  const std::string& clientSecret() const override {
    CONSTRUCT_ON_FIRST_USE(std::string, "asdf_client_secret_fdsa");
  }
  const std::string& hmacSecret() const override {
    CONSTRUCT_ON_FIRST_USE(std::string, TEST_HMAC_SECRET);
  }
};

class MockOAuth2CookieValidator : public CookieValidator {
public:
  MOCK_METHOD(std::string&, username, (), (const));
  MOCK_METHOD(std::string&, token, (), (const));
  MOCK_METHOD(std::string&, refreshToken, (), (const));

  MOCK_METHOD(bool, canUpdateTokenByRefreshToken, (), (const));
  MOCK_METHOD(bool, isValid, (), (const));
  MOCK_METHOD(void, setParams, (const Http::RequestHeaderMap& headers, const std::string& secret));
};

class MockOAuth2Client : public OAuth2Client {
public:
  void onSuccess(const Http::AsyncClient::Request&, Http::ResponseMessagePtr&&) override {}
  void onFailure(const Http::AsyncClient::Request&, Http::AsyncClient::FailureReason) override {}
  void setCallbacks(FilterCallbacks&) override {}
  void onBeforeFinalizeUpstreamSpan(Envoy::Tracing::Span&,
                                    const Http::ResponseHeaderMap*) override {}

  MOCK_METHOD(void, asyncGetAccessToken,
              (const std::string&, const std::string&, const std::string&, const std::string&,
               const std::string&, Envoy::Extensions::HttpFilters::Oauth2::AuthType));

  MOCK_METHOD(void, asyncRefreshAccessToken,
              (const std::string&, const std::string&, const std::string&,
               Envoy::Extensions::HttpFilters::Oauth2::AuthType));
};

class OAuth2Test : public testing::TestWithParam<int> {
public:
  OAuth2Test(bool run_init = true) : request_(&cm_.thread_local_cluster_.async_client_) {
    factory_context_.server_factory_context_.cluster_manager_.initializeClusters(
        {"auth.example.com"}, {});
    if (run_init) {
      init();
    }
  }

  void init() { init(getConfig()); }

  void init(FilterConfigSharedPtr config) {
    // Set up the OAuth client.
    oauth_client_ = new MockOAuth2Client();
    std::unique_ptr<OAuth2Client> oauth_client_ptr{oauth_client_};

    config_ = config;
    ON_CALL(test_random_, random()).WillByDefault(Return(123456789));
    filter_ = std::make_shared<OAuth2Filter>(config_, std::move(oauth_client_ptr), test_time_,
                                             test_random_);
    filter_->setDecoderFilterCallbacks(decoder_callbacks_);
    filter_->setEncoderFilterCallbacks(encoder_callbacks_);
    validator_ = std::make_shared<MockOAuth2CookieValidator>();
    filter_->validator_ = validator_;
  }

  // Set up proto fields with standard config.
  FilterConfigSharedPtr getConfig(
      bool forward_bearer_token = true, bool use_refresh_token = false,
      ::envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType auth_type =
          ::envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
              OAuth2Config_AuthType_URL_ENCODED_BODY,
      int default_refresh_token_expires_in = 0, bool preserve_authorization_header = false,
      bool disable_id_token_set_cookie = false, bool set_cookie_domain = false,
      bool disable_access_token_set_cookie = false, bool disable_refresh_token_set_cookie = false,
      ::envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite bearer_samesite =
          ::envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite::
              CookieConfig_SameSite_DISABLED,
      ::envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite hmac_samesite =
          ::envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite::
              CookieConfig_SameSite_DISABLED,
      ::envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite expires_samesite =
          ::envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite::
              CookieConfig_SameSite_DISABLED,
      ::envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite id_token_samesite =
          ::envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite::
              CookieConfig_SameSite_DISABLED,
      ::envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite refresh_token_samesite =
          ::envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite::
              CookieConfig_SameSite_DISABLED,
      ::envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite nonce_samesite =
          ::envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite::
              CookieConfig_SameSite_DISABLED,
      ::envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite code_verifier_samesite =
          ::envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite::
              CookieConfig_SameSite_DISABLED,
      int csrf_token_expires_in = 0, int code_verifier_token_expires_in = 0) {
    envoy::extensions::filters::http::oauth2::v3::OAuth2Config p;
    auto* endpoint = p.mutable_token_endpoint();
    endpoint->set_cluster("auth.example.com");
    endpoint->set_uri("auth.example.com/_oauth");
    endpoint->mutable_timeout()->set_seconds(1);
    p.set_redirect_uri("%REQ(:scheme)%://%REQ(:authority)%" + TEST_CALLBACK);
    p.mutable_redirect_path_matcher()->mutable_path()->set_exact(TEST_CALLBACK);
    p.set_authorization_endpoint("https://auth.example.com/oauth/authorize/");
    p.mutable_signout_path()->mutable_path()->set_exact("/_signout");
    p.set_forward_bearer_token(forward_bearer_token);
    p.set_preserve_authorization_header(preserve_authorization_header);
    p.set_disable_id_token_set_cookie(disable_id_token_set_cookie);
    p.set_disable_access_token_set_cookie(disable_access_token_set_cookie);
    p.set_disable_refresh_token_set_cookie(disable_refresh_token_set_cookie);
    p.set_stat_prefix("my_prefix");

    auto* useRefreshToken = p.mutable_use_refresh_token();
    useRefreshToken->set_value(use_refresh_token);

    if (default_refresh_token_expires_in != 0) {
      auto* refresh_token_expires_in = p.mutable_default_refresh_token_expires_in();
      refresh_token_expires_in->set_seconds(default_refresh_token_expires_in);
    }

    if (csrf_token_expires_in != 0) {
      auto* expires_in = p.mutable_csrf_token_expires_in();
      expires_in->set_seconds(csrf_token_expires_in);
    }

    if (code_verifier_token_expires_in != 0) {
      auto* expires_in = p.mutable_code_verifier_token_expires_in();
      expires_in->set_seconds(code_verifier_token_expires_in);
    }

    p.set_auth_type(auth_type);
    p.add_auth_scopes("user");
    p.add_auth_scopes("openid");
    p.add_auth_scopes("email");
    p.add_resources("oauth2-resource");
    p.add_resources("http://example.com");
    p.add_resources("https://example.com/some/path%2F..%2F/utf8\xc3\x83;foo=bar?var1=1&var2=2");
    auto* matcher = p.add_pass_through_matcher();
    matcher->set_name(":method");
    matcher->mutable_string_match()->set_exact("OPTIONS");
    auto* deny_redirect_matcher = p.add_deny_redirect_matcher();
    deny_redirect_matcher->set_name("X-Requested-With");
    deny_redirect_matcher->mutable_string_match()->set_exact("XMLHttpRequest");
    auto credentials = p.mutable_credentials();
    credentials->set_client_id(TEST_CLIENT_ID);
    credentials->mutable_token_secret()->set_name("secret");
    credentials->mutable_hmac_secret()->set_name("hmac");
    // Skipping setting credentials.cookie_names field should give default cookie names:
    // BearerToken, OauthHMAC, and OauthExpires.
    if (set_cookie_domain) {
      credentials->set_cookie_domain("example.com");
    }

    // Initialize CookieConfigs
    auto* cookie_configs = p.mutable_cookie_configs();

    // Bearer Token Cookie Config
    auto* bearer_config = cookie_configs->mutable_bearer_token_cookie_config();
    bearer_config->set_same_site(bearer_samesite);

    // HMAC Cookie Config, Set value to disabled by default.
    auto* hmac_config = cookie_configs->mutable_oauth_hmac_cookie_config();
    hmac_config->set_same_site(hmac_samesite);

    // Set value to disabled by default.
    auto* expires_config = cookie_configs->mutable_oauth_expires_cookie_config();
    expires_config->set_same_site(expires_samesite);

    // Set value to disabled by default.
    auto* id_token_config = cookie_configs->mutable_id_token_cookie_config();
    id_token_config->set_same_site(id_token_samesite);

    // Set value to disabled by default.
    auto* refresh_token_config = cookie_configs->mutable_refresh_token_cookie_config();
    refresh_token_config->set_same_site(refresh_token_samesite);

    // Set value to disabled by default.
    auto* oauth_nonce_config = cookie_configs->mutable_oauth_nonce_cookie_config();
    oauth_nonce_config->set_same_site(nonce_samesite);

    // Set value to disabled by default.
    auto* code_verifier_config = cookie_configs->mutable_code_verifier_cookie_config();
    code_verifier_config->set_same_site(code_verifier_samesite);

    MessageUtil::validate(p, ProtobufMessage::getStrictValidationVisitor());

    // Create filter config.
    auto secret_reader = std::make_shared<MockSecretReader>();
    FilterConfigSharedPtr c = std::make_shared<FilterConfig>(
        p, factory_context_.server_factory_context_, secret_reader, scope_, "test.");

    return c;
  }

  // Validates the behavior of the cookie validator.
  void expectValidCookies(const CookieNames& cookie_names, const std::string& cookie_domain) {
    // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
    test_time_.setSystemTime(SystemTime(std::chrono::seconds(1000)));

    const auto expires_at_s = DateUtil::nowToSeconds(test_time_.timeSystem()) + 10;

    Http::TestRequestHeaderMapImpl request_headers{
        {Http::Headers::get().Host.get(), "traffic.example.com"},
        {Http::Headers::get().Path.get(), "/anypath"},
        {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
        {Http::Headers::get().Cookie.get(),
         fmt::format("{}={}", cookie_names.oauth_expires_, expires_at_s)},
        {Http::Headers::get().Cookie.get(),
         absl::StrCat(cookie_names.bearer_token_, "=" + TEST_ENCRYPTED_ACCESS_TOKEN)},
        {Http::Headers::get().Cookie.get(),
         absl::StrCat(cookie_names.oauth_hmac_, "=oMh0+qk68Y4ya4JGQqT+Ja1Y1X58Sc8iATRxPPPG5Yc=")},
    };

    auto cookie_validator =
        std::make_shared<OAuth2CookieValidator>(test_time_, cookie_names, cookie_domain);
    EXPECT_EQ(cookie_validator->token(), "");
    EXPECT_EQ(cookie_validator->refreshToken(), "");
    cookie_validator->setParams(request_headers, TEST_HMAC_SECRET);

    EXPECT_TRUE(cookie_validator->hmacIsValid());
    EXPECT_TRUE(cookie_validator->timestampIsValid());
    EXPECT_TRUE(cookie_validator->isValid());
    EXPECT_FALSE(cookie_validator->canUpdateTokenByRefreshToken());

    // If we advance time beyond 10s the timestamp should no longer be valid.
    test_time_.advanceTimeWait(std::chrono::seconds(11));

    EXPECT_FALSE(cookie_validator->timestampIsValid());
    EXPECT_FALSE(cookie_validator->isValid());
  }

  NiceMock<Event::MockTimer>* attachmentTimeout_timer_{};
  NiceMock<Server::Configuration::MockFactoryContext> factory_context_;
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks_;
  NiceMock<Http::MockStreamEncoderFilterCallbacks> encoder_callbacks_;
  NiceMock<Upstream::MockClusterManager> cm_;
  std::shared_ptr<MockOAuth2CookieValidator> validator_;
  std::shared_ptr<OAuth2Filter> filter_;
  MockOAuth2Client* oauth_client_;
  FilterConfigSharedPtr config_;
  Http::MockAsyncClientRequest request_;
  std::deque<Http::AsyncClient::Callbacks*> callbacks_;
  Stats::IsolatedStoreImpl store_;
  Stats::Scope& scope_{*store_.rootScope()};
  Event::SimulatedTimeSystem test_time_;
  NiceMock<Random::MockRandomGenerator> test_random_;
};

// Verifies that the OAuth SDSSecretReader correctly updates dynamic generic secret.
TEST_F(OAuth2Test, SdsDynamicGenericSecret) {
  NiceMock<Server::MockConfigTracker> config_tracker;
  Secret::SecretManagerImpl secret_manager{config_tracker};
  envoy::config::core::v3::ConfigSource config_source;

  NiceMock<Server::Configuration::MockTransportSocketFactoryContext> secret_context;
  NiceMock<LocalInfo::MockLocalInfo> local_info;
  Api::ApiPtr api = Api::createApiForTest();
  NiceMock<Init::MockManager> init_manager;
  Init::TargetHandlePtr init_handle;
  NiceMock<Event::MockDispatcher> dispatcher;
  EXPECT_CALL(secret_context.server_context_, localInfo()).WillRepeatedly(ReturnRef(local_info));
  EXPECT_CALL(secret_context.server_context_, api()).WillRepeatedly(ReturnRef(*api));
  EXPECT_CALL(secret_context.server_context_, mainThreadDispatcher())
      .WillRepeatedly(ReturnRef(dispatcher));
  EXPECT_CALL(secret_context, initManager()).Times(0);
  EXPECT_CALL(init_manager, add(_))
      .WillRepeatedly(Invoke([&init_handle](const Init::Target& target) {
        init_handle = target.createHandle("test");
      }));

  auto client_secret_provider = secret_manager.findOrCreateGenericSecretProvider(
      config_source, "client", secret_context.server_context_, init_manager);
  auto client_callback =
      secret_context.server_context_.cluster_manager_.subscription_factory_.callbacks_;
  auto token_secret_provider = secret_manager.findOrCreateGenericSecretProvider(
      config_source, "token", secret_context.server_context_, init_manager);
  auto token_callback =
      secret_context.server_context_.cluster_manager_.subscription_factory_.callbacks_;

  NiceMock<ThreadLocal::MockInstance> tls;
  SDSSecretReader secret_reader(std::move(client_secret_provider), std::move(token_secret_provider),
                                tls, *api);
  EXPECT_TRUE(secret_reader.clientSecret().empty());
  EXPECT_TRUE(secret_reader.hmacSecret().empty());

  const std::string yaml_client = R"EOF(
name: client
generic_secret:
  secret:
    inline_string: "client_test"
)EOF";

  envoy::extensions::transport_sockets::tls::v3::Secret typed_secret;
  TestUtility::loadFromYaml(yaml_client, typed_secret);
  const auto decoded_resources_client = TestUtility::decodeResources({typed_secret});

  EXPECT_TRUE(client_callback->onConfigUpdate(decoded_resources_client.refvec_, "").ok());
  EXPECT_EQ(secret_reader.clientSecret(), "client_test");
  EXPECT_EQ(secret_reader.hmacSecret(), "");

  const std::string yaml_token = R"EOF(
name: token
generic_secret:
  secret:
    inline_string: "token_test"
)EOF";
  TestUtility::loadFromYaml(yaml_token, typed_secret);
  const auto decoded_resources_token = TestUtility::decodeResources({typed_secret});

  EXPECT_TRUE(token_callback->onConfigUpdate(decoded_resources_token.refvec_, "").ok());
  EXPECT_EQ(secret_reader.clientSecret(), "client_test");
  EXPECT_EQ(secret_reader.hmacSecret(), "token_test");

  const std::string yaml_client_recheck = R"EOF(
name: client
generic_secret:
  secret:
    inline_string: "client_test_recheck"
)EOF";
  TestUtility::loadFromYaml(yaml_client_recheck, typed_secret);
  const auto decoded_resources_client_recheck = TestUtility::decodeResources({typed_secret});

  EXPECT_TRUE(client_callback->onConfigUpdate(decoded_resources_client_recheck.refvec_, "").ok());
  EXPECT_EQ(secret_reader.clientSecret(), "client_test_recheck");
  EXPECT_EQ(secret_reader.hmacSecret(), "token_test");
}
// Verifies that we fail constructing the filter if the configured cluster doesn't exist.
TEST_F(OAuth2Test, InvalidCluster) {
  ON_CALL(factory_context_.server_factory_context_.cluster_manager_, clusters())
      .WillByDefault(Return(Upstream::ClusterManager::ClusterInfoMaps()));

  EXPECT_THROW_WITH_MESSAGE(init(), EnvoyException,
                            "OAuth2 filter: unknown cluster 'auth.example.com' in config. Please "
                            "specify which cluster to direct OAuth requests to.");
}

// Verifies that we fail constructing the filter if the authorization endpoint isn't a valid URL.
TEST_F(OAuth2Test, InvalidAuthorizationEndpoint) {
  // Create a filter config with an invalid authorization_endpoint URL.
  envoy::extensions::filters::http::oauth2::v3::OAuth2Config p;
  auto* endpoint = p.mutable_token_endpoint();
  endpoint->set_cluster("auth.example.com");
  p.set_authorization_endpoint("INVALID_URL");
  // Add mandatory fields.
  p.set_redirect_uri("%REQ(:scheme)%://%REQ(:authority)%/redirected");
  p.mutable_redirect_path_matcher()->mutable_path()->set_exact("redirected");
  p.mutable_signout_path()->mutable_path()->set_exact("/_signout");

  // Attempt to create the OAuth config.
  auto secret_reader = std::make_shared<MockSecretReader>();
  EXPECT_THROW_WITH_MESSAGE(
      std::make_shared<FilterConfig>(p, factory_context_.server_factory_context_, secret_reader,
                                     scope_, "test."),
      EnvoyException, "OAuth2 filter: invalid authorization endpoint URL 'INVALID_URL' in config.");
}

// Verifies that the OAuth config is created with a default value for auth_scopes field when it is
// not set in proto/yaml.
TEST_F(OAuth2Test, DefaultAuthScope) {
  // Set up proto fields with no auth scope set.
  envoy::extensions::filters::http::oauth2::v3::OAuth2Config p;
  auto* endpoint = p.mutable_token_endpoint();
  endpoint->set_cluster("auth.example.com");
  endpoint->set_uri("auth.example.com/_oauth");
  endpoint->mutable_timeout()->set_seconds(1);
  p.set_redirect_uri("%REQ(:scheme)%://%REQ(:authority)%" + TEST_CALLBACK);
  p.mutable_redirect_path_matcher()->mutable_path()->set_exact(TEST_CALLBACK);
  p.set_authorization_endpoint("https://auth.example.com/oauth/authorize/");
  p.mutable_signout_path()->mutable_path()->set_exact("/_signout");
  p.set_forward_bearer_token(true);
  auto* matcher = p.add_pass_through_matcher();
  matcher->set_name(":method");
  matcher->mutable_string_match()->set_exact("OPTIONS");

  auto credentials = p.mutable_credentials();
  credentials->set_client_id(TEST_CLIENT_ID);
  credentials->mutable_token_secret()->set_name("secret");
  credentials->mutable_hmac_secret()->set_name("hmac");

  MessageUtil::validate(p, ProtobufMessage::getStrictValidationVisitor());

  // Create the OAuth config.
  auto secret_reader = std::make_shared<MockSecretReader>();
  FilterConfigSharedPtr test_config_;
  test_config_ = std::make_shared<FilterConfig>(p, factory_context_.server_factory_context_,
                                                secret_reader, scope_, "test.");

  // resource is optional
  EXPECT_EQ(test_config_->encodedResourceQueryParams(), "");

  // Recreate the filter with current config and test if the scope was added
  // as a query parameter in response headers.
  init(test_config_);
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
  };

  Http::TestResponseHeaderMapImpl response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "CodeVerifier=" + TEST_ENCRYPTED_CODE_VERIFIER + ";path=/;Max-Age=600;secure;HttpOnly"},

      {Http::Headers::get().Location.get(),
       "https://auth.example.com/oauth/"
       "authorize/?client_id=" +
           TEST_CLIENT_ID + "&code_challenge=" + TEST_CODE_CHALLENGE +
           "&code_challenge_method=S256" +
           "&redirect_uri=https%3A%2F%2Ftraffic.example.com%2F_oauth"
           "&response_type=code"
           "&scope=" +
           TEST_DEFAULT_SCOPE + "&state=" + TEST_ENCODED_STATE},
  };

  // explicitly tell the validator to fail the validation.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));
  EXPECT_CALL(*validator_, canUpdateTokenByRefreshToken()).WillOnce(Return(false));

  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
}

// Verifies that the CSRF token cookie expiration (Max-Age) uses the custom
// value from csrf_token_expires_in configuration.
TEST_F(OAuth2Test, CustomCsrfTokenExpiresIn) {
  // Create a filter config with a custom CSRF token expiration.
  envoy::extensions::filters::http::oauth2::v3::OAuth2Config p;
  auto* endpoint = p.mutable_token_endpoint();
  endpoint->set_cluster("auth.example.com");
  endpoint->set_uri("auth.example.com/_oauth");
  endpoint->mutable_timeout()->set_seconds(1);
  p.set_redirect_uri("%REQ(:scheme)%://%REQ(:authority)%" + TEST_CALLBACK);
  p.mutable_redirect_path_matcher()->mutable_path()->set_exact(TEST_CALLBACK);
  p.set_authorization_endpoint("https://auth.example.com/oauth/authorize/");
  p.mutable_signout_path()->mutable_path()->set_exact("/_signout");
  auto credentials = p.mutable_credentials();
  credentials->set_client_id(TEST_CLIENT_ID);
  credentials->mutable_token_secret()->set_name("secret");
  credentials->mutable_hmac_secret()->set_name("hmac");

  // Set custom CSRF token expiration
  const int custom_csrf_token_expires_in = 1234;
  auto* csrf_token_expires_in = p.mutable_csrf_token_expires_in();
  csrf_token_expires_in->set_seconds(custom_csrf_token_expires_in);

  // Create the OAuth config.
  auto secret_reader = std::make_shared<MockSecretReader>();
  FilterConfigSharedPtr test_config_;
  test_config_ = std::make_shared<FilterConfig>(p, factory_context_.server_factory_context_,
                                                secret_reader, scope_, "test.");

  init(test_config_);
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
  };

  // Explicitly tell the validator to fail the validation.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));
  EXPECT_CALL(*validator_, canUpdateTokenByRefreshToken()).WillOnce(Return(false));

  // Verify that the CSRF token cookie (OauthNonce) expiration is set to the custom value.
  Http::TestResponseHeaderMapImpl response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN +
           ";path=/;Max-Age=" + std::to_string(custom_csrf_token_expires_in) + ";secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "CodeVerifier=" + TEST_ENCRYPTED_CODE_VERIFIER + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Location.get(),
       "https://auth.example.com/oauth/"
       "authorize/?client_id=" +
           TEST_CLIENT_ID + "&code_challenge=" + TEST_CODE_CHALLENGE +
           "&code_challenge_method=S256" +
           "&redirect_uri=https%3A%2F%2Ftraffic.example.com%2F_oauth"
           "&response_type=code"
           "&scope=" +
           TEST_DEFAULT_SCOPE + "&state=" + TEST_ENCODED_STATE},
  };
  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
}

// Verifies that the code verifier token cookie expiration (Max-Age) uses the custom
// value from code_verifier_token_expires_in configuration.
TEST_F(OAuth2Test, CustomCodeVerifierTokenExpiresIn) {
  // Create a filter config with a custom code verifier token expiration.
  envoy::extensions::filters::http::oauth2::v3::OAuth2Config p;
  auto* endpoint = p.mutable_token_endpoint();
  endpoint->set_cluster("auth.example.com");
  endpoint->set_uri("auth.example.com/_oauth");
  endpoint->mutable_timeout()->set_seconds(1);
  p.set_redirect_uri("%REQ(:scheme)%://%REQ(:authority)%" + TEST_CALLBACK);
  p.mutable_redirect_path_matcher()->mutable_path()->set_exact(TEST_CALLBACK);
  p.set_authorization_endpoint("https://auth.example.com/oauth/authorize/");
  p.mutable_signout_path()->mutable_path()->set_exact("/_signout");
  auto credentials = p.mutable_credentials();
  credentials->set_client_id(TEST_CLIENT_ID);
  credentials->mutable_token_secret()->set_name("secret");
  credentials->mutable_hmac_secret()->set_name("hmac");

  // Set custom code verifier token expiration
  const int custom_code_verifier_token_expires_in = 1234;
  auto* code_verifier_token_expires_in = p.mutable_code_verifier_token_expires_in();
  code_verifier_token_expires_in->set_seconds(custom_code_verifier_token_expires_in);

  // Create the OAuth config.
  auto secret_reader = std::make_shared<MockSecretReader>();
  FilterConfigSharedPtr test_config_;
  test_config_ = std::make_shared<FilterConfig>(p, factory_context_.server_factory_context_,
                                                secret_reader, scope_, "test.");

  init(test_config_);
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
  };

  // Explicitly tell the validator to fail the validation.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));
  EXPECT_CALL(*validator_, canUpdateTokenByRefreshToken()).WillOnce(Return(false));

  // Verify that the CSRF token cookie (OauthNonce) expiration is set to the custom value.
  Http::TestResponseHeaderMapImpl response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "CodeVerifier=" + TEST_ENCRYPTED_CODE_VERIFIER + ";path=/;Max-Age=" +
           std::to_string(custom_code_verifier_token_expires_in) + ";secure;HttpOnly"},
      {Http::Headers::get().Location.get(),
       "https://auth.example.com/oauth/"
       "authorize/?client_id=" +
           TEST_CLIENT_ID + "&code_challenge=" + TEST_CODE_CHALLENGE +
           "&code_challenge_method=S256" +
           "&redirect_uri=https%3A%2F%2Ftraffic.example.com%2F_oauth"
           "&response_type=code"
           "&scope=" +
           TEST_DEFAULT_SCOPE + "&state=" + TEST_ENCODED_STATE},
  };
  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
}

// Verifies that query parameters in the authorization_endpoint URL are preserved.
TEST_F(OAuth2Test, PreservesQueryParametersInAuthorizationEndpoint) {
  // Create a filter config with an authorization_endpoint URL with query parameters.
  envoy::extensions::filters::http::oauth2::v3::OAuth2Config p;
  auto* endpoint = p.mutable_token_endpoint();
  endpoint->set_cluster("auth.example.com");
  endpoint->set_uri("auth.example.com/_oauth");
  endpoint->mutable_timeout()->set_seconds(1);
  p.set_redirect_uri("%REQ(:scheme)%://%REQ(:authority)%" + TEST_CALLBACK);
  p.mutable_redirect_path_matcher()->mutable_path()->set_exact(TEST_CALLBACK);
  p.set_authorization_endpoint("https://auth.example.com/oauth/authorize/?foo=bar");
  p.mutable_signout_path()->mutable_path()->set_exact("/_signout");
  auto credentials = p.mutable_credentials();
  credentials->set_client_id(TEST_CLIENT_ID);
  credentials->mutable_token_secret()->set_name("secret");
  credentials->mutable_hmac_secret()->set_name("hmac");

  // Create the OAuth config.
  auto secret_reader = std::make_shared<MockSecretReader>();
  FilterConfigSharedPtr test_config_;
  test_config_ = std::make_shared<FilterConfig>(p, factory_context_.server_factory_context_,
                                                secret_reader, scope_, "test.");
  init(test_config_);
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
  };

  // Explicitly tell the validator to fail the validation.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));
  EXPECT_CALL(*validator_, canUpdateTokenByRefreshToken()).WillOnce(Return(false));

  // Verify that the foo=bar query parameter is preserved in the redirect.
  Http::TestResponseHeaderMapImpl response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "CodeVerifier=" + TEST_ENCRYPTED_CODE_VERIFIER + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Location.get(),
       "https://auth.example.com/oauth/"
       "authorize/?client_id=" +
           TEST_CLIENT_ID + "&code_challenge=" + TEST_CODE_CHALLENGE +
           "&code_challenge_method=S256" +
           "&foo=bar"
           "&redirect_uri=https%3A%2F%2Ftraffic.example.com%2F_oauth"
           "&response_type=code"
           "&scope=" +
           TEST_DEFAULT_SCOPE + "&state=" + TEST_ENCODED_STATE},
  };
  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
}

TEST_F(OAuth2Test, PreservesQueryParametersInAuthorizationEndpointWithUrlEncoding) {
  // Create a filter config with an authorization_endpoint URL with query parameters.
  envoy::extensions::filters::http::oauth2::v3::OAuth2Config p;
  auto* endpoint = p.mutable_token_endpoint();
  endpoint->set_cluster("auth.example.com");
  endpoint->set_uri("auth.example.com/_oauth");
  endpoint->mutable_timeout()->set_seconds(1);
  p.set_redirect_uri("%REQ(:scheme)%://%REQ(:authority)%" + TEST_CALLBACK);
  p.mutable_redirect_path_matcher()->mutable_path()->set_exact(TEST_CALLBACK);
  p.set_authorization_endpoint("https://auth.example.com/oauth/authorize/?foo=bar");
  p.mutable_signout_path()->mutable_path()->set_exact("/_signout");
  auto credentials = p.mutable_credentials();
  credentials->set_client_id(TEST_CLIENT_ID);
  credentials->mutable_token_secret()->set_name("secret");
  credentials->mutable_hmac_secret()->set_name("hmac");

  // Create the OAuth config.
  auto secret_reader = std::make_shared<MockSecretReader>();
  FilterConfigSharedPtr test_config_;
  test_config_ = std::make_shared<FilterConfig>(p, factory_context_.server_factory_context_,
                                                secret_reader, scope_, "test.");
  init(test_config_);
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
  };

  // Explicitly tell the validator to fail the validation.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));
  EXPECT_CALL(*validator_, canUpdateTokenByRefreshToken()).WillOnce(Return(false));

  // Verify that the foo=bar query parameter is preserved in the redirect.
  Http::TestResponseHeaderMapImpl response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "CodeVerifier=" + TEST_ENCRYPTED_CODE_VERIFIER + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Location.get(),
       "https://auth.example.com/oauth/"
       "authorize/?client_id=" +
           TEST_CLIENT_ID + "&code_challenge=" + TEST_CODE_CHALLENGE +
           "&code_challenge_method=S256" +
           "&foo=bar"
           "&redirect_uri=https%3A%2F%2Ftraffic.example.com%2F_oauth"
           "&response_type=code"
           "&scope=" +
           TEST_DEFAULT_SCOPE + "&state=" + TEST_ENCODED_STATE},
  };
  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
}

/**
 * Scenario: The OAuth filter receives a sign out request.
 *
 * Expected behavior: the filter should redirect to the server name with cleared OAuth cookies.
 */
TEST_F(OAuth2Test, RequestSignout) {
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/_signout"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
  };

  Http::TestResponseHeaderMapImpl response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthHMAC=deleted; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=deleted; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=deleted; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=deleted; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT"},
      {Http::Headers::get().SetCookie.get(),
       "OauthNonce=deleted; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT"},
      {Http::Headers::get().SetCookie.get(),
       "CodeVerifier=deleted; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT"},
      {Http::Headers::get().Location.get(), "https://traffic.example.com/"},
  };
  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
}

/**
 * Scenario: The OAuth filter receives a sign out request when end session endpoint is configured.
 *
 * Expected behavior: the filter should redirect to the end session endpoint.
 */
TEST_F(OAuth2Test, RequestSignoutWhenEndSessionEndpointIsConfigured) {
  // Create a filter config with end session endpoint and openid scope.
  envoy::extensions::filters::http::oauth2::v3::OAuth2Config p;
  auto* endpoint = p.mutable_token_endpoint();
  endpoint->set_cluster("auth.example.com");
  endpoint->set_uri("auth.example.com/_oauth");
  endpoint->mutable_timeout()->set_seconds(1);
  p.set_redirect_uri("%REQ(:scheme)%://%REQ(:authority)%" + TEST_CALLBACK);
  p.mutable_redirect_path_matcher()->mutable_path()->set_exact(TEST_CALLBACK);
  p.set_authorization_endpoint("https://auth.example.com/oauth/authorize/");
  p.mutable_signout_path()->mutable_path()->set_exact("/_signout");
  auto credentials = p.mutable_credentials();
  credentials->set_client_id(TEST_CLIENT_ID);
  credentials->mutable_token_secret()->set_name("secret");
  credentials->mutable_hmac_secret()->set_name("hmac");
  p.set_end_session_endpoint("https://auth.example.com/oauth/logout");
  p.add_auth_scopes("openid");

  // Create the OAuth config.
  auto secret_reader = std::make_shared<MockSecretReader>();
  FilterConfigSharedPtr test_config_;
  test_config_ = std::make_shared<FilterConfig>(p, factory_context_.server_factory_context_,
                                                secret_reader, scope_, "test.");
  init(test_config_);

  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/_signout"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::Headers::get().Cookie.get(), "IdToken=xyztoken"},
  };

  Http::TestResponseHeaderMapImpl response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthHMAC=deleted; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=deleted; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=deleted; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=deleted; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT"},
      {Http::Headers::get().SetCookie.get(),
       "OauthNonce=deleted; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT"},
      {Http::Headers::get().SetCookie.get(),
       "CodeVerifier=deleted; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT"},
      {Http::Headers::get().Location.get(), "https://auth.example.com/oauth/"
                                            "logout?id_token_hint=xyztoken&client_id=1&post_logout_"
                                            "redirect_uri=https%3A%2F%2Ftraffic.example.com%2F"},
  };
  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
}

/**
 * Scenario: The OAuth filter receives a request to an arbitrary path with valid OAuth cookies
 * (cookie values and validation are mocked out)
 * In a real flow, the injected OAuth headers should be sanitized and replaced with legitimate
 * values.
 *
 * Expected behavior: the filter should let the request proceed, and sanitize the injected headers.
 */
TEST_F(OAuth2Test, OAuthOkPass) {
  Http::TestRequestHeaderMapImpl mock_request_headers{
      {Http::Headers::get().Path.get(), "/anypath"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::CustomHeaders::get().Authorization.get(), "Bearer injected_malice!"},
  };

  Http::TestRequestHeaderMapImpl expected_headers{
      {Http::Headers::get().Path.get(), "/anypath"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::CustomHeaders::get().Authorization.get(), "Bearer legit_token"},
  };

  // cookie-validation mocking
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(true));

  // Sanitized return reference mocking
  std::string legit_token{"legit_token"};
  EXPECT_CALL(*validator_, token()).WillRepeatedly(ReturnRef(legit_token));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue,
            filter_->decodeHeaders(mock_request_headers, false));

  // Ensure that existing OAuth forwarded headers got sanitized.
  EXPECT_EQ(mock_request_headers, expected_headers);

  EXPECT_EQ(scope_.counterFromString("test.my_prefix.oauth_failure").value(), 0);
  EXPECT_EQ(scope_.counterFromString("test.my_prefix.oauth_success").value(), 1);
}

/**
 * Scenario: The OAuth filter receives a request to an arbitrary path with valid OAuth cookies
 * (cookie values and validation are mocked out), but with an invalid token in the Authorization
 * header and forwarding bearer token is disabled.
 *
 * Expected behavior: the filter should sanitize the Authorization header and let the request
 * proceed.
 */
TEST_F(OAuth2Test, OAuthOkPassButInvalidToken) {
  init(getConfig(false /* forward_bearer_token */));

  Http::TestRequestHeaderMapImpl mock_request_headers{
      {Http::Headers::get().Path.get(), "/anypath"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::CustomHeaders::get().Authorization.get(), "Bearer injected_malice!"},
  };

  Http::TestRequestHeaderMapImpl expected_headers{
      {Http::Headers::get().Path.get(), "/anypath"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
  };

  // cookie-validation mocking
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(true));

  // Sanitized return reference mocking
  std::string legit_token{"legit_token"};
  EXPECT_CALL(*validator_, token()).WillRepeatedly(ReturnRef(legit_token));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue,
            filter_->decodeHeaders(mock_request_headers, false));

  // Ensure that existing OAuth forwarded headers got sanitized.
  EXPECT_EQ(mock_request_headers, expected_headers);

  EXPECT_EQ(scope_.counterFromString("test.my_prefix.oauth_failure").value(), 0);
  EXPECT_EQ(scope_.counterFromString("test.my_prefix.oauth_success").value(), 1);
}

/**
 * Scenario: The OAuth filter receives a request with a foreign token in the Authorization
 * header. This header should be forwarded when preserve authorization header is enabled
 * and forwarding bearer token is disabled.
 *
 * Expected behavior: the filter should forward the foreign token and let the request proceed.
 */
TEST_F(OAuth2Test, OAuthOkPreserveForeignAuthHeader) {
  init(getConfig(false /* forward_bearer_token */, true /* use_refresh_token */,
                 ::envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
                     OAuth2Config_AuthType_URL_ENCODED_BODY /* encoded_body_type */,
                 1200 /* default_refresh_token_expires_in */,
                 true /* preserve_authorization_header */));

  Http::TestRequestHeaderMapImpl mock_request_headers{
      {Http::Headers::get().Path.get(), "/anypath"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::CustomHeaders::get().Authorization.get(), "Bearer ValidAuthorizationHeader"},
  };

  Http::TestRequestHeaderMapImpl expected_headers{
      {Http::Headers::get().Path.get(), "/anypath"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::CustomHeaders::get().Authorization.get(), "Bearer ValidAuthorizationHeader"},
  };

  // cookie-validation mocking
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(true));

  // Sanitized return reference mocking
  std::string legit_token{"legit_token"};
  EXPECT_CALL(*validator_, token()).WillRepeatedly(ReturnRef(legit_token));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue,
            filter_->decodeHeaders(mock_request_headers, false));

  // Ensure that existing OAuth forwarded headers got sanitized.
  EXPECT_EQ(mock_request_headers, expected_headers);

  EXPECT_EQ(scope_.counterFromString("test.my_prefix.oauth_failure").value(), 0);
  EXPECT_EQ(scope_.counterFromString("test.my_prefix.oauth_success").value(), 1);
}

TEST_F(OAuth2Test, SetBearerToken) {
  init(getConfig(false /* forward_bearer_token */, true /* use_refresh_token */));

  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(1000)));

  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/_oauth?code=123&state=" + TEST_ENCODED_STATE},
      {Http::Headers::get().Cookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Cookie.get(),
       "CodeVerifier=" + TEST_ENCRYPTED_CODE_VERIFIER + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };

  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  // Sanitized return reference mocking
  // std::string legit_token{"legit_token"};
  // EXPECT_CALL(*validator_, token()).WillRepeatedly(ReturnRef(legit_token));

  EXPECT_CALL(*oauth_client_, asyncGetAccessToken("123", TEST_CLIENT_ID, "asdf_client_secret_fdsa",
                                                  "https://traffic.example.com" + TEST_CALLBACK,
                                                  TEST_CODE_VERIFIER, AuthType::UrlEncodedBody));

  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndBuffer,
            filter_->decodeHeaders(request_headers, false));

  // Expected response after the callback & validation is complete - verifying we kept the
  // state and method of the original request, including the query string parameters.
  Http::TestRequestHeaderMapImpl response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(), "OauthHMAC="
                                             "4TKyxPV/F7yyvr0XgJ2bkWFOc8t4IOFen1k29b84MAQ=;"
                                             "path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "OauthExpires=1600;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + TEST_ENCRYPTED_ID_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN + ";path=/;Max-Age=604800;secure;HttpOnly"},
      {Http::Headers::get().Location.get(),
       "https://traffic.example.com/original_path?var1=1&var2=2"},
  };

  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));

  filter_->onGetAccessTokenSuccess("access_code", "some-id-token", "some-refresh-token",
                                   std::chrono::seconds(600));

  EXPECT_EQ(scope_.counterFromString("test.my_prefix.oauth_failure").value(), 0);
  EXPECT_EQ(scope_.counterFromString("test.my_prefix.oauth_success").value(), 1);
}

TEST_F(OAuth2Test, SetBearerTokenWithEncryptionDisabled) {
  TestScopedRuntime scoped_runtime;
  scoped_runtime.mergeValues({{"envoy.reloadable_features.oauth2_encrypt_tokens", "false"}});

  init(getConfig(false /* forward_bearer_token */, true /* use_refresh_token */));

  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(1000)));

  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/_oauth?code=123&state=" + TEST_ENCODED_STATE},
      {Http::Headers::get().Cookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Cookie.get(),
       "CodeVerifier=" + TEST_ENCRYPTED_CODE_VERIFIER + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };

  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  EXPECT_CALL(*oauth_client_, asyncGetAccessToken("123", TEST_CLIENT_ID, "asdf_client_secret_fdsa",
                                                  "https://traffic.example.com" + TEST_CALLBACK,
                                                  TEST_CODE_VERIFIER, AuthType::UrlEncodedBody));

  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndBuffer,
            filter_->decodeHeaders(request_headers, false));

  // Expected response after the callback & validation is complete - verifying we kept the
  // state and method of the original request, including the query string parameters.
  Http::TestRequestHeaderMapImpl response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(), "OauthHMAC="
                                             "4TKyxPV/F7yyvr0XgJ2bkWFOc8t4IOFen1k29b84MAQ=;"
                                             "path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "OauthExpires=1600;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=access_code;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=some-id-token;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=some-refresh-token;path=/;Max-Age=604800;secure;HttpOnly"},
      {Http::Headers::get().Location.get(),
       "https://traffic.example.com/original_path?var1=1&var2=2"},
  };

  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));

  filter_->onGetAccessTokenSuccess("access_code", "some-id-token", "some-refresh-token",
                                   std::chrono::seconds(600));

  EXPECT_EQ(scope_.counterFromString("test.my_prefix.oauth_failure").value(), 0);
  EXPECT_EQ(scope_.counterFromString("test.my_prefix.oauth_success").value(), 1);
}

/**
 * Scenario: The OAuth filter receives a request without valid OAuth cookies to a non-callback URL
 * (indicating that the user needs to re-validate cookies or get 401'd).
 * This also tests both a forwarded http protocol from upstream and a plaintext connection.
 *
 * Expected behavior: the filter should redirect the user to the OAuth server with the credentials
 * in the query parameters.
 */
TEST_F(OAuth2Test, OAuthErrorNonOAuthHttpCallback) {
  // First construct the initial request to the oauth filter with URI parameters.
  Http::TestRequestHeaderMapImpl first_request_headers{
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Post},
      {Http::Headers::get().Scheme.get(), "https"},
  };

  // This is the immediate response - a redirect to the auth cluster.
  Http::TestResponseHeaderMapImpl first_response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "CodeVerifier=" + TEST_ENCRYPTED_CODE_VERIFIER + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Location.get(),
       "https://auth.example.com/oauth/"
       "authorize/?client_id=" +
           TEST_CLIENT_ID + "&code_challenge=" + TEST_CODE_CHALLENGE +
           "&code_challenge_method=S256" +
           "&redirect_uri=https%3A%2F%2Ftraffic.example.com%2F_oauth"
           "&response_type=code"
           "&scope=" +
           TEST_ENCODED_AUTH_SCOPES + "&state=" + TEST_ENCODED_STATE + "&resource=oauth2-resource" +
           "&resource=http%3A%2F%2Fexample.com"
           "&resource=https%3A%2F%2Fexample.com%2Fsome%2Fpath%252F..%252F%2Futf8%C3%83%3Bfoo%3Dbar%"
           "3Fvar1%3D1%26var2%3D2"},
  };

  // Fail the validation to trigger the OAuth flow.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  // Check that the redirect includes the URL encoded query parameter characters
  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&first_response_headers), true));

  // This represents the beginning of the OAuth filter.
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(first_request_headers, false));

  // This represents the callback request from the authorization server.
  Http::TestRequestHeaderMapImpl second_request_headers{
      {Http::Headers::get().Path.get(), "/_oauth?code=123&state=" + TEST_ENCODED_STATE},
      {Http::Headers::get().Cookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Cookie.get(),
       "CodeVerifier=" + TEST_ENCRYPTED_CODE_VERIFIER + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
  };

  // Deliberately fail the HMAC validation check.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  EXPECT_CALL(*oauth_client_, asyncGetAccessToken("123", TEST_CLIENT_ID, "asdf_client_secret_fdsa",
                                                  "https://traffic.example.com" + TEST_CALLBACK,
                                                  TEST_CODE_VERIFIER, AuthType::UrlEncodedBody));

  // Invoke the callback logic. As a side effect, state_ will be populated.
  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndBuffer,
            filter_->decodeHeaders(second_request_headers, false));

  EXPECT_EQ(1, config_->stats().oauth_unauthorized_rq_.value());
  EXPECT_EQ(config_->clusterName(), "auth.example.com");

  // Expected response after the callback & validation is complete - verifying we kept the
  // state and method of the original request, including the query string parameters.
  Http::TestRequestHeaderMapImpl second_response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(), "OauthHMAC="
                                             "fV62OgLipChTQQC3UFgDp+l5sCiSb3zt7nCoJiVivWw=;"
                                             "path=/;Max-Age=;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(), "OauthExpires=;path=/;Max-Age=;secure;HttpOnly"},
      {Http::Headers::get().Location.get(),
       "https://traffic.example.com/original_path?var1=1&var2=2"},
  };

  EXPECT_CALL(decoder_callbacks_,
              encodeHeaders_(HeaderMapEqualRef(&second_response_headers), true));

  filter_->finishGetAccessTokenFlow();

  // Deliberately fail the HMAC validation check.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  EXPECT_CALL(*oauth_client_, asyncGetAccessToken("123", TEST_CLIENT_ID, "asdf_client_secret_fdsa",
                                                  "https://traffic.example.com" + TEST_CALLBACK,
                                                  TEST_CODE_VERIFIER, AuthType::UrlEncodedBody));

  // Invoke the callback logic. As a side effect, state_ will be populated.
  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndBuffer,
            filter_->decodeHeaders(second_request_headers, false));

  EXPECT_EQ(1, config_->stats().oauth_unauthorized_rq_.value());
  EXPECT_EQ(config_->clusterName(), "auth.example.com");
}

/**
 * Scenario: The OAuth filter receives a callback request with an error code
 */
TEST_F(OAuth2Test, OAuthErrorQueryString) {
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/_oauth?error=someerrorcode"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };

  Http::TestResponseHeaderMapImpl response_headers{
      {Http::Headers::get().Status.get(), "401"},
      {Http::Headers::get().ContentLength.get(), "18"}, // unauthorizedBodyMessage()
      {Http::Headers::get().ContentType.get(), "text/plain"},
  };

  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), false));
  EXPECT_CALL(decoder_callbacks_, encodeData(_, true));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));

  EXPECT_EQ(scope_.counterFromString("test.my_prefix.oauth_failure").value(), 1);
  EXPECT_EQ(scope_.counterFromString("test.my_prefix.oauth_success").value(), 0);
}

/**
 * Scenario: The OAuth filter receives a callback request from the OAuth server.
 *
 * Expected behavior: the filter should pause the request and call the OAuth client to get the
 * tokens.
 */
TEST_F(OAuth2Test, OAuthCallbackStartsAuthentication) {
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/_oauth?code=123&state=" + TEST_ENCODED_STATE},
      {Http::Headers::get().Cookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Cookie.get(),
       "CodeVerifier=" + TEST_ENCRYPTED_CODE_VERIFIER + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };

  // Deliberately fail the HMAC Validation check.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  EXPECT_CALL(*oauth_client_, asyncGetAccessToken("123", TEST_CLIENT_ID, "asdf_client_secret_fdsa",
                                                  "https://traffic.example.com" + TEST_CALLBACK,
                                                  TEST_CODE_VERIFIER, AuthType::UrlEncodedBody));

  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndBuffer,
            filter_->decodeHeaders(request_headers, false));
}

/**
 * Scenario: The OAuth filter receives a callback request from the OAuth server that has
 * an invalid CodeVerifier cookie.
 *
 * Expected behavior: the filter should fail the request and return a 401 Unauthorized response.
 */
TEST_F(OAuth2Test, OAuthCallbackWithInvalidCodeVerifierCookie) {
  static const std::string invalid_encrypted_code_verifier = "Fc1bBwAAAAAVzVsHAAAAABjf";
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/_oauth?code=123&state=" + TEST_ENCODED_STATE},
      {Http::Headers::get().Cookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Cookie.get(),
       "CodeVerifier=" + invalid_encrypted_code_verifier + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };

  // Deliberately fail the HMAC Validation check.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  EXPECT_CALL(decoder_callbacks_, sendLocalReply(Http::Code::Unauthorized, _, _, _, _));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
}

/**
 * Scenario: The OAuth filter receives a callback request from the OAuth server that lacks
 * the CodeVerifier cookie.
 *
 * Expected behavior: the filter should fail the request and return a 401 Unauthorized response.
 */
TEST_F(OAuth2Test, OAuthCallbackWithoutCodeVerifierCookie) {
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/_oauth?code=123&state=" + TEST_ENCODED_STATE},
      {Http::Headers::get().Cookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };

  // Deliberately fail the HMAC Validation check.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  EXPECT_CALL(decoder_callbacks_, sendLocalReply(Http::Code::Unauthorized, _, _, _, _));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
}

/**
 * Scenario: The OAuth filter receives a callback request from the OAuth server that lacks a CSRF
 * token. This scenario simulates a CSRF attack where the original OAuth request was inserted to the
 * user's browser by a malicious actor, and the user was tricked into clicking on the link.
 *
 * Expected behavior: the filter should fail the request and return a 401 Unauthorized response.
 */
TEST_F(OAuth2Test, OAuthCallbackStartsAuthenticationNoCsrfToken) {
  // {"url":"https://traffic.example.com/original_path?var1=1&var2=2"}
  static const std::string state_without_csrf_token =
      "eyJ1cmwiOiJodHRwczovL3RyYWZmaWMuZXhhbXBsZS5jb20vb3JpZ2luYWxfcGF0aD92YXIxPTEmdmFyMj0yIn0";
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/_oauth?code=123&state=" + state_without_csrf_token},
      {Http::Headers::get().Cookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };

  // Deliberately fail the HMAC Validation check.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  EXPECT_CALL(decoder_callbacks_, sendLocalReply(Http::Code::Unauthorized, _, _, _, _));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
}

/**
 * Scenario: The OAuth filter receives a callback request from the OAuth server that has an invalid
 * CSRF token (without Dot). This scenario simulates a CSRF attack where the original OAuth request
 * was inserted to the user's browser by a malicious actor, and the user was tricked into clicking
 * on the link.
 *
 * Expected behavior: the filter should fail the request and return a 401 Unauthorized response.
 */
TEST_F(OAuth2Test, OAuthCallbackStartsAuthenticationInvalidCsrfTokenWithoutDot) {
  // {"url":"https://traffic.example.com/original_path?var1=1&var2=2","csrf_token":"${extracted}"}
  static const std::string state_with_invalid_csrf_token =
      "eyJ1cmwiOiJodHRwczovL3RyYWZmaWMuZXhhbXBsZS5jb20vb3JpZ2luYWxfcGF0aD92YXIxPTEmdmFyMj0yIiwiY3Ny"
      "Zl90b2tlbiI6IjAwMDAwMDAwMDc1YmNkMTUifQ";
  static const std::string invalid_csrf_token_cookie = "00000000075bcd15";
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/_oauth?code=123&state=" + state_with_invalid_csrf_token},
      {Http::Headers::get().Cookie.get(),
       "OauthNonce=" + invalid_csrf_token_cookie + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };

  // Deliberately fail the HMAC Validation check.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  EXPECT_CALL(decoder_callbacks_, sendLocalReply(Http::Code::Unauthorized, _, _, _, _));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
}

/**
 * Scenario: The OAuth filter receives a callback request from the OAuth server that has an invalid
 * CSRF token (hmac doesn't match). This scenario simulates a CSRF attack where the original OAuth
 * request was inserted to the user's browser by a malicious actor, and the user was tricked into
 * clicking on the link.
 *
 * Expected behavior: the filter should fail the request and return a 401 Unauthorized response.
 */
TEST_F(OAuth2Test, OAuthCallbackStartsAuthenticationInvalidCsrfTokenInvalidHmac) {
  // {"url":"https://traffic.example.com/original_path?var1=1&var2=2","csrf_token":"${extracted}"}
  static const std::string state_with_invalid_csrf_token =
      "eyJ1cmwiOiJodHRwczovL3RyYWZmaWMuZXhhbXBsZS5jb20vb3JpZ2luYWxfcGF0aD92YXIxPTEmdmFyMj0yIiwiY3Ny"
      "Zl90b2tlbiI6IjAwMDAwMDAwMDc1YmNkMTUuaW52YWxpZGhtYWMifQ";
  static const std::string invalid_csrf_token_cookie = "00000000075bcd15.invalidhmac";
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/_oauth?code=123&state=" + state_with_invalid_csrf_token},
      {Http::Headers::get().Cookie.get(),
       "OauthNonce=" + invalid_csrf_token_cookie + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };

  // Deliberately fail the HMAC Validation check.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  EXPECT_CALL(decoder_callbacks_, sendLocalReply(Http::Code::Unauthorized, _, _, _, _));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
}

/**
 * Scenario: The OAuth filter receives a callback request from the OAuth server that has a malformed
 * state. This scenario simulates a CSRF attack where the original OAuth request was inserted to the
 * user's browser by a malicious actor, and the user was tricked into clicking on the link.
 *
 * Expected behavior: the filter should fail the request and return a 401 Unauthorized response.
 */
TEST_F(OAuth2Test, OAuthCallbackStartsAuthenticationMalformedState) {
  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(0)));

  // {"url":"https://traffic.example.com/original_path?var1=1&var2=2","csrf_token":"}
  static const std::string state_with_invalid_csrf_token_json =
      "eyJ1cmwiOiJodHRwczovL3RyYWZmaWMuZXhhbXBsZS5jb20vb3JpZ2luYWxfcGF0aD92YXIxPTEmdmFyMj0yIiwiY3Ny"
      "Zl90b2tlbiI6In0";

  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(),
       "/_oauth?code=123&state=" + state_with_invalid_csrf_token_json},
      {Http::Headers::get().Cookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };

  // Deliberately fail the HMAC Validation check.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  EXPECT_CALL(decoder_callbacks_, sendLocalReply(Http::Code::Unauthorized, _, _, _, _));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
}

/**
 * Scenario: The OAuth filter receives a request with an invalid CSRF token cookie.
 * This scenario simulates an attacker trying to forge a CSRF token.
 *
 * Expected behavior: the filter will ignore the invalid CSRF token and generate a new one
 */
TEST_F(OAuth2Test, RedirectToOAuthServerWithInvalidCSRFToken) {
  static const std::string invalid_csrf_token = "00000000075bcd15.invalidhmac";
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::Headers::get().Cookie.get(), "OauthNonce=" + invalid_csrf_token},
  };

  // Explicitly fail the validation to trigger the OAuth flow.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  Http::TestResponseHeaderMapImpl response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "CodeVerifier=" + TEST_ENCRYPTED_CODE_VERIFIER + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Location.get(),
       "https://auth.example.com/oauth/"
       "authorize/?client_id=" +
           TEST_CLIENT_ID + "&code_challenge=" + TEST_CODE_CHALLENGE +
           "&code_challenge_method=S256" +
           "&redirect_uri=https%3A%2F%2Ftraffic.example.com%2F_oauth"
           "&response_type=code"
           "&scope=" +
           TEST_ENCODED_AUTH_SCOPES + "&state=" + TEST_ENCODED_STATE + "&resource=oauth2-resource" +
           "&resource=http%3A%2F%2Fexample.com"
           "&resource=https%3A%2F%2Fexample.com%2Fsome%2Fpath%252F..%252F%2Futf8%C3%83%3Bfoo%3Dbar%"
           "3Fvar1%3D1%26var2%3D2"},
  };

  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
}

/**
 * Scenario: Protoc in opted-in to allow OPTIONS requests to pass-through. This is important as
 * POST requests initiate an OPTIONS request first in order to ensure POST is supported. During a
 * preflight request where the client Javascript initiates a remote call to a different endpoint,
 * we don't want to fail the call immediately due to browser restrictions, and use existing
 * cookies instead (OPTIONS requests do not send OAuth cookies.)
 */
TEST_F(OAuth2Test, OAuthOptionsRequestAndContinue) {
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/anypath"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Options},
      {Http::CustomHeaders::get().Authorization.get(), "Bearer xyz-header-token"}};

  Http::TestRequestHeaderMapImpl expected_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/anypath"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Options},
      {Http::CustomHeaders::get().Authorization.get(), "Bearer xyz-header-token"}};

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ(request_headers, expected_headers);
  EXPECT_EQ(scope_.counterFromString("test.my_prefix.oauth_failure").value(), 0);
  EXPECT_EQ(scope_.counterFromString("test.my_prefix.oauth_passthrough").value(), 1);
  EXPECT_EQ(scope_.counterFromString("test.my_prefix.oauth_success").value(), 0);
}

/**
 * Scenario: The OAuth filter receives a request without valid OAuth cookies to a non-callback URL
 * that matches the deny_redirect_matcher.
 *
 * Expected behavior: the filter should should return 401 Unauthorized response.
 */
TEST_F(OAuth2Test, AjaxDoesNotRedirect) {
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/anypath"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Post},
      {Http::Headers::get().Scheme.get(), "https"},
      {"X-Requested-With", "XMLHttpRequest"},
  };

  // explicitly tell the validator to fail the validation.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  // Unauthorized response is expected instead of 302 redirect.
  EXPECT_CALL(decoder_callbacks_, sendLocalReply(Http::Code::Unauthorized, _, _, _, _));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));

  EXPECT_EQ(1, config_->stats().oauth_failure_.value());
  EXPECT_EQ(0, config_->stats().oauth_unauthorized_rq_.value());
}

// Validates the behavior of the cookie validator.
TEST_F(OAuth2Test, CookieValidator) {
  expectValidCookies(CookieNames{"BearerToken", "OauthHMAC", "OauthExpires", "IdToken",
                                 "RefreshToken", "OauthNonce", "CodeVerifier"},
                     "");
}

// Validates the behavior of the cookie validator with custom cookie names.
TEST_F(OAuth2Test, CookieValidatorWithCustomNames) {
  expectValidCookies(CookieNames{"CustomBearerToken", "CustomOauthHMAC", "CustomOauthExpires",
                                 "CustomIdToken", "CustomRefreshToken", "CustomOauthNonce",
                                 "CustomCodeVerifier"},
                     "");
}

// Validates the behavior of the cookie validator with custom cookie domain.
TEST_F(OAuth2Test, CookieValidatorWithCookieDomain) {
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(0)));
  auto cookie_names = CookieNames{"BearerToken",  "OauthHMAC",  "OauthExpires", "IdToken",
                                  "RefreshToken", "OauthNonce", "CodeVerifier"};
  const auto expires_at_s = DateUtil::nowToSeconds(test_time_.timeSystem()) + 5;

  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/anypath"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Cookie.get(),
       fmt::format("{}={}", cookie_names.oauth_expires_, expires_at_s)},
      {Http::Headers::get().Cookie.get(),
       absl::StrCat(cookie_names.bearer_token_, "=", TEST_ENCRYPTED_ACCESS_TOKEN)},
      {Http::Headers::get().Cookie.get(),
       absl::StrCat(cookie_names.oauth_hmac_, "=PHLtlCLTIjfuAocmHmW8QzM3YSTRF6L+E3o6a1+TiS4=")},
  };

  auto cookie_validator =
      std::make_shared<OAuth2CookieValidator>(test_time_, cookie_names, "example.com");

  EXPECT_EQ(cookie_validator->token(), "");
  EXPECT_EQ(cookie_validator->refreshToken(), "");
  cookie_validator->setParams(request_headers, TEST_HMAC_SECRET);

  EXPECT_TRUE(cookie_validator->hmacIsValid());
  EXPECT_TRUE(cookie_validator->timestampIsValid());
  EXPECT_TRUE(cookie_validator->isValid());
}

// Validates the behavior of the cookie validator when the combination of some fields could be same.
TEST_F(OAuth2Test, CookieValidatorSame) {
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(0)));
  auto cookie_names = CookieNames{"BearerToken",  "OauthHMAC",  "OauthExpires", "IdToken",
                                  "RefreshToken", "OauthNonce", "CodeVerifier"};
  const auto expires_at_s = DateUtil::nowToSeconds(test_time_.timeSystem()) + 5;

  // Host name is `traffic.example.com:101` and the expire time is 5.
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com:101"},
      {Http::Headers::get().Path.get(), "/anypath"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Cookie.get(),
       fmt::format("{}={}", cookie_names.oauth_expires_, expires_at_s)},
      {Http::Headers::get().Cookie.get(),
       absl::StrCat(cookie_names.bearer_token_, "=", TEST_ENCRYPTED_ACCESS_TOKEN)},
      {Http::Headers::get().Cookie.get(),
       absl::StrCat(cookie_names.oauth_hmac_, "=eYef0itomg0CAjYygAfCLwmS2s1DaiL+N1Ql5V48o4o=")},
  };

  auto cookie_validator = std::make_shared<OAuth2CookieValidator>(test_time_, cookie_names, "");
  EXPECT_EQ(cookie_validator->token(), "");
  cookie_validator->setParams(request_headers, TEST_HMAC_SECRET);

  EXPECT_TRUE(cookie_validator->hmacIsValid());
  EXPECT_TRUE(cookie_validator->timestampIsValid());
  EXPECT_TRUE(cookie_validator->isValid());

  // If we advance time beyond 5s the timestamp should no longer be valid.
  test_time_.advanceTimeWait(std::chrono::seconds(6));

  EXPECT_FALSE(cookie_validator->timestampIsValid());
  EXPECT_FALSE(cookie_validator->isValid());

  test_time_.setSystemTime(SystemTime(std::chrono::seconds(0)));
  const auto new_expires_at_s = DateUtil::nowToSeconds(test_time_.timeSystem()) + 15;

  // Host name is `traffic.example.com:10` and the expire time is 15.
  // HMAC should be different from the above one with the separator fix.
  Http::TestRequestHeaderMapImpl request_headers_second{
      {Http::Headers::get().Host.get(), "traffic.example.com:10"},
      {Http::Headers::get().Path.get(), "/anypath"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Cookie.get(),
       fmt::format("{}={}", cookie_names.oauth_expires_, new_expires_at_s)},
      {Http::Headers::get().Cookie.get(),
       absl::StrCat(cookie_names.bearer_token_, "=", TEST_ENCRYPTED_ACCESS_TOKEN)},
      {Http::Headers::get().Cookie.get(),
       absl::StrCat(cookie_names.oauth_hmac_, "=VSTrKslW8ZNUqwgP+6Ocm1+7+NcF8GG/e1dqKsq14rc=")},
  };

  cookie_validator->setParams(request_headers_second, TEST_HMAC_SECRET);

  EXPECT_TRUE(cookie_validator->hmacIsValid());
  EXPECT_TRUE(cookie_validator->timestampIsValid());
  EXPECT_TRUE(cookie_validator->isValid());

  // If we advance time beyond 15s the timestamp should no longer be valid.
  test_time_.advanceTimeWait(std::chrono::seconds(16));

  EXPECT_FALSE(cookie_validator->timestampIsValid());
  EXPECT_FALSE(cookie_validator->isValid());
}

// Validates the behavior of the cookie validator when the expires_at value is not a valid integer.
TEST_F(OAuth2Test, CookieValidatorInvalidExpiresAt) {
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/anypath"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Cookie.get(), "OauthExpires=notanumber"},
      {Http::Headers::get().Cookie.get(), "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN},
      {Http::Headers::get().Cookie.get(), "OauthHMAC="
                                          "042KfjoL8OTsm8r4l6IO5dlxjzkaTDSyCaAibGI00bM="},
  };

  auto cookie_validator = std::make_shared<OAuth2CookieValidator>(
      test_time_,
      CookieNames{"BearerToken", "OauthHMAC", "OauthExpires", "IdToken", "RefreshToken",
                  "OauthNonce", "CodeVerifier"},
      "");
  cookie_validator->setParams(request_headers, TEST_HMAC_SECRET);

  EXPECT_TRUE(cookie_validator->hmacIsValid());
  EXPECT_FALSE(cookie_validator->timestampIsValid());
  EXPECT_FALSE(cookie_validator->isValid());
}

// Validates the behavior of the cookie validator when the expires_at value is not a valid integer.
TEST_F(OAuth2Test, CookieValidatorCanUpdateToken) {
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/anypath"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Cookie.get(), "OauthExpires=notanumber"},
      {Http::Headers::get().Cookie.get(), "BearerToken=xyztoken;RefreshToken=dsdtoken;"},
  };

  auto cookie_validator = std::make_shared<OAuth2CookieValidator>(
      test_time_,
      CookieNames("BearerToken", "OauthHMAC", "OauthExpires", "IdToken", "RefreshToken",
                  "OauthNonce", "CodeVerifier"),
      "");
  cookie_validator->setParams(request_headers, "mock-secret");

  EXPECT_TRUE(cookie_validator->canUpdateTokenByRefreshToken());
}

// Verify that we 401 the request if the state query param doesn't contain a valid URL.
TEST_F(OAuth2Test, OAuthTestInvalidUrlInStateQueryParam) {
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(0)));

  static const std::string state_with_invalid_url =
      "eyJ1cmwiOiJibGFoIiwiY3NyZl90b2tlbiI6IjAwMDAwMDAwMDc1YmNkMTUubmE2a3J1NHgxcEhnb2NTSWVVL21kdEhZ"
      "bjU4R2gxYnF3ZVM0WFhvaXFWZz0ifQ";
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Path.get(),
       "/_oauth?code=abcdefxyz123&scope=" + TEST_ENCODED_AUTH_SCOPES +
           "&state=" + state_with_invalid_url},
      {Http::Headers::get().Cookie.get(), "OauthExpires=123"},
      {Http::Headers::get().Cookie.get(), "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN},
      {Http::Headers::get().Cookie.get(),
       "OauthHMAC="
       "ZTRlMzU5N2Q4ZDIwZWE5ZTU5NTg3YTU3YTcxZTU0NDFkMzY1ZTc1NjMyODYyMj"
       "RlNjMxZTJmNTZkYzRmZTM0ZQ===="},
      {Http::Headers::get().Cookie.get(), "OauthNonce=" + TEST_CSRF_TOKEN},
  };

  Http::TestRequestHeaderMapImpl expected_headers{
      {Http::Headers::get().Status.get(), "401"},
      {Http::Headers::get().ContentLength.get(), "18"},
      {Http::Headers::get().ContentType.get(), "text/plain"},
      // Invalid URL: we inject a few : in the middle of the URL.
  };

  // Succeed the HMAC validation.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(true));

  std::string legit_token{"access_code"};
  EXPECT_CALL(*validator_, token()).WillRepeatedly(ReturnRef(legit_token));

  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&expected_headers), false));
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
}

// Verify that we 401 the request if the state query param contains the callback URL.
TEST_F(OAuth2Test, OAuthTestCallbackUrlInStateQueryParam) {
  // {"url":"https://traffic.example.com/_oauth","csrf_token":"${extracted}"}
  static const std::string state_with_callback_url =
      "eyJ1cmwiOiJodHRwczovL3RyYWZmaWMuZXhhbXBsZS5jb20vX29hdXRoIiwiY3NyZl90b2tlbiI6IjAwMDAwMDAwMDc1"
      "YmNkMTUubmE2a3J1NHgxcEhnb2NTSWVVL21kdEhZbjU4R2gxYnF3ZVM0WFhvaXFWZz0ifSA";

  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Path.get(),
       "/_oauth?code=abcdefxyz123&scope=" + TEST_ENCODED_AUTH_SCOPES +
           "&state=" + state_with_callback_url},

      {Http::Headers::get().Cookie.get(), "OauthExpires=123"},
      {Http::Headers::get().Cookie.get(), "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN},
      {Http::Headers::get().Cookie.get(),
       "OauthHMAC="
       "ZTRlMzU5N2Q4ZDIwZWE5ZTU5NTg3YTU3YTcxZTU0NDFkMzY1ZTc1NjMyODYyMj"
       "RlNjMxZTJmNTZkYzRmZTM0ZQ===="},
      {Http::Headers::get().Cookie.get(), "OauthNonce=" + TEST_CSRF_TOKEN},
  };

  Http::TestRequestHeaderMapImpl expected_response_headers{
      {Http::Headers::get().Status.get(), "401"},
      {Http::Headers::get().ContentLength.get(), "18"},
      {Http::Headers::get().ContentType.get(), "text/plain"},
  };

  // Succeed the HMAC validation.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(true));

  std::string legit_token{"access_code"};
  EXPECT_CALL(*validator_, token()).WillRepeatedly(ReturnRef(legit_token));

  EXPECT_CALL(decoder_callbacks_,
              encodeHeaders_(HeaderMapEqualRef(&expected_response_headers), false));
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
}

TEST_F(OAuth2Test, OAuthTestUpdatePathAfterSuccess) {
  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(0)));
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Path.get(),
       "/_oauth?code=abcdefxyz123&scope=" + TEST_ENCODED_AUTH_SCOPES +
           "&state=" + TEST_ENCODED_STATE},
      {Http::Headers::get().Cookie.get(), "OauthExpires=123"},
      {Http::Headers::get().Cookie.get(), "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN},
      {Http::Headers::get().Cookie.get(),
       "OauthHMAC="
       "ZTRlMzU5N2Q4ZDIwZWE5ZTU5NTg3YTU3YTcxZTU0NDFkMzY1ZTc1NjMyODYyMj"
       "RlNjMxZTJmNTZkYzRmZTM0ZQ===="},
      {Http::Headers::get().Cookie.get(), "OauthNonce=" + TEST_CSRF_TOKEN},
  };

  Http::TestRequestHeaderMapImpl expected_response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().Location.get(),
       "https://traffic.example.com/original_path?var1=1&var2=2"},
  };

  // Succeed the HMAC validation.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(true));

  std::string legit_token{"access_code"};
  EXPECT_CALL(*validator_, token()).WillRepeatedly(ReturnRef(legit_token));

  EXPECT_CALL(decoder_callbacks_,
              encodeHeaders_(HeaderMapEqualRef(&expected_response_headers), true));
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));

  EXPECT_EQ(request_headers.getHostValue(), "traffic.example.com");
  EXPECT_EQ(request_headers.getMethodValue(), Http::Headers::get().MethodValues.Get);
  EXPECT_EQ(request_headers.getPathValue(),
            "/_oauth?code=abcdefxyz123&scope=" + TEST_ENCODED_AUTH_SCOPES +
                "&state=" + TEST_ENCODED_STATE);
  auto auth_header = request_headers.get(Http::CustomHeaders::get().Authorization);
  EXPECT_EQ(auth_header[0]->value().getStringView(), "Bearer access_code");

  auto cookies = Http::Utility::parseCookies(request_headers);
  EXPECT_EQ(cookies["OauthExpires"], "123");
  EXPECT_EQ(cookies["BearerToken"], "access_code");
  EXPECT_EQ(
      cookies["OauthHMAC"],
      "ZTRlMzU5N2Q4ZDIwZWE5ZTU5NTg3YTU3YTcxZTU0NDFkMzY1ZTc1NjMyODYyMjRlNjMxZTJmNTZkYzRmZTM0ZQ====");
  EXPECT_EQ(cookies["OauthNonce"], TEST_CSRF_TOKEN);
}

/**
 * Testing oauth state with cookie domain.
 *
 * Expected behavior: Cookie domain should be set to the domain in the config.
 */
TEST_F(OAuth2Test, OAuthTestFullFlowPostWithCookieDomain) {
  init(getConfig(true, true,
                 ::envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
                     OAuth2Config_AuthType_URL_ENCODED_BODY,
                 0, false, false, true /* set_cookie_domain */));
  // First construct the initial request to the oauth filter with URI parameters.
  Http::TestRequestHeaderMapImpl first_request_headers{
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Post},
      {Http::Headers::get().Scheme.get(), "https"},
  };

  // This is the immediate response - a redirect to the auth cluster.
  Http::TestResponseHeaderMapImpl first_response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";domain=example.com;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "CodeVerifier=" + TEST_ENCRYPTED_CODE_VERIFIER +
           ";domain=example.com;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Location.get(),
       "https://auth.example.com/oauth/"
       "authorize/?client_id=" +
           TEST_CLIENT_ID + "&code_challenge=" + TEST_CODE_CHALLENGE +
           "&code_challenge_method=S256" +
           "&redirect_uri=https%3A%2F%2Ftraffic.example.com%2F_oauth"
           "&response_type=code"
           "&scope=" +
           TEST_ENCODED_AUTH_SCOPES + "&state=" + TEST_ENCODED_STATE + "&resource=oauth2-resource" +
           "&resource=http%3A%2F%2Fexample.com"
           "&resource=https%3A%2F%2Fexample.com%2Fsome%2Fpath%252F..%252F%2Futf8%C3%83%3Bfoo%3Dbar%"
           "3Fvar1%3D1%26var2%3D2"},
  };

  // Fail the validation to trigger the OAuth flow.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));
  EXPECT_CALL(*validator_, canUpdateTokenByRefreshToken()).WillOnce(Return(false));

  // Check that the redirect includes URL encoded query parameter characters.
  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&first_response_headers), true));

  // This represents the beginning of the OAuth filter.
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(first_request_headers, false));

  // This represents the callback request from the authorization server.
  Http::TestRequestHeaderMapImpl second_request_headers{
      {Http::Headers::get().Cookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";domain=example.com;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Cookie.get(),
       "CodeVerifier=" + TEST_ENCRYPTED_CODE_VERIFIER +
           ";domain=example.com;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Path.get(), "/_oauth?code=123&state=" + TEST_ENCODED_STATE},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
  };
  // Deliberately fail the HMAC validation check.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  EXPECT_CALL(*oauth_client_, asyncGetAccessToken("123", TEST_CLIENT_ID, "asdf_client_secret_fdsa",
                                                  "https://traffic.example.com" + TEST_CALLBACK,
                                                  TEST_CODE_VERIFIER, AuthType::UrlEncodedBody));

  // Invoke the callback logic. As a side effect, state_ will be populated.
  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndBuffer,
            filter_->decodeHeaders(second_request_headers, false));

  EXPECT_EQ(1, config_->stats().oauth_unauthorized_rq_.value());
  EXPECT_EQ(config_->clusterName(), "auth.example.com");

  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(0)));
  const std::chrono::seconds expiredTime(10);
  filter_->updateTokens("access_code", "some-id-token", "some-refresh-token", expiredTime);

  // Expected response after the callback & validation is complete - verifying we kept the
  // state and method of the original request, including the query string parameters.
  Http::TestRequestHeaderMapImpl second_response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthHMAC=seD1HFQMr2pDwXgZKYQ1+D8R/p8tCa2fO8xTmfAgAUg=;"
       "domain=example.com;path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "OauthExpires=10;domain=example.com;path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN +
           ";domain=example.com;path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + TEST_ENCRYPTED_ID_TOKEN +
           ";domain=example.com;path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN +
           ";domain=example.com;path=/;Max-Age=604800;secure;HttpOnly"},
      {Http::Headers::get().Location.get(),
       "https://traffic.example.com/original_path?var1=1&var2=2"},
  };

  EXPECT_CALL(decoder_callbacks_,
              encodeHeaders_(HeaderMapEqualRef(&second_response_headers), true));

  filter_->finishGetAccessTokenFlow();
}

/**
 * Testing oauth state with special characters that must be escaped in JSON.
 *
 * Expected behavior: the JSON string in the state query parameter should be correctly escaped and
 * the final redirect should equal the original request.
 */
TEST_F(OAuth2Test, OAuthTestFullFlowPostWithSpecialCharactersForJson) {
  init(getConfig(true /* forward_bearer_token */, true /* use_refresh_token */));
  const std::string url_with_special_characters =
      R"(/original_path?query="value"&key=val\ue#frag<ment>{data}[info]|test\^space)";
  const std::string test_encoded_state_with_special_characters =
      "eyJ1cmwiOiJodHRwczovL3RyYWZmaWMuZXhhbXBsZS5jb20vb3JpZ2luYWxfcGF0aD9xdWVyeT1cInZhbHVlXCIma2V5"
      "PXZhbFxcdWUjZnJhZzxtZW50PntkYXRhfVtpbmZvXXx0ZXN0XFxec3BhY2UiLCJjc3JmX3Rva2VuIjoiMDAwMDAwMDAw"
      "NzViY2QxNS5uYTZrcnU0eDFwSGdvY1NJZVUvbWR0SFluNThHaDFicXdlUzRYWG9pcVZnPSJ9";
  // First construct the initial request to the oauth filter with URI parameters.
  Http::TestRequestHeaderMapImpl first_request_headers{
      {Http::Headers::get().Path.get(), url_with_special_characters},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Post},
      {Http::Headers::get().Scheme.get(), "https"},
  };

  // This is the immediate response - a redirect to the auth cluster.
  Http::TestResponseHeaderMapImpl first_response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "CodeVerifier=" + TEST_ENCRYPTED_CODE_VERIFIER + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Location.get(),
       "https://auth.example.com/oauth/"
       "authorize/?client_id=" +
           TEST_CLIENT_ID + "&code_challenge=" + TEST_CODE_CHALLENGE +
           "&code_challenge_method=S256" +
           "&redirect_uri=https%3A%2F%2Ftraffic.example.com%2F_oauth"
           "&response_type=code"
           "&scope=" +
           TEST_ENCODED_AUTH_SCOPES + "&state=" + test_encoded_state_with_special_characters +
           "&resource=oauth2-resource" +
           "&resource=http%3A%2F%2Fexample.com"
           "&resource=https%3A%2F%2Fexample.com%2Fsome%2Fpath%252F..%252F%2Futf8%C3%83%3Bfoo%"
           "3Dbar%"
           "3Fvar1%3D1%26var2%3D2"},
  };

  // Fail the validation to trigger the OAuth flow.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));
  EXPECT_CALL(*validator_, canUpdateTokenByRefreshToken()).WillOnce(Return(false));

  // Check that the redirect includes URL encoded query parameter characters.
  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&first_response_headers), true));

  // This represents the beginning of the OAuth filter.
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(first_request_headers, false));

  // This represents the callback request from the authorization server.
  Http::TestRequestHeaderMapImpl second_request_headers{
      {Http::Headers::get().Cookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Cookie.get(),
       "CodeVerifier=" + TEST_ENCRYPTED_CODE_VERIFIER + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Path.get(),
       "/_oauth?code=123&state=" + test_encoded_state_with_special_characters},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
  };
  // Deliberately fail the HMAC validation check.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  EXPECT_CALL(*oauth_client_, asyncGetAccessToken("123", TEST_CLIENT_ID, "asdf_client_secret_fdsa",
                                                  "https://traffic.example.com" + TEST_CALLBACK,
                                                  TEST_CODE_VERIFIER, AuthType::UrlEncodedBody));

  // Invoke the callback logic. As a side effect, state_ will be populated.
  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndBuffer,
            filter_->decodeHeaders(second_request_headers, false));

  EXPECT_EQ(1, config_->stats().oauth_unauthorized_rq_.value());
  EXPECT_EQ(config_->clusterName(), "auth.example.com");

  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(0)));
  const std::chrono::seconds expiredTime(10);
  filter_->updateTokens("access_code", "some-id-token", "some-refresh-token", expiredTime);

  // Expected response after the callback & validation is complete - verifying we kept the
  // state and method of the original request, including the query string parameters.
  Http::TestRequestHeaderMapImpl second_response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(), "OauthHMAC="
                                             "UzbL/bzvWEP8oaoPDfQrD0zu6zC6m0yBOowKx1Mdr6o=;"
                                             "path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(), "OauthExpires=10;path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN + ";path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + TEST_ENCRYPTED_ID_TOKEN + ";path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN + ";path=/;Max-Age=604800;secure;HttpOnly"},
      {Http::Headers::get().Location.get(),
       "https://traffic.example.com" + url_with_special_characters},
  };

  EXPECT_CALL(decoder_callbacks_,
              encodeHeaders_(HeaderMapEqualRef(&second_response_headers), true));

  filter_->finishGetAccessTokenFlow();
}

class DisabledIdTokenTests : public OAuth2Test {
public:
  DisabledIdTokenTests() : OAuth2Test(false) {
    // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
    test_time_.setSystemTime(SystemTime(std::chrono::seconds(1000)));

    request_headers_ = {
        {Http::Headers::get().Host.get(), "traffic.example.com"},
        {Http::Headers::get().Path.get(), "/_oauth"},
        {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
    };

    // Note no IdToken cookie below.
    expected_headers_ = {
        {Http::Headers::get().Status.get(), "302"},
        {Http::Headers::get().SetCookie.get(),
         "OauthHMAC=" + hmac_without_id_token_ + ";path=/;Max-Age=600;secure;HttpOnly"},
        {Http::Headers::get().SetCookie.get(),
         "OauthExpires=1600;path=/;Max-Age=600;secure;HttpOnly"},
        {Http::Headers::get().SetCookie.get(),
         "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
        {Http::Headers::get().SetCookie.get(),
         "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
    };

    init(getConfig(true /* forward_bearer_token */, true /* use_refresh_token */,
                   ::envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
                       OAuth2Config_AuthType_URL_ENCODED_BODY /* encoded_body_type */,
                   600 /* default_refresh_token_expires_in */,
                   false /* preserve_authorization_header */,
                   true /* disable_id_token_set_cookie */));

    EXPECT_CALL(*validator_, token()).WillRepeatedly(ReturnRef(hmac_without_id_token_));
    EXPECT_CALL(*validator_, setParams(_, _));
    EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));
  }

  std::string hmac_without_id_token_{"kEbe8eYQkIkoHDQSzf1e38bSXNrgFCSEUWHZtEX6Q4c="};
  const std::string access_code_{"access_code"};
  const std::string id_token_{"some-id-token"};
  const std::string refresh_token_{"some-refresh-token"};
  const std::chrono::seconds expires_in_{600};
  Http::TestRequestHeaderMapImpl request_headers_;
  Http::TestResponseHeaderMapImpl expected_headers_;
};

// When disable_id_token_set_cookie is `true`, then during the access token flow the filter should
// *not* set the IdToken cookie in the 302 response and should produce an HMAC that does not
// consider the id-token.
TEST_F(DisabledIdTokenTests, SetCookieIgnoresIdTokenWhenDisabledAccessToken) {
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  expected_headers_.addCopy(Http::Headers::get().Location.get(), "");
  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&expected_headers_), true));

  // An ID token is still received from the IdP, but not set in the response headers above.
  filter_->onGetAccessTokenSuccess(access_code_, id_token_, refresh_token_, expires_in_);
}

// When disable_id_token_set_cookie is `true`, then during the refresh token flow the filter should
// *not* set the IdToken request header that's forwarded, the response headers that are returned,
// and should produce an HMAC that does not consider the id-token.
TEST_F(DisabledIdTokenTests, SetCookieIgnoresIdTokenWhenDisabledRefreshToken) {
  TestScopedRuntime scoped_runtime;
  scoped_runtime.mergeValues({{"envoy.reloadable_features.oauth2_cleanup_cookies", "false"}});
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  // An ID token is still received from the IdP, but not set in the request headers that are
  // forwarded.
  EXPECT_CALL(decoder_callbacks_, continueDecoding());
  filter_->onRefreshAccessTokenSuccess(access_code_, id_token_, refresh_token_, expires_in_);
  auto cookies = Http::Utility::parseCookies(request_headers_);
  const auto cookie_names = config_->cookieNames();
  EXPECT_EQ(cookies[cookie_names.oauth_hmac_], hmac_without_id_token_);
  EXPECT_EQ(cookies[cookie_names.oauth_expires_],
            "1600"); // Uses default_refresh_token_expires_in since not a legitimate JWT.
  EXPECT_EQ(cookies[cookie_names.bearer_token_], "access_code");
  EXPECT_EQ(cookies[cookie_names.refresh_token_], "some-refresh-token");
  EXPECT_EQ(cookies.contains(cookie_names.id_token_), false);

  // And ensure when the response comes back, it has the same cookies in the `expected_headers_`.
  Http::TestResponseHeaderMapImpl response_headers = {{Http::Headers::get().Status.get(), "302"}};
  filter_->encodeHeaders(response_headers, false);
  EXPECT_THAT(response_headers, HeaderMapEqualRef(&expected_headers_));
}

class DisabledTokenTests : public OAuth2Test {
public:
  DisabledTokenTests() : OAuth2Test(false) {
    // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
    test_time_.setSystemTime(SystemTime(std::chrono::seconds(1000)));

    request_headers_ = {
        {Http::Headers::get().Host.get(), "traffic.example.com"},
        {Http::Headers::get().Path.get(), "/_oauth"},
        {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
    };

    // Note no Token cookies below.
    expected_headers_ = {
        {Http::Headers::get().Status.get(), "302"},
        {Http::Headers::get().SetCookie.get(),
         "OauthHMAC=" + hmac_without_tokens_ + ";path=/;Max-Age=600;secure;HttpOnly"},
        {Http::Headers::get().SetCookie.get(),
         "OauthExpires=1600;path=/;Max-Age=600;secure;HttpOnly"},
    };

    init(getConfig(
        true /* forward_bearer_token */, true /* use_refresh_token */,
        ::envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
            OAuth2Config_AuthType_URL_ENCODED_BODY /* encoded_body_type */,
        600 /* default_refresh_token_expires_in */, false /* preserve_authorization_header */,
        true /* disable_id_token_set_cookie */, false /* set_cookie_domain */,
        true /* disable_access_token_set_cookie */, true /* disable_refresh_token_set_cookie */));

    EXPECT_CALL(*validator_, token()).WillRepeatedly(ReturnRef(hmac_without_tokens_));
    EXPECT_CALL(*validator_, setParams(_, _));
    EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));
  }

  std::string hmac_without_tokens_{"Crs4S83olTGsGL7jbxBWw37gvuv0P2WbOvGTr/F6Z0o="};
  const std::string access_code_{"access_code"};
  const std::string id_token_{"some-id-token"};
  const std::string refresh_token_{"some-refresh-token"};
  const std::chrono::seconds expires_in_{600};
  Http::TestRequestHeaderMapImpl request_headers_;
  Http::TestResponseHeaderMapImpl expected_headers_;
};

// When disable_id_token_set_cookie is `true`, then during the access token flow the filter should
// *not* set the IdToken cookie in the 302 response and should produce an HMAC that does not
// consider the id-token.
TEST_F(DisabledTokenTests, SetCookieIgnoresTokensWhenAllTokensAreDisabled1) {
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  expected_headers_.addCopy(Http::Headers::get().Location.get(), "");
  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&expected_headers_), true));

  // All Tokens are still received from the IdP, but not set in the response headers above.
  filter_->onGetAccessTokenSuccess(access_code_, id_token_, refresh_token_, expires_in_);
}

// When disable_id_token_set_cookie is `true`, then during the refresh token flow the filter should
// *not* set the IdToken request header that's forwarded, the response headers that are returned,
// and should produce an HMAC that does not consider the id-token.
TEST_F(DisabledTokenTests, SetCookieIgnoresTokensWhenAllTokensAreDisabled2) {
  TestScopedRuntime scoped_runtime;
  scoped_runtime.mergeValues({{"envoy.reloadable_features.oauth2_cleanup_cookies", "false"}});

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  // All tokens are still received from the IdP, but not set in the request headers that are
  // forwarded.
  EXPECT_CALL(decoder_callbacks_, continueDecoding());
  filter_->onRefreshAccessTokenSuccess(access_code_, id_token_, refresh_token_, expires_in_);
  auto cookies = Http::Utility::parseCookies(request_headers_);
  const auto cookie_names = config_->cookieNames();
  EXPECT_EQ(cookies[cookie_names.oauth_hmac_], hmac_without_tokens_);
  EXPECT_EQ(cookies[cookie_names.oauth_expires_],
            "1600"); // Uses default_refresh_token_expires_in since not a legitimate JWT.
  EXPECT_EQ(cookies.contains(cookie_names.bearer_token_), false);
  EXPECT_EQ(cookies.contains(cookie_names.refresh_token_), false);
  EXPECT_EQ(cookies.contains(cookie_names.id_token_), false);

  // And ensure when the response comes back, it has the same cookies in the `expected_headers_`.
  Http::TestResponseHeaderMapImpl response_headers = {{Http::Headers::get().Status.get(), "302"}};
  filter_->encodeHeaders(response_headers, false);
  EXPECT_THAT(response_headers, HeaderMapEqualRef(&expected_headers_));
}

/**
 * Testing oauth response after tokens are set.
 *
 * Expected behavior: cookies are set.
 */

std::string oauthHMAC;

TEST_F(OAuth2Test, OAuthAccessTokenSucessWithTokens) {
  oauthHMAC = "fueOhiagmqQRQSxerTj/KZ065YXYk5SOiLtEvm9qlyA=;";
  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(1000)));

  // host_ must be set, which is guaranteed (ASAN).
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/_signout"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };
  filter_->decodeHeaders(request_headers, false);

  // Expected response after the callback is complete.
  Http::TestRequestHeaderMapImpl expected_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthHMAC=" + oauthHMAC + "path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "OauthExpires=1600;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + TEST_ENCRYPTED_ID_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Location.get(), ""},
  };

  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&expected_headers), true));

  filter_->onGetAccessTokenSuccess("access_code", "some-id-token", "some-refresh-token",
                                   std::chrono::seconds(600));
}

TEST_F(OAuth2Test, OAuthAccessTokenSucessWithTokensUseRefreshToken) {
  init(getConfig(true /* forward_bearer_token */, true /* use_refresh_token */));
  oauthHMAC = "4TKyxPV/F7yyvr0XgJ2bkWFOc8t4IOFen1k29b84MAQ=;";
  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(1000)));

  // host_ must be set, which is guaranteed (ASAN).
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/_signout"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };
  filter_->decodeHeaders(request_headers, false);

  // Expected response after the callback is complete.
  Http::TestRequestHeaderMapImpl expected_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthHMAC=" + oauthHMAC + "path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "OauthExpires=1600;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + TEST_ENCRYPTED_ID_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN + ";path=/;Max-Age=604800;secure;HttpOnly"},
      {Http::Headers::get().Location.get(), ""},
  };

  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&expected_headers), true));

  filter_->onGetAccessTokenSuccess("access_code", "some-id-token", "some-refresh-token",
                                   std::chrono::seconds(600));
}

TEST_F(OAuth2Test, OAuthAccessTokenSucessWithTokensUseRefreshTokenAndDefaultRefreshTokenExpiresIn) {
  init(getConfig(true /* forward_bearer_token */, true /* use_refresh_token */,
                 ::envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
                     OAuth2Config_AuthType_URL_ENCODED_BODY /* encoded_body_type */,
                 1200 /* default_refresh_token_expires_in */));
  TestScopedRuntime scoped_runtime;
  oauthHMAC = "4TKyxPV/F7yyvr0XgJ2bkWFOc8t4IOFen1k29b84MAQ=;";
  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(1000)));

  // host_ must be set, which is guaranteed (ASAN).
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/_signout"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };
  filter_->decodeHeaders(request_headers, false);

  // Expected response after the callback is complete.
  Http::TestRequestHeaderMapImpl expected_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthHMAC=" + oauthHMAC + "path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "OauthExpires=1600;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + TEST_ENCRYPTED_ID_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN + ";path=/;Max-Age=1200;secure;HttpOnly"},
      {Http::Headers::get().Location.get(), ""},
  };

  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&expected_headers), true));

  filter_->onGetAccessTokenSuccess("access_code", "some-id-token", "some-refresh-token",
                                   std::chrono::seconds(600));
}

/**
 * Scenario: The Oauth filter saves cookies with tokens after successful receipt of the tokens.
 *
 * Expected behavior: The lifetime of the refresh token cookie is taken from the exp claim of the
 * refresh token.
 */

TEST_F(OAuth2Test, OAuthAccessTokenSucessWithTokensUseRefreshTokenAndRefreshTokenExpiresInFromJwt) {
  init(getConfig(true /* forward_bearer_token */, true /* use_refresh_token */,
                 ::envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
                     OAuth2Config_AuthType_URL_ENCODED_BODY /* encoded_body_type */,
                 1200 /* default_refresh_token_expires_in */));
  oauthHMAC = "CmrSZUsPEF1D4UgEnuz2d2s878YnAoOpxQCtE9LJ89M=;";
  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(1000)));

  // host_ must be set, which is guaranteed (ASAN).
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/_signout"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };
  filter_->decodeHeaders(request_headers, false);

  const std::string refreshToken =
      "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
      "eyJ1bmlxdWVfbmFtZSI6ImFsZXhjZWk4OCIsInN1YiI6ImFsZXhjZWk4OCIsImp0aSI6IjQ5ZTFjMzc1IiwiYXVkIjoi"
      "dGVzdCIsIm5iZiI6MTcwNzQxNDYzNSwiZXhwIjoyNTU0NDE2MDAwLCJpYXQiOjE3MDc0MTQ2MzYsImlzcyI6ImRvdG5l"
      "dC11c2VyLWp3dHMifQ.LaGOw6x0-m7r-WzxgCIdPnAfp0O1hy6mW4klq9Vs2XM";
  const std::string encrypted_refresh_token =
      "Fc1bBwAAAAAVzVsHAAAAANmnPnluIb9exn3WlbkgaDHNTVoZUE-1O8H_"
      "amXtsHZWG04QXuzJxsFxxe58HpCeWYx7QYi886mP3fCWDBrOJZ4DkwJjQXtvp9VdmKhCr1qCYQ9mSdv6GY50g-aOOr-"
      "x1wXNGCfnURYA48u2BulYuHqG2FzNAfbPo8uNO0IS3CUNE3C9gLcs4gHq9AjMwXVe3PLxV0ihrcXCUVp0ao9R2k2Ki1V"
      "LZpaH6ntay0IUJft2hjvq3lVvtCakEH0LYmzx9G0MGwaqiaeeFBNQyCY9iji5BOAfFezKnLKAvsYn2egVDHEFXCCSUW2"
      "3YEA57eGNDrs1PIZXRvLrjyJCiBE-0Iiq74MgHSG6usBK21wks8VOGyIy3qRkz-LcmgLX9ZB1lA";

  // Expected response after the callback is complete.
  Http::TestRequestHeaderMapImpl expected_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthHMAC=" + oauthHMAC + "path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "OauthExpires=1600;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + TEST_ENCRYPTED_ID_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=" + encrypted_refresh_token + ";path=/;Max-Age=2554415000;secure;HttpOnly"},
      {Http::Headers::get().Location.get(), ""},
  };

  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&expected_headers), true));

  filter_->onGetAccessTokenSuccess("access_code", "some-id-token", refreshToken,
                                   std::chrono::seconds(600));
}

/**
 * Scenario: The Oauth filter doesn't save cookie with refresh token because the token is expired.
 *
 * Expected behavior: The age of the cookie with refresh token is equal to zero.
 */

TEST_F(OAuth2Test, OAuthAccessTokenSucessWithTokensUseRefreshTokenAndExpiredRefreshToken) {
  init(getConfig(true /* forward_bearer_token */, true /* use_refresh_token */,
                 ::envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
                     OAuth2Config_AuthType_URL_ENCODED_BODY /* encoded_body_type */,
                 1200 /* default_refresh_token_expires_in */));
  TestScopedRuntime scoped_runtime;
  oauthHMAC = "73RuBwU3Kx/7RP4N1yy+8QnhARjA15QOoxdKD7zk1pI=;";
  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(2554515000)));

  // host_ must be set, which is guaranteed (ASAN).
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/_signout"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };
  filter_->decodeHeaders(request_headers, false);

  const std::string refreshToken =
      "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
      "eyJ1bmlxdWVfbmFtZSI6ImFsZXhjZWk4OCIsInN1YiI6ImFsZXhjZWk4OCIsImp0aSI6IjQ5ZTFjMzc1IiwiYXVkIjoi"
      "dGVzdCIsIm5iZiI6MTcwNzQxNDYzNSwiZXhwIjoyNTU0NDE2MDAwLCJpYXQiOjE3MDc0MTQ2MzYsImlzcyI6ImRvdG5l"
      "dC11c2VyLWp3dHMifQ.LaGOw6x0-m7r-WzxgCIdPnAfp0O1hy6mW4klq9Vs2XM";
  const std::string encrypted_refresh_token =
      "Fc1bBwAAAAAVzVsHAAAAANmnPnluIb9exn3WlbkgaDHNTVoZUE-1O8H_"
      "amXtsHZWG04QXuzJxsFxxe58HpCeWYx7QYi886mP3fCWDBrOJZ4DkwJjQXtvp9VdmKhCr1qCYQ9mSdv6GY50g-aOOr-"
      "x1wXNGCfnURYA48u2BulYuHqG2FzNAfbPo8uNO0IS3CUNE3C9gLcs4gHq9AjMwXVe3PLxV0ihrcXCUVp0ao9R2k2Ki1V"
      "LZpaH6ntay0IUJft2hjvq3lVvtCakEH0LYmzx9G0MGwaqiaeeFBNQyCY9iji5BOAfFezKnLKAvsYn2egVDHEFXCCSUW2"
      "3YEA57eGNDrs1PIZXRvLrjyJCiBE-0Iiq74MgHSG6usBK21wks8VOGyIy3qRkz-LcmgLX9ZB1lA";

  // Expected response after the callback is complete.
  Http::TestRequestHeaderMapImpl expected_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthHMAC=" + oauthHMAC + "path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "OauthExpires=2554515600;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + TEST_ENCRYPTED_ID_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=" + encrypted_refresh_token + ";path=/;Max-Age=0;secure;HttpOnly"},
      {Http::Headers::get().Location.get(), ""},
  };

  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&expected_headers), true));

  filter_->onGetAccessTokenSuccess("access_code", "some-id-token", refreshToken,
                                   std::chrono::seconds(600));
}

/**
 * Scenario: The Oauth filter receives the refresh token without exp claim.
 *
 * Expected behavior: The age of the cookie with refresh token is equal to default value.
 */

TEST_F(OAuth2Test, OAuthAccessTokenSucessWithTokensUseRefreshTokenAndNoExpClaimInRefreshToken) {
  init(getConfig(true /* forward_bearer_token */, true /* use_refresh_token */,
                 ::envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
                     OAuth2Config_AuthType_URL_ENCODED_BODY /* encoded_body_type */,
                 1200 /* default_refresh_token_expires_in */));
  TestScopedRuntime scoped_runtime;
  oauthHMAC = "euROdA+Ca4p/9JoMnX50fiqHormIWP/S+Fse+wD+V8I=;";
  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(1000)));

  // host_ must be set, which is guaranteed (ASAN).
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/_signout"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };
  filter_->decodeHeaders(request_headers, false);

  const std::string refreshToken =
      "eyJhbGciOiJIUzI1NiJ9."
      "eyJSb2xlIjoiQWRtaW4iLCJJc3N1ZXIiOiJJc3N1ZXIiLCJVc2VybmFtZSI6IkphdmFJblVzZSIsImlhdCI6MTcwODA2"
      "NDcyOH0.92H-X2Oa4ECNmFLZBWBHP0BJyEHDprLkEIc2JBJYwkI";
  const std::string encrypted_refresh_token =
      "Fc1bBwAAAAAVzVsHAAAAANmnPnluIb9exn3WlbkgaDE7Qej3gaQyBPqvzoNiSVn8-sv2lmZF7nT3OVnBe7X-KK-"
      "jOOVaiHesGNEsPt5F0CmkMytmf-t0VMASmnC8FhgnCsRkf2XHL_"
      "z18YGJTvbHgc6QDdKUDwGuMTL048BdQYelXZ9nwtNchSkbZIa8yUf5wrZtEvFpOzE-brHaI3LOWmHaQ27h_"
      "lm5eH0qKwMy_jXZMXhxzO_-Rrz9XBlVwIMP";

  // Expected response after the callback is complete.
  Http::TestRequestHeaderMapImpl expected_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthHMAC=" + oauthHMAC + "path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "OauthExpires=1600;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + TEST_ENCRYPTED_ID_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=" + encrypted_refresh_token + ";path=/;Max-Age=1200;secure;HttpOnly"},
      {Http::Headers::get().Location.get(), ""},
  };

  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&expected_headers), true));

  filter_->onGetAccessTokenSuccess("access_code", "some-id-token", refreshToken,
                                   std::chrono::seconds(600));
}

/**
 * Scenario: The Oauth filter saves cookies with tokens after successful receipt of the tokens.
 *
 * Expected behavior: The lifetime of the id token cookie is taken from the exp claim of the
 * id token.
 */

TEST_F(OAuth2Test, OAuthAccessTokenSucessWithTokensIdTokenExpiresInFromJwt) {
  init(getConfig(true /* forward_bearer_token */, true /* use_refresh_token */,
                 ::envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
                     OAuth2Config_AuthType_URL_ENCODED_BODY /* encoded_body_type */,
                 1200 /* default_refresh_token_expires_in */));
  TestScopedRuntime scoped_runtime;
  oauthHMAC = "MqrMKGLbdIEogLWZPRffaVTXDGRRveG3gn9bZu5Gd4Q=;";
  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(1000)));

  // host_ must be set, which is guaranteed (ASAN).
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/_signout"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };
  filter_->decodeHeaders(request_headers, false);

  const std::string id_token =
      "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
      "eyJ1bmlxdWVfbmFtZSI6ImFsZXhjZWk4OCIsInN1YiI6ImFsZXhjZWk4OCIsImp0aSI6IjQ5ZTFjMzc1IiwiYXVkIjoi"
      "dGVzdCIsIm5iZiI6MTcwNzQxNDYzNSwiZXhwIjoyNTU0NDE2MDAwLCJpYXQiOjE3MDc0MTQ2MzYsImlzcyI6ImRvdG5l"
      "dC11c2VyLWp3dHMifQ.LaGOw6x0-m7r-WzxgCIdPnAfp0O1hy6mW4klq9Vs2XM";
  const std::string encrypted_id_token =
      "Fc1bBwAAAAAVzVsHAAAAANmnPnluIb9exn3WlbkgaDHNTVoZUE-1O8H_"
      "amXtsHZWG04QXuzJxsFxxe58HpCeWYx7QYi886mP3fCWDBrOJZ4DkwJjQXtvp9VdmKhCr1qCYQ9mSdv6GY50g-aOOr-"
      "x1wXNGCfnURYA48u2BulYuHqG2FzNAfbPo8uNO0IS3CUNE3C9gLcs4gHq9AjMwXVe3PLxV0ihrcXCUVp0ao9R2k2Ki1V"
      "LZpaH6ntay0IUJft2hjvq3lVvtCakEH0LYmzx9G0MGwaqiaeeFBNQyCY9iji5BOAfFezKnLKAvsYn2egVDHEFXCCSUW2"
      "3YEA57eGNDrs1PIZXRvLrjyJCiBE-0Iiq74MgHSG6usBK21wks8VOGyIy3qRkz-LcmgLX9ZB1lA";

  // Expected response after the callback is complete.
  Http::TestRequestHeaderMapImpl expected_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthHMAC=" + oauthHMAC + "path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "OauthExpires=1600;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + encrypted_id_token + ";path=/;Max-Age=2554415000;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN + ";path=/;Max-Age=1200;secure;HttpOnly"},
      {Http::Headers::get().Location.get(), ""},
  };

  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&expected_headers), true));

  filter_->onGetAccessTokenSuccess("access_code", id_token, "some-refresh-token",
                                   std::chrono::seconds(600));
}

/**
 * Scenario: The Oauth filter doesn't save cookie with id token because the token is expired.
 *
 * Expected behavior: The age of the cookie with the id token is equal to zero.
 */

TEST_F(OAuth2Test, OAuthAccessTokenSucessWithTokensExpiredIdToken) {
  init(getConfig(true /* forward_bearer_token */, true /* use_refresh_token */,
                 ::envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
                     OAuth2Config_AuthType_URL_ENCODED_BODY /* encoded_body_type */,
                 1200 /* default_refresh_token_expires_in */));
  TestScopedRuntime scoped_runtime;
  oauthHMAC = "eQmiVNw3uAZixmzqtd75kD/0MeSJzS/ROl99NNfWoyU=;";
  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(2554515000)));

  // host_ must be set, which is guaranteed (ASAN).
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/_signout"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };
  filter_->decodeHeaders(request_headers, false);

  const std::string id_token =
      "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
      "eyJ1bmlxdWVfbmFtZSI6ImFsZXhjZWk4OCIsInN1YiI6ImFsZXhjZWk4OCIsImp0aSI6IjQ5ZTFjMzc1IiwiYXVkIjoi"
      "dGVzdCIsIm5iZiI6MTcwNzQxNDYzNSwiZXhwIjoyNTU0NDE2MDAwLCJpYXQiOjE3MDc0MTQ2MzYsImlzcyI6ImRvdG5l"
      "dC11c2VyLWp3dHMifQ.LaGOw6x0-m7r-WzxgCIdPnAfp0O1hy6mW4klq9Vs2XM";
  const std::string encrypted_id_token =
      "Fc1bBwAAAAAVzVsHAAAAANmnPnluIb9exn3WlbkgaDHNTVoZUE-1O8H_"
      "amXtsHZWG04QXuzJxsFxxe58HpCeWYx7QYi886mP3fCWDBrOJZ4DkwJjQXtvp9VdmKhCr1qCYQ9mSdv6GY50g-aOOr-"
      "x1wXNGCfnURYA48u2BulYuHqG2FzNAfbPo8uNO0IS3CUNE3C9gLcs4gHq9AjMwXVe3PLxV0ihrcXCUVp0ao9R2k2Ki1V"
      "LZpaH6ntay0IUJft2hjvq3lVvtCakEH0LYmzx9G0MGwaqiaeeFBNQyCY9iji5BOAfFezKnLKAvsYn2egVDHEFXCCSUW2"
      "3YEA57eGNDrs1PIZXRvLrjyJCiBE-0Iiq74MgHSG6usBK21wks8VOGyIy3qRkz-LcmgLX9ZB1lA";

  // Expected response after the callback is complete.
  Http::TestRequestHeaderMapImpl expected_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthHMAC=" + oauthHMAC + "path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "OauthExpires=2554515600;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + encrypted_id_token + ";path=/;Max-Age=0;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN + ";path=/;Max-Age=1200;secure;HttpOnly"},
      {Http::Headers::get().Location.get(), ""},
  };

  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&expected_headers), true));

  filter_->onGetAccessTokenSuccess("access_code", id_token, "some-refresh-token",
                                   std::chrono::seconds(600));
}

/**
 * Scenario: The Oauth filter receives the id token without exp claim.
 *           This should never happen as the id token is a JWT with required exp claim per OpenID
 * Connect 1.0 specification.
 *
 * Expected behavior: The age of the cookie with id token is equal to the access token expiry.
 */

TEST_F(OAuth2Test, OAuthAccessTokenSucessWithTokensNoExpClaimInIdToken) {
  init(getConfig(true /* forward_bearer_token */, true /* use_refresh_token */,
                 ::envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
                     OAuth2Config_AuthType_URL_ENCODED_BODY /* encoded_body_type */,
                 1200 /* default_refresh_token_expires_in */));
  TestScopedRuntime scoped_runtime;
  oauthHMAC = "CU0eIzpTJSD/LFOVPaH7ypOQqqBvh4s6Tin3ip9rajk=;";
  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(1000)));

  // host_ must be set, which is guaranteed (ASAN).
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/_signout"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };
  filter_->decodeHeaders(request_headers, false);

  const std::string id_token =
      "eyJhbGciOiJIUzI1NiJ9."
      "eyJSb2xlIjoiQWRtaW4iLCJJc3N1ZXIiOiJJc3N1ZXIiLCJVc2VybmFtZSI6IkphdmFJblVzZSIsImlhdCI6MTcwODA2"
      "NDcyOH0.92H-X2Oa4ECNmFLZBWBHP0BJyEHDprLkEIc2JBJYwkI";
  const std::string encrypted_id_token =
      "Fc1bBwAAAAAVzVsHAAAAANmnPnluIb9exn3WlbkgaDE7Qej3gaQyBPqvzoNiSVn8-sv2lmZF7nT3OVnBe7X-KK-"
      "jOOVaiHesGNEsPt5F0CmkMytmf-t0VMASmnC8FhgnCsRkf2XHL_"
      "z18YGJTvbHgc6QDdKUDwGuMTL048BdQYelXZ9nwtNchSkbZIa8yUf5wrZtEvFpOzE-brHaI3LOWmHaQ27h_"
      "lm5eH0qKwMy_jXZMXhxzO_-Rrz9XBlVwIMP";

  // Expected response after the callback is complete.
  Http::TestRequestHeaderMapImpl expected_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthHMAC=" + oauthHMAC + "path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "OauthExpires=1600;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + encrypted_id_token + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN + ";path=/;Max-Age=1200;secure;HttpOnly"},
      {Http::Headers::get().Location.get(), ""},
  };

  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&expected_headers), true));

  filter_->onGetAccessTokenSuccess("access_code", id_token, "some-refresh-token",
                                   std::chrono::seconds(600));
}

TEST_F(OAuth2Test, OAuthBearerTokenFlowFromHeader) {
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/test?role=bearer"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::CustomHeaders::get().Authorization.get(), "Bearer xyz-header-token"},
  };

  // Fail the validation.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
}

TEST_F(OAuth2Test, OAuthBearerTokenFlowFromQueryParameters) {
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/test?role=bearer&token=xyz-queryparam-token"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
  };

  // Fail the validation.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
}

TEST_F(OAuth2Test, CookieValidatorInTransition) {
  Http::TestRequestHeaderMapImpl request_headers_base64only{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/_signout"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Cookie.get(), "OauthExpires=1600"},
      {Http::Headers::get().Cookie.get(), "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN},
      {Http::Headers::get().Cookie.get(), "IdToken=" + TEST_ENCRYPTED_ID_TOKEN},
      {Http::Headers::get().Cookie.get(), "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN},
      {Http::Headers::get().Cookie.get(), "OauthHMAC=eK7Kw2VqlnZJiz93KTnZqUar3ajNAe+ubmosGFkyL4I="},
  };

  auto cookie_validator = std::make_shared<OAuth2CookieValidator>(
      test_time_,
      CookieNames{"BearerToken", "OauthHMAC", "OauthExpires", "IdToken", "RefreshToken",
                  "OauthNonce", "CodeVerifier"},
      "");
  cookie_validator->setParams(request_headers_base64only, "mock-secret");
  EXPECT_TRUE(cookie_validator->hmacIsValid());

  Http::TestRequestHeaderMapImpl request_headers_hexbase64{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/_signout"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Cookie.get(), "OauthExpires=1600"},
      {Http::Headers::get().Cookie.get(), "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN},
      {Http::Headers::get().Cookie.get(), "IdToken=" + TEST_ENCRYPTED_ID_TOKEN},
      {Http::Headers::get().Cookie.get(), "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN},
      {Http::Headers::get().Cookie.get(), "OauthHMAC=eK7Kw2VqlnZJiz93KTnZqUar3ajNAe+ubmosGFkyL4I="},
  };
  cookie_validator->setParams(request_headers_hexbase64, "mock-secret");

  EXPECT_TRUE(cookie_validator->hmacIsValid());
}

// - The filter receives the initial request
// - The filter redirects a user to the authorization endpoint
// - The filter receives the callback request from the authorization endpoint
// - The filter gets a bearer and refresh tokens from the authorization endpoint
// - The filter redirects a user to the user agent with actual authorization data
// - The filter receives an other request when a bearer token is expired
// - The filter tries to update a bearer token via the refresh token instead of redirect user to the
// authorization endpoint
// - The filter gets a new bearer and refresh tokens via the current refresh token
// - The filter continues to handler the request without redirection to the user agent
TEST_F(OAuth2Test, OAuthTestFullFlowWithUseRefreshToken) {
  init(getConfig(true /* forward_bearer_token */, true /* use_refresh_token */));
  // First construct the initial request to the oauth filter with URI parameters.
  Http::TestRequestHeaderMapImpl first_request_headers{
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Post},
      {Http::Headers::get().Scheme.get(), "https"},
  };

  // This is the immediate response - a redirect to the auth cluster.
  Http::TestResponseHeaderMapImpl first_response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "CodeVerifier=" + TEST_ENCRYPTED_CODE_VERIFIER + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Location.get(),
       "https://auth.example.com/oauth/"
       "authorize/?client_id=" +
           TEST_CLIENT_ID + "&code_challenge=" + TEST_CODE_CHALLENGE +
           "&code_challenge_method=S256" +
           "&redirect_uri=https%3A%2F%2Ftraffic.example.com%2F_oauth"
           "&response_type=code"
           "&scope=" +
           TEST_ENCODED_AUTH_SCOPES + "&state=" + TEST_ENCODED_STATE + "&resource=oauth2-resource" +
           "&resource=http%3A%2F%2Fexample.com"
           "&resource=https%3A%2F%2Fexample.com%2Fsome%2Fpath%252F..%252F%2Futf8%C3%83%3Bfoo%3Dbar%"
           "3Fvar1%3D1%26var2%3D2"},
  };

  // Fail the validation to trigger the OAuth flow.

  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));
  EXPECT_CALL(*validator_, canUpdateTokenByRefreshToken()).WillOnce(Return(false));

  // Check that the redirect includes the escaped parameter characters, '?', '&' and '='.
  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&first_response_headers), true));

  // This represents the beginning of the OAuth filter.
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(first_request_headers, false));

  // This represents the callback request from the authorization server.
  Http::TestRequestHeaderMapImpl second_request_headers{
      {Http::Headers::get().Cookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";domain=example.com;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Cookie.get(),
       "CodeVerifier=" + TEST_ENCRYPTED_CODE_VERIFIER +
           ";domain=example.com;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Path.get(), "/_oauth?code=123&state=" + TEST_ENCODED_STATE},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Scheme.get(), "https"},
  };

  // Deliberately fail the HMAC validation check.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  EXPECT_CALL(*oauth_client_, asyncGetAccessToken("123", TEST_CLIENT_ID, "asdf_client_secret_fdsa",
                                                  "https://traffic.example.com" + TEST_CALLBACK,
                                                  TEST_CODE_VERIFIER, AuthType::UrlEncodedBody));

  // Invoke the callback logic. As a side effect, state_ will be populated.
  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndBuffer,
            filter_->decodeHeaders(second_request_headers, false));

  EXPECT_EQ(1, config_->stats().oauth_unauthorized_rq_.value());
  EXPECT_EQ(config_->clusterName(), "auth.example.com");

  // Expected response after the callback & validation is complete - verifying we kept the
  // state and method of the original request, including the query string parameters.
  Http::TestRequestHeaderMapImpl second_response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(), "OauthHMAC="
                                             "fV62OgLipChTQQC3UFgDp+l5sCiSb3zt7nCoJiVivWw=;"
                                             "path=/;Max-Age=;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(), "OauthExpires=;path=/;Max-Age=;secure;HttpOnly"},
      {Http::Headers::get().Location.get(),
       "https://traffic.example.com/original_path?var1=1&var2=2"},
  };

  EXPECT_CALL(decoder_callbacks_,
              encodeHeaders_(HeaderMapEqualRef(&second_response_headers), true));

  filter_->finishGetAccessTokenFlow();

  // the third request to the oauth filter with URI parameters.
  Http::TestRequestHeaderMapImpl third_request_headers{
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Post},
      {Http::Headers::get().Scheme.get(), "https"},
  };

  std::string legit_refresh_token{"legit_refresh_token"};
  EXPECT_CALL(*validator_, refreshToken()).WillRepeatedly(ReturnRef(legit_refresh_token));

  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));
  EXPECT_CALL(*validator_, canUpdateTokenByRefreshToken()).WillOnce(Return(true));

  EXPECT_CALL(*oauth_client_,
              asyncRefreshAccessToken(legit_refresh_token, TEST_CLIENT_ID,
                                      "asdf_client_secret_fdsa", AuthType::UrlEncodedBody));

  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(third_request_headers, false));

  EXPECT_CALL(decoder_callbacks_, continueDecoding());

  filter_->finishRefreshAccessTokenFlow();
  EXPECT_EQ(1, config_->stats().oauth_refreshtoken_success_.value());
  EXPECT_EQ(2, config_->stats().oauth_success_.value());
}

TEST_F(OAuth2Test, OAuthTestRefreshAccessTokenSuccess) {
  init(getConfig(true /* forward_bearer_token */, true /* use_refresh_token */));
  // First construct the initial request to the oauth filter with URI parameters.
  Http::TestRequestHeaderMapImpl first_request_headers{
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Post},
      {Http::Headers::get().Scheme.get(), "https"},
  };

  std::string legit_token{"legit_token"};
  EXPECT_CALL(*validator_, token()).WillRepeatedly(ReturnRef(legit_token));

  std::string legit_refresh_token{"legit_refresh_token"};
  EXPECT_CALL(*validator_, refreshToken()).WillRepeatedly(ReturnRef(legit_refresh_token));

  // Fail the validation to trigger the OAuth flow with trying to get the access token using by
  // refresh token.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));
  EXPECT_CALL(*validator_, canUpdateTokenByRefreshToken()).WillOnce(Return(true));

  EXPECT_CALL(*oauth_client_,
              asyncRefreshAccessToken(legit_refresh_token, TEST_CLIENT_ID,
                                      "asdf_client_secret_fdsa", AuthType::UrlEncodedBody));

  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(first_request_headers, false));

  Http::TestResponseHeaderMapImpl redirect_response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().Location.get(),
       "https://auth.example.com/oauth/"
       "authorize/?client_id=" +
           TEST_CLIENT_ID +
           "&redirect_uri=https%3A%2F%2Ftraffic.example.com%2F_oauth"
           "&response_type=code"
           "&scope=" +
           TEST_ENCODED_AUTH_SCOPES + "&state=" + TEST_ENCODED_STATE +
           "resource=oauth2-resource&resource=http%3A%2F%2Fexample.com"
           "&resource=https%3A%2F%2Fexample.com"},
  };

  // Check that the redirect includes the escaped parameter characters, '?', '&' and '='.
  EXPECT_CALL(decoder_callbacks_, continueDecoding());

  filter_->onRefreshAccessTokenSuccess("", "", "", std::chrono::seconds(10));

  EXPECT_EQ(1, config_->stats().oauth_refreshtoken_success_.value());
  EXPECT_EQ(1, config_->stats().oauth_success_.value());
}

TEST_F(OAuth2Test, OAuthTestRefreshAccessTokenFail) {
  init(getConfig(true /* forward_bearer_token */, true /* use_refresh_token */));
  // First construct the initial request to the oauth filter with URI parameters.
  Http::TestRequestHeaderMapImpl first_request_headers{
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Post},
      {Http::Headers::get().Scheme.get(), "https"},
  };

  std::string legit_token{"legit_token"};
  EXPECT_CALL(*validator_, token()).WillRepeatedly(ReturnRef(legit_token));

  std::string legit_refresh_token{"legit_refresh_token"};
  EXPECT_CALL(*validator_, refreshToken()).WillRepeatedly(ReturnRef(legit_refresh_token));

  // Fail the validation to trigger the OAuth flow with trying to get the access token using by
  // refresh token.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));
  EXPECT_CALL(*validator_, canUpdateTokenByRefreshToken()).WillOnce(Return(true));

  EXPECT_CALL(*oauth_client_,
              asyncRefreshAccessToken(legit_refresh_token, TEST_CLIENT_ID,
                                      "asdf_client_secret_fdsa", AuthType::UrlEncodedBody));

  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(first_request_headers, false));

  Http::TestResponseHeaderMapImpl redirect_response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "CodeVerifier=" + TEST_ENCRYPTED_CODE_VERIFIER + ";path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().Location.get(),
       "https://auth.example.com/oauth/"
       "authorize/?client_id=" +
           TEST_CLIENT_ID + "&code_challenge=" + TEST_CODE_CHALLENGE +
           "&code_challenge_method=S256" +
           "&redirect_uri=https%3A%2F%2Ftraffic.example.com%2F_oauth"
           "&response_type=code"
           "&scope=" +
           TEST_ENCODED_AUTH_SCOPES + "&state=" + TEST_ENCODED_STATE + "&resource=oauth2-resource" +
           "&resource=http%3A%2F%2Fexample.com"
           "&resource=https%3A%2F%2Fexample.com%2Fsome%2Fpath%252F..%252F%2Futf8%C3%83%3Bfoo%3Dbar%"
           "3Fvar1%3D1%26var2%3D2"},
  };

  // Check that the redirect includes the escaped parameter characters, '?', '&' and '='.
  EXPECT_CALL(decoder_callbacks_,
              encodeHeaders_(HeaderMapEqualRef(&redirect_response_headers), true));

  filter_->onRefreshAccessTokenFailure();

  EXPECT_EQ(1, config_->stats().oauth_unauthorized_rq_.value());
  EXPECT_EQ(1, config_->stats().oauth_refreshtoken_failure_.value());
}

/**
 * Scenario: The OAuth filter refresh flow fails for a request that matches the
 * deny_redirect_matcher.
 *
 * Expected behavior: the filter should should return 401 Unauthorized response.
 */
TEST_F(OAuth2Test, AjaxRefreshDoesNotRedirect) {
  init(getConfig(true /* forward_bearer_token */, true /* use_refresh_token */));
  // First construct the initial request to the oauth filter with URI parameters.
  Http::TestRequestHeaderMapImpl first_request_headers{
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Post},
      {Http::Headers::get().Scheme.get(), "https"},
      {"X-Requested-With", "XMLHttpRequest"},
  };

  std::string legit_token{"legit_token"};
  EXPECT_CALL(*validator_, token()).WillRepeatedly(ReturnRef(legit_token));

  std::string legit_refresh_token{"legit_refresh_token"};
  EXPECT_CALL(*validator_, refreshToken()).WillRepeatedly(ReturnRef(legit_refresh_token));

  // Fail the validation to trigger the OAuth flow with trying to get the access token using by
  // refresh token.
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));
  EXPECT_CALL(*validator_, canUpdateTokenByRefreshToken()).WillOnce(Return(true));

  EXPECT_CALL(*oauth_client_,
              asyncRefreshAccessToken(legit_refresh_token, TEST_CLIENT_ID,
                                      "asdf_client_secret_fdsa", AuthType::UrlEncodedBody));

  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(first_request_headers, false));

  // Unauthorized response is expected instead of 302 redirect.
  EXPECT_CALL(decoder_callbacks_, sendLocalReply(Http::Code::Unauthorized, _, _, _, _));

  filter_->onRefreshAccessTokenFailure();

  EXPECT_EQ(0, config_->stats().oauth_unauthorized_rq_.value());
  EXPECT_EQ(1, config_->stats().oauth_refreshtoken_failure_.value());
  EXPECT_EQ(1, config_->stats().oauth_failure_.value());
}

TEST_F(OAuth2Test, OAuthTestSetCookiesAfterRefreshAccessToken) {
  init(getConfig(true /* forward_bearer_token */, true /* use_refresh_token */));

  const auto expires_at_s = DateUtil::nowToSeconds(test_time_.timeSystem()) - 10;

  // the third request to the oauth filter with URI parameters.
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Post},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::Headers::get().Cookie.get(), fmt::format("OauthExpires={}", expires_at_s)},
      {Http::Headers::get().Cookie.get(), "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN},
      {Http::Headers::get().Cookie.get(), "OauthHMAC=dCu0otMcLoaGF73jrT+R8rGA0pnWyMgNf4+GivGrHEI="},
  };

  std::string legit_refresh_token{"legit_refresh_token"};
  EXPECT_CALL(*validator_, refreshToken()).WillRepeatedly(ReturnRef(legit_refresh_token));

  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));
  EXPECT_CALL(*validator_, canUpdateTokenByRefreshToken()).WillOnce(Return(true));

  EXPECT_CALL(*oauth_client_,
              asyncRefreshAccessToken(legit_refresh_token, TEST_CLIENT_ID,
                                      "asdf_client_secret_fdsa", AuthType::UrlEncodedBody));

  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers, false));

  EXPECT_CALL(decoder_callbacks_, continueDecoding());

  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(0)));
  const std::chrono::seconds expiredTime(10);
  filter_->updateTokens("access_code", "some-id-token", "some-refresh-token", expiredTime);

  filter_->finishRefreshAccessTokenFlow();

  Http::TestResponseHeaderMapImpl response_headers{};

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));

  Http::TestResponseHeaderMapImpl expected_response_headers{
      {Http::Headers::get().SetCookie.get(), "OauthHMAC="
                                             "UzbL/bzvWEP8oaoPDfQrD0zu6zC6m0yBOowKx1Mdr6o=;"
                                             "path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(), "OauthExpires=10;path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN + ";path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + TEST_ENCRYPTED_ID_TOKEN + ";path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN + ";path=/;Max-Age=604800;secure;HttpOnly"},
  };

  EXPECT_THAT(response_headers, HeaderMapEqualRef(&expected_response_headers));

  auto cookies = Http::Utility::parseCookies(request_headers);
  EXPECT_EQ(cookies.at("BearerToken"), "access_code");
  EXPECT_EQ(cookies.at("IdToken"), "some-id-token");

  // OAuth flow cookies should be removed before forwarding the request
  EXPECT_EQ(cookies.contains("OauthHMAC"), false);
  EXPECT_EQ(cookies.contains("OauthExpires"), false);
  EXPECT_EQ(cookies.contains("RefreshToken"), false);
  EXPECT_EQ(cookies.contains("OauthNonce"), false);
  EXPECT_EQ(cookies.contains("CodeVerifier"), false);
}

// When a refresh flow succeeds, but a new refresh token isn't received from the OAuth server, the
// previously received refresh token should be set in the response cookies.
TEST_F(OAuth2Test, OAuthTestSetCookiesAfterRefreshAccessTokenNoNewRefreshToken) {
  init(getConfig(true /* forward_bearer_token */, true /* use_refresh_token */));

  const auto expires_at_s = DateUtil::nowToSeconds(test_time_.timeSystem()) - 10;

  std::string legit_refresh_token = "legit_refresh_token";
  std::string encrypted_refresh_token =
      "Fc1bBwAAAAAVzVsHAAAAAOh8bHz59OyZPtKMgiX5FWJMyTXqsPjbf1j-Ao8fn1tb";
  // the third request to the oauth filter with URI parameters.
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Post},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::Headers::get().Cookie.get(), fmt::format("OauthExpires={}", expires_at_s)},
      {Http::Headers::get().Cookie.get(), fmt::format("RefreshToken={}", encrypted_refresh_token)},
      {Http::Headers::get().Cookie.get(), "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN},
      {Http::Headers::get().Cookie.get(), "OauthHMAC=dCu0otMcLoaGF73jrT+R8rGA0pnWyMgNf4+GivGrHEI="},
  };

  EXPECT_CALL(*validator_, refreshToken()).WillRepeatedly(ReturnRef(legit_refresh_token));

  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));
  EXPECT_CALL(*validator_, canUpdateTokenByRefreshToken()).WillOnce(Return(true));

  EXPECT_CALL(*oauth_client_,
              asyncRefreshAccessToken(legit_refresh_token, TEST_CLIENT_ID,
                                      "asdf_client_secret_fdsa", AuthType::UrlEncodedBody));

  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers, false));

  EXPECT_CALL(decoder_callbacks_, continueDecoding());

  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(0)));
  const std::chrono::seconds expiredTime(10);
  filter_->updateTokens("access_code", "some-id-token", "", expiredTime);

  filter_->finishRefreshAccessTokenFlow();

  Http::TestResponseHeaderMapImpl response_headers{};

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));

  Http::TestResponseHeaderMapImpl expected_response_headers{
      {Http::Headers::get().SetCookie.get(), "OauthHMAC="
                                             "xQCNvPMLwq3rF1dB/mSwyVz7kcIZai8pD8rS5SNLgRU=;"
                                             "path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(), "OauthExpires=10;path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN + ";path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + TEST_ENCRYPTED_ID_TOKEN + ";path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       fmt::format("RefreshToken={};path=/;Max-Age=604800;secure;HttpOnly",
                   encrypted_refresh_token)},
  };

  EXPECT_THAT(response_headers, HeaderMapEqualRef(&expected_response_headers));

  auto cookies = Http::Utility::parseCookies(request_headers);
  EXPECT_EQ(cookies.at("BearerToken"), "access_code");
  EXPECT_EQ(cookies.at("IdToken"), "some-id-token");

  // OAuth flow cookies should be removed before forwarding the request
  EXPECT_EQ(cookies.contains("OauthHMAC"), false);
  EXPECT_EQ(cookies.contains("OauthExpires"), false);
  EXPECT_EQ(cookies.contains("RefreshToken"), false);
  EXPECT_EQ(cookies.contains("OauthNonce"), false);
  EXPECT_EQ(cookies.contains("CodeVerifier"), false);
}

TEST_F(OAuth2Test, OAuthTestSetCookiesAfterRefreshAccessTokenWithBasicAuth) {
  init(getConfig(true /* forward_bearer_token */, true /* use_refresh_token */,
                 ::envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
                     OAuth2Config_AuthType_BASIC_AUTH
                 /* authType */));

  // 1. Test sending a request with expired tokens.
  // Set the expiration time to 10 seconds in the past to simulate token expiration.
  const auto expires_at_s = DateUtil::nowToSeconds(test_time_.timeSystem()) - 10;

  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Post},
      {Http::Headers::get().Scheme.get(), "https"},
      {Http::Headers::get().Cookie.get(), fmt::format("OauthExpires={}", expires_at_s)},
      {Http::Headers::get().Cookie.get(), "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN},
      {Http::Headers::get().Cookie.get(), "OauthHMAC=dCu0otMcLoaGF73jrT+R8rGA0pnWyMgNf4+GivGrHEI="},
      {Http::Headers::get().Cookie.get(), "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN},
  };

  std::string legit_refresh_token{"some-refresh-token"};
  EXPECT_CALL(*validator_, refreshToken()).WillRepeatedly(ReturnRef(legit_refresh_token));

  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));
  EXPECT_CALL(*validator_, canUpdateTokenByRefreshToken()).WillOnce(Return(true));

  // Filter should refresh the tokens using the refresh token because the tokens are expired and a
  // refresh token is available.
  EXPECT_CALL(*oauth_client_,
              asyncRefreshAccessToken(legit_refresh_token, TEST_CLIENT_ID,
                                      "asdf_client_secret_fdsa", AuthType::BasicAuth));

  // Filter should stop iteration because the tokens are expired.
  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers, false));

  // 2. Test refresh flow succeeds.
  // The new tokens received from the refresh flow.
  const std::string access_token = "accessToken";
  const std::string id_token = "idToken";
  const std::string refresh_token = "refreshToken";
  const std::string encrypted_id_token = "Fc1bBwAAAAAVzVsHAAAAAPD4z8oLeVyvkfTcl_cw198";
  const std::string encrypted_access_token = "Fc1bBwAAAAAVzVsHAAAAAGUINzc06x19yQYjN4Kb-YA";
  const std::string encrypted_refresh_token = "Fc1bBwAAAAAVzVsHAAAAACWUO4LpH2VJBN_6jSUWDPg";

  // Filter should continue decoding because the tokens are refreshed.
  EXPECT_CALL(decoder_callbacks_, continueDecoding());

  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(0)));
  const std::chrono::seconds expiredTime(10);
  filter_->updateTokens(access_token, id_token, refresh_token, expiredTime);

  filter_->finishRefreshAccessTokenFlow();

  Http::TestResponseHeaderMapImpl response_headers{};

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));

  Http::TestResponseHeaderMapImpl expected_response_headers{
      {Http::Headers::get().SetCookie.get(), "OauthHMAC="
                                             "OYnODPsSGabEpZ2LAiPxyjAFgN/7/5Xg24G7jUoUbyI=;"
                                             "path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(), "OauthExpires=10;path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + encrypted_access_token + ";path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + encrypted_id_token + ";path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=" + encrypted_refresh_token + ";path=/;Max-Age=604800;secure;HttpOnly"},
  };

  // Test the response headers are set correctly with the new tokens.
  EXPECT_THAT(response_headers, HeaderMapEqualRef(&expected_response_headers));

  // Test the request headers are updated with the new tokens.
  auto cookies = Http::Utility::parseCookies(request_headers);
  EXPECT_EQ(cookies.at("BearerToken"), "accessToken");
  EXPECT_EQ(cookies.at("IdToken"), "idToken");

  // OAuth flow cookies should be removed before forwarding the request
  EXPECT_EQ(cookies.contains("OauthHMAC"), false);
  EXPECT_EQ(cookies.contains("OauthExpires"), false);
  EXPECT_EQ(cookies.contains("RefreshToken"), false);
  EXPECT_EQ(cookies.contains("OauthNonce"), false);
  EXPECT_EQ(cookies.contains("CodeVerifier"), false);
}

// Test all cookies with STRICT SameSite
TEST_F(OAuth2Test, AllCookiesStrictSameSite) {
  using SameSite = envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite;
  init(getConfig(true, true,
                 envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
                     OAuth2Config_AuthType_URL_ENCODED_BODY,
                 0, false, false, false, false, false, SameSite::CookieConfig_SameSite_STRICT,
                 SameSite::CookieConfig_SameSite_STRICT, SameSite::CookieConfig_SameSite_STRICT,
                 SameSite::CookieConfig_SameSite_STRICT, SameSite::CookieConfig_SameSite_STRICT,
                 SameSite::CookieConfig_SameSite_STRICT, SameSite::CookieConfig_SameSite_STRICT));
  oauthHMAC = "4TKyxPV/F7yyvr0XgJ2bkWFOc8t4IOFen1k29b84MAQ=;";
  TestScopedRuntime scoped_runtime;
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(1000)));
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/_signout"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };

  Http::TestResponseHeaderMapImpl response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthHMAC=" + oauthHMAC + "path=/;Max-Age=600;secure;HttpOnly;SameSite=Strict"},
      {Http::Headers::get().SetCookie.get(),
       "OauthExpires=1600;path=/;Max-Age=600;secure;HttpOnly;SameSite=Strict"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN +
           ";path=/;Max-Age=600;secure;HttpOnly;SameSite=Strict"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + TEST_ENCRYPTED_ID_TOKEN +
           ";path=/;Max-Age=600;secure;HttpOnly;SameSite=Strict"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN +
           ";path=/;Max-Age=604800;secure;HttpOnly;SameSite=Strict"},
      {Http::Headers::get().Location.get(), ""},
  };

  filter_->decodeHeaders(request_headers, false);
  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));

  filter_->onGetAccessTokenSuccess("access_code", "some-id-token", "some-refresh-token",
                                   std::chrono::seconds(600));
}

// Test all cookies with NONE SameSite
TEST_F(OAuth2Test, AllCookiesNoneSameSite) {
  using SameSite = envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite;
  init(getConfig(true, true,
                 envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
                     OAuth2Config_AuthType_URL_ENCODED_BODY,
                 0, false, false, false, false, false, SameSite::CookieConfig_SameSite_NONE,
                 SameSite::CookieConfig_SameSite_NONE, SameSite::CookieConfig_SameSite_NONE,
                 SameSite::CookieConfig_SameSite_NONE, SameSite::CookieConfig_SameSite_NONE,
                 SameSite::CookieConfig_SameSite_NONE));
  oauthHMAC = "4TKyxPV/F7yyvr0XgJ2bkWFOc8t4IOFen1k29b84MAQ=;";
  TestScopedRuntime scoped_runtime;
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(1000)));
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/_signout"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };

  Http::TestResponseHeaderMapImpl response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthHMAC=" + oauthHMAC + "path=/;Max-Age=600;secure;HttpOnly;SameSite=None"},
      {Http::Headers::get().SetCookie.get(),
       "OauthExpires=1600;path=/;Max-Age=600;secure;HttpOnly;SameSite=None"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN +
           ";path=/;Max-Age=600;secure;HttpOnly;SameSite=None"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + TEST_ENCRYPTED_ID_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly;SameSite=None"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN +
           ";path=/;Max-Age=604800;secure;HttpOnly;SameSite=None"},
      {Http::Headers::get().Location.get(), ""},
  };

  filter_->decodeHeaders(request_headers, false);
  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));

  filter_->onGetAccessTokenSuccess("access_code", "some-id-token", "some-refresh-token",
                                   std::chrono::seconds(600));
}

// Test all cookies with LAX SameSite
TEST_F(OAuth2Test, AllCookiesLaxSameSite) {
  using SameSite = envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite;
  init(getConfig(true, true,
                 envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
                     OAuth2Config_AuthType_URL_ENCODED_BODY,
                 0, false, false, false, false, false, SameSite::CookieConfig_SameSite_LAX,
                 SameSite::CookieConfig_SameSite_LAX, SameSite::CookieConfig_SameSite_LAX,
                 SameSite::CookieConfig_SameSite_LAX, SameSite::CookieConfig_SameSite_LAX,
                 SameSite::CookieConfig_SameSite_LAX, SameSite::CookieConfig_SameSite_LAX));
  oauthHMAC = "4TKyxPV/F7yyvr0XgJ2bkWFOc8t4IOFen1k29b84MAQ=;";
  TestScopedRuntime scoped_runtime;
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(1000)));
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/_signout"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };

  Http::TestResponseHeaderMapImpl response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthHMAC=" + oauthHMAC + "path=/;Max-Age=600;secure;HttpOnly;SameSite=Lax"},
      {Http::Headers::get().SetCookie.get(),
       "OauthExpires=1600;path=/;Max-Age=600;secure;HttpOnly;SameSite=Lax"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN +
           ";path=/;Max-Age=600;secure;HttpOnly;SameSite=Lax"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + TEST_ENCRYPTED_ID_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly;SameSite=Lax"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN +
           ";path=/;Max-Age=604800;secure;HttpOnly;SameSite=Lax"},
      {Http::Headers::get().Location.get(), ""},
  };

  filter_->decodeHeaders(request_headers, false);
  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));

  filter_->onGetAccessTokenSuccess("access_code", "some-id-token", "some-refresh-token",
                                   std::chrono::seconds(600));
}

// Test mixed SameSite configurations with some disabled
TEST_F(OAuth2Test, MixedCookieSameSiteWithDisabled) {
  using SameSite = envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite;
  init(getConfig(true, true,
                 envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
                     OAuth2Config_AuthType_URL_ENCODED_BODY,
                 0, false, false, false, false, false, SameSite::CookieConfig_SameSite_STRICT,
                 SameSite::CookieConfig_SameSite_LAX, SameSite::CookieConfig_SameSite_DISABLED,
                 SameSite::CookieConfig_SameSite_NONE, SameSite::CookieConfig_SameSite_STRICT,
                 SameSite::CookieConfig_SameSite_DISABLED, SameSite::CookieConfig_SameSite_LAX));
  oauthHMAC = "4TKyxPV/F7yyvr0XgJ2bkWFOc8t4IOFen1k29b84MAQ=;";
  TestScopedRuntime scoped_runtime;
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(1000)));
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/_signout"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };

  Http::TestResponseHeaderMapImpl response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthHMAC=" + oauthHMAC + "path=/;Max-Age=600;secure;HttpOnly;SameSite=Lax"},
      {Http::Headers::get().SetCookie.get(),
       "OauthExpires=1600;path=/;Max-Age=600;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN +
           ";path=/;Max-Age=600;secure;HttpOnly;SameSite=Strict"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + TEST_ENCRYPTED_ID_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly;SameSite=None"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN +
           ";path=/;Max-Age=604800;secure;HttpOnly;SameSite=Strict"},
      {Http::Headers::get().Location.get(), ""},
  };

  filter_->decodeHeaders(request_headers, false);
  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));

  filter_->onGetAccessTokenSuccess("access_code", "some-id-token", "some-refresh-token",
                                   std::chrono::seconds(600));
}

// Test mixed SameSite configurations without disabled
TEST_F(OAuth2Test, MixedCookieSameSiteWithoutDisabled) {
  using SameSite = envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite;
  init(getConfig(true, true,
                 envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
                     OAuth2Config_AuthType_URL_ENCODED_BODY,
                 0, false, false, false, false, false, SameSite::CookieConfig_SameSite_STRICT,
                 SameSite::CookieConfig_SameSite_LAX, SameSite::CookieConfig_SameSite_NONE,
                 SameSite::CookieConfig_SameSite_STRICT, SameSite::CookieConfig_SameSite_LAX,
                 SameSite::CookieConfig_SameSite_NONE, SameSite::CookieConfig_SameSite_LAX));
  oauthHMAC = "4TKyxPV/F7yyvr0XgJ2bkWFOc8t4IOFen1k29b84MAQ=;";
  TestScopedRuntime scoped_runtime;
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(1000)));
  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/_signout"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
  };

  Http::TestResponseHeaderMapImpl response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthHMAC=" + oauthHMAC + "path=/;Max-Age=600;secure;HttpOnly;SameSite=Lax"},
      {Http::Headers::get().SetCookie.get(),
       "OauthExpires=1600;path=/;Max-Age=600;secure;HttpOnly;SameSite=None"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN +
           ";path=/;Max-Age=600;secure;HttpOnly;SameSite=Strict"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=" + TEST_ENCRYPTED_ID_TOKEN +
           ";path=/;Max-Age=600;secure;HttpOnly;SameSite=Strict"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN +
           ";path=/;Max-Age=604800;secure;HttpOnly;SameSite=Lax"},
      {Http::Headers::get().Location.get(), ""},
  };

  filter_->decodeHeaders(request_headers, false);
  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));

  filter_->onGetAccessTokenSuccess("access_code", "some-id-token", "some-refresh-token",
                                   std::chrono::seconds(600));
}

TEST_F(OAuth2Test, CSRFSameSiteWithCookieDomain) {
  using SameSite = envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite;
  init(getConfig(true, true,
                 envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
                     OAuth2Config_AuthType_URL_ENCODED_BODY,
                 0, false, false, true, false, false, SameSite::CookieConfig_SameSite_DISABLED,
                 SameSite::CookieConfig_SameSite_DISABLED, SameSite::CookieConfig_SameSite_DISABLED,
                 SameSite::CookieConfig_SameSite_DISABLED, SameSite::CookieConfig_SameSite_DISABLED,
                 SameSite::CookieConfig_SameSite_STRICT, SameSite::CookieConfig_SameSite_LAX));
  // First construct the initial request to the oauth filter with URI parameters.
  Http::TestRequestHeaderMapImpl first_request_headers{
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Post},
      {Http::Headers::get().Scheme.get(), "https"},
  };

  // This is the immediate response - a redirect to the auth cluster.
  Http::TestResponseHeaderMapImpl first_response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN +
           ";domain=example.com;path=/;Max-Age=600;secure;HttpOnly;SameSite=Strict"},
      {Http::Headers::get().SetCookie.get(),
       "CodeVerifier=" + TEST_ENCRYPTED_CODE_VERIFIER +
           ";domain=example.com;path=/;Max-Age=600;secure;HttpOnly;SameSite=Lax"},
      {Http::Headers::get().Location.get(),
       "https://auth.example.com/oauth/"
       "authorize/?client_id=" +
           TEST_CLIENT_ID + "&code_challenge=" + TEST_CODE_CHALLENGE +
           "&code_challenge_method=S256" +
           "&redirect_uri=https%3A%2F%2Ftraffic.example.com%2F_oauth"
           "&response_type=code"
           "&scope=" +
           TEST_ENCODED_AUTH_SCOPES + "&state=" + TEST_ENCODED_STATE + "&resource=oauth2-resource" +
           "&resource=http%3A%2F%2Fexample.com"
           "&resource=https%3A%2F%2Fexample.com%2Fsome%2Fpath%252F..%252F%2Futf8%C3%83%3Bfoo%3Dbar%"
           "3Fvar1%3D1%26var2%3D2"},
  };

  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));
  EXPECT_CALL(*validator_, canUpdateTokenByRefreshToken()).WillOnce(Return(false));
  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&first_response_headers), true));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(first_request_headers, false));
}

// Ensure that the token cookies are deleted when the tokens are cleared
TEST_F(OAuth2Test, CookiesDeletedWhenTokensCleared) {
  // Initialize with use_refresh_token set to false
  init(getConfig(true /* forward_bearer_token */, false /* use_refresh_token */));

  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(0)));

  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Cookie.get(),
       "OauthHMAC=4TKyxPV/F7yyvr0XgJ2bkWFOc8t4IOFen1k29b84MAQ=;path=/"
       ";Max-Age=600;secure;HttpOnly;SameSite=Lax"},
      {Http::Headers::get().Cookie.get(),
       "OauthExpires=1600;path=/;Max-Age=600;secure;HttpOnly;SameSite=None"},
      {Http::Headers::get().Cookie.get(),
       "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN +
           ";path=/;Max-Age=600;secure;HttpOnly;SameSite=Strict"},
      {Http::Headers::get().Cookie.get(),
       "IdToken=" + TEST_ENCRYPTED_ID_TOKEN +
           ";path=/;Max-Age=600;secure;HttpOnly;SameSite=Strict"},
      {Http::Headers::get().Cookie.get(),
       "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN +
           ";path=/;Max-Age=604800;secure;HttpOnly;SameSite=Lax"},
      {Http::Headers::get().Cookie.get(),
       "OauthNonce=" + TEST_CSRF_TOKEN + ";path=/;Max-Age=600;secure;HttpOnly;SameSite=Strict"},
  };

  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(false));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));

  // Expect to clear the headers
  Http::TestResponseHeaderMapImpl response_headers{
      {Http::Headers::get().Status.get(), "302"},
      {Http::Headers::get().SetCookie.get(),
       "OauthHMAC=8p68j+W60Z7OJUXYNYpVQfkb+XRUm01bM0M/xzTRVBU=;path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(), "OauthExpires=10;path=/;Max-Age=10;secure;HttpOnly"},
      {Http::Headers::get().SetCookie.get(),
       "BearerToken=deleted; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT"},
      {Http::Headers::get().SetCookie.get(),
       "IdToken=deleted; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT"},
      {Http::Headers::get().SetCookie.get(),
       "RefreshToken=deleted; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT"},
      {Http::Headers::get().Location.get(), ""},
  };
  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));

  const std::chrono::seconds expiredTime(10);
  filter_->onGetAccessTokenSuccess("", "", "", expiredTime);
}

// Ensure that the token cookies are decrypted before forwarding the request
TEST_F(OAuth2Test, CookiesDecryptedBeforeForwarding) {
  // Initialize with use_refresh_token set to false
  init(getConfig(true /* forward_bearer_token */));

  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(0)));

  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Cookie.get(), "OauthHMAC=4TKyxPV/F7yyvr0XgJ2bkWFOc8t4IOFen1k29b84MAQ="},
      {Http::Headers::get().Cookie.get(), "OauthExpires=1600"},
      {Http::Headers::get().Cookie.get(), "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN},
      {Http::Headers::get().Cookie.get(), "IdToken=" + TEST_ENCRYPTED_ID_TOKEN},
      {Http::Headers::get().Cookie.get(), "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN},
      {Http::Headers::get().Cookie.get(), "OauthNonce=" + TEST_CSRF_TOKEN},
  };

  // cookie-validation mocking
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(true));

  // return reference mocking
  std::string access_token{"access_code"};
  EXPECT_CALL(*validator_, token()).WillRepeatedly(ReturnRef(access_token));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  // Expect the request headers to be updated with the decrypted tokens
  auto cookies = Http::Utility::parseCookies(request_headers);
  EXPECT_EQ(cookies.at("BearerToken"), "access_code");
  EXPECT_EQ(cookies.at("IdToken"), "some-id-token");

  // OAuth flow cookies should be removed before forwarding the request
  EXPECT_EQ(cookies.contains("OauthHMAC"), false);
  EXPECT_EQ(cookies.contains("OauthExpires"), false);
  EXPECT_EQ(cookies.contains("RefreshToken"), false);
  EXPECT_EQ(cookies.contains("OauthNonce"), false);
  EXPECT_EQ(cookies.contains("CodeVerifier"), false);
}

// Ensure that the token cookies are decrypted before forwarding the request
TEST_F(OAuth2Test, CookiesDecryptedBeforeForwardingWithEncryptionDisabled) {
  TestScopedRuntime scoped_runtime;
  scoped_runtime.mergeValues({{"envoy.reloadable_features.oauth2_encrypt_tokens", "false"}});

  // Initialize with use_refresh_token set to false
  init(getConfig(true /* forward_bearer_token */));

  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(0)));

  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Cookie.get(), "OauthHMAC=4TKyxPV/F7yyvr0XgJ2bkWFOc8t4IOFen1k29b84MAQ="},
      {Http::Headers::get().Cookie.get(), "OauthExpires=1600"},
      {Http::Headers::get().Cookie.get(), "BearerToken=access_code"},
      {Http::Headers::get().Cookie.get(), "IdToken=some-id-token"},
      {Http::Headers::get().Cookie.get(), "RefreshToken=some-refresh-token"},
      {Http::Headers::get().Cookie.get(), "OauthNonce=" + TEST_CSRF_TOKEN},
  };

  // cookie-validation mocking
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(true));

  // return reference mocking
  std::string access_token{"access_code"};
  EXPECT_CALL(*validator_, token()).WillRepeatedly(ReturnRef(access_token));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  // Expect the request headers to be updated with the decrypted tokens
  auto cookies = Http::Utility::parseCookies(request_headers);
  EXPECT_EQ(cookies.at("BearerToken"), "access_code");
  EXPECT_EQ(cookies.at("IdToken"), "some-id-token");

  // OAuth flow cookies should be removed before forwarding the request
  EXPECT_EQ(cookies.contains("OauthHMAC"), false);
  EXPECT_EQ(cookies.contains("OauthExpires"), false);
  EXPECT_EQ(cookies.contains("RefreshToken"), false);
  EXPECT_EQ(cookies.contains("OauthNonce"), false);
  EXPECT_EQ(cookies.contains("CodeVerifier"), false);
}

// Ensure that the token cookies are decrypted before forwarding the request
TEST_F(OAuth2Test, CookiesDecryptedBeforeForwardingWithCleanupOAuthCookiesDisabled) {
  TestScopedRuntime scoped_runtime;
  scoped_runtime.mergeValues({{"envoy.reloadable_features.oauth2_cleanup_cookies", "false"}});

  // Initialize with use_refresh_token set to false
  init(getConfig(true /* forward_bearer_token */));

  // Set SystemTime to a fixed point so we get consistent HMAC encodings between test runs.
  test_time_.setSystemTime(SystemTime(std::chrono::seconds(0)));

  Http::TestRequestHeaderMapImpl request_headers{
      {Http::Headers::get().Host.get(), "traffic.example.com"},
      {Http::Headers::get().Path.get(), "/original_path?var1=1&var2=2"},
      {Http::Headers::get().Method.get(), Http::Headers::get().MethodValues.Get},
      {Http::Headers::get().Cookie.get(), "OauthHMAC=4TKyxPV/F7yyvr0XgJ2bkWFOc8t4IOFen1k29b84MAQ="},
      {Http::Headers::get().Cookie.get(), "OauthExpires=1600"},
      {Http::Headers::get().Cookie.get(), "BearerToken=" + TEST_ENCRYPTED_ACCESS_TOKEN},
      {Http::Headers::get().Cookie.get(), "IdToken=" + TEST_ENCRYPTED_ID_TOKEN},
      {Http::Headers::get().Cookie.get(), "RefreshToken=" + TEST_ENCRYPTED_REFRESH_TOKEN},
      {Http::Headers::get().Cookie.get(), "OauthNonce=" + TEST_CSRF_TOKEN},
  };

  // cookie-validation mocking
  EXPECT_CALL(*validator_, setParams(_, _));
  EXPECT_CALL(*validator_, isValid()).WillOnce(Return(true));

  // return reference mocking
  std::string access_token{"access_code"};
  EXPECT_CALL(*validator_, token()).WillRepeatedly(ReturnRef(access_token));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  // Expect the request headers to be updated with the decrypted tokens
  auto cookies = Http::Utility::parseCookies(request_headers);
  EXPECT_EQ(cookies.at("BearerToken"), "access_code");
  EXPECT_EQ(cookies.at("IdToken"), "some-id-token");
  EXPECT_EQ(cookies.at("RefreshToken"), "some-refresh-token");
}

} // namespace Oauth2
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
