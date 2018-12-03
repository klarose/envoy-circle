#include "test/integration/src_transparent_integration_test.h"

#include "common/buffer/buffer_impl.h"

namespace Envoy {

SrcTransparentIntegrationTest::~SrcTransparentIntegrationTest() { cleanupConnections(); }
void SrcTransparentIntegrationTest::enableSrcTransparency(size_t cluster_index) {
  config_helper_.addConfigModifier([this, cluster_index](envoy::config::bootstrap::v2::Bootstrap&) {
    // auto* cluster = bootstrap.mutable_static_resources()->mutable_clusters(cluster_index);
    // cluster->mutable_upstream_connection_options()->set_src_transparent(true);
    // cluster->mutable_upstream_bind_config()->mutable_freebind()->set_value(true);
    // cluster->mutable_upstream_bind_config()->mutable_source_address()->set_address("0.0.0.0");
    // cluster->mutable_upstream_bind_config()->mutable_source_address()->set_port_value(0);
  });
}

HttpIntegrationTest::ConnectionCreationFunction
SrcTransparentIntegrationTest::getSourceIpConnectionCreator(const std::string& ip) {
  ConnectionCreationFunction creator = [this, ip]() -> Network::ClientConnectionPtr {
    Network::ClientConnectionPtr conn = makeClientConnection(lookupPort("http"));
    Buffer::OwnedImpl buf("PROXY TCP4 " + ip + " 254.254.254.254 65535 1234\r\n");
    conn->write(buf, false);
    return conn;
  };

  return creator;
}

void SrcTransparentIntegrationTest::sendAndReceiveRepeatable(ConnectionCreationFunction creator) {
  // note: we close the upstream. Otherwise, the server shuts down on us!
  testRouterHeaderOnlyRequestAndResponse(true /* close upstream */, &creator);
  cleanupUpstreamAndDownstream();
  fake_upstream_connection_ = nullptr;
}

void SrcTransparentIntegrationTest::sendHeaderOnlyRequest(ConnectionCreationFunction creator,
                                                          const Http::HeaderMap& headers) {
  auto codec_client = makeHttpConnection(creator());
  auto response = codec_client->makeHeaderOnlyRequest(headers);
  parallel_clients_.emplace_back(std::move(codec_client));
  parallel_responses_.emplace_back(std::move(response));
}

void SrcTransparentIntegrationTest::establishUpstreamInformation(size_t num_upstreams) {
  for (size_t i = 0; i < num_upstreams; i++) {
    waitForNextUpstreamRequest();
    parallel_requests_.emplace_back(std::move(upstream_request_));
    parallel_addresses_.push_back(first_upstream_remote_address_);
    parallel_connections_.emplace_back(std::move(fake_upstream_connection_));
  }
}

void SrcTransparentIntegrationTest::sendHeaderOnlyResponse(
    size_t upstream_index, IntegrationStreamDecoder& expected_response,
    const Http::HeaderMapImpl& headers) {
  parallel_requests_[upstream_index]->encodeHeaders(headers, true /* no body => close */);
  expected_response.waitForEndStream();
  EXPECT_TRUE(expected_response.complete());
}

void SrcTransparentIntegrationTest::cleanupConnections() {
  // the order here is important. See cleanupUpstreamAndDownstream
  cleanupUpstreamConnectionsRange(0, parallel_connections_.size());
  std::for_each(parallel_clients_.begin(), parallel_clients_.end(),
                [](auto& client) { client->close(); });

  parallel_requests_.clear();
  parallel_connections_.clear();
  parallel_responses_.clear();
}

void SrcTransparentIntegrationTest::cleanupUpstreamConnectionsRange(size_t start, size_t end) {
  // the order here is important. See cleanupUpstreamAndDownstream
  std::for_each(std::next(parallel_connections_.begin(), start),
                std::next(parallel_connections_.begin(), end), [](auto& connection) {
                  auto result = connection->close();
                  RELEASE_ASSERT(result, result.message());
                });
  std::for_each(std::next(parallel_connections_.begin(), start),
                std::next(parallel_connections_.begin(), end), [](auto& connection) {
                  auto result = connection->waitForDisconnect();
                  RELEASE_ASSERT(result, result.message());
                });
}

void SrcTransparentIntegrationTest::cleanupSingleUpstreamConnection(size_t index) {
  cleanupUpstreamConnectionsRange(index, index + 1);
}

TEST_F(SrcTransparentIntegrationTest, basicTransparency) {
  auto creator = getSourceIpConnectionCreator("127.0.0.2");
  enableSrcTransparency(0);
  testRouterRequestAndResponseWithBody(1024, 512, false, &creator);
  auto expected_ip = Network::Utility::parseInternetAddress("127.0.0.2");
  EXPECT_EQ(expected_ip->ip()->ipv4()->address(),
            first_upstream_remote_address_->ip()->ipv4()->address());
}

TEST_F(SrcTransparentIntegrationTest, backToBackConnectionsSameIp) {
  enableSrcTransparency(0);
  auto creator = getSourceIpConnectionCreator("127.0.0.2");
  sendAndReceiveRepeatable(creator);
  sendAndReceiveRepeatable(creator);
  auto expected_ip = Network::Utility::parseInternetAddress("127.0.0.2");
  EXPECT_EQ(expected_ip->ip()->ipv4()->address(),
            first_upstream_remote_address_->ip()->ipv4()->address());
}

TEST_F(SrcTransparentIntegrationTest, backToBackConnectionsDifferentIp) {
  enableSrcTransparency(0);
  auto creator1 = getSourceIpConnectionCreator("127.0.0.2");
  sendAndReceiveRepeatable(creator1);
  auto creator2 = getSourceIpConnectionCreator("127.0.0.3");
  sendAndReceiveRepeatable(creator2);
  auto expected_ip = Network::Utility::parseInternetAddress("127.0.0.3");
  EXPECT_EQ(expected_ip->ip()->ipv4()->address(),
            first_upstream_remote_address_->ip()->ipv4()->address());
}

TEST_F(SrcTransparentIntegrationTest, parallelDownstreamParallelUpstream) {
  enableSrcTransparency(0);
  initialize();
  auto creator = getSourceIpConnectionCreator("127.0.0.2");
  sendHeaderOnlyRequest(creator, default_request_headers_);
  sendHeaderOnlyRequest(creator, default_request_headers_);
  establishUpstreamInformation(2);
  sendHeaderOnlyResponse(0, *parallel_responses_[0], default_response_headers_);
  sendHeaderOnlyResponse(1, *parallel_responses_[1], default_response_headers_);

  EXPECT_TRUE(parallel_requests_[0]->complete());
  EXPECT_TRUE(parallel_requests_[1]->complete());

  auto expected_ip = Network::Utility::parseInternetAddress("127.0.0.2");
  EXPECT_EQ(expected_ip->ip()->ipv4()->address(), parallel_addresses_[0]->ip()->ipv4()->address());
  EXPECT_EQ(expected_ip->ip()->ipv4()->address(), parallel_addresses_[1]->ip()->ipv4()->address());
}

TEST_F(SrcTransparentIntegrationTest, parallelDownstreamSameUpstream) {
  enableSrcTransparency(0);

  // Force use of the same upstream connection by only allowing one at a time.
  config_helper_.addConfigModifier([this](envoy::config::bootstrap::v2::Bootstrap& bootstrap) {
    auto* cluster = bootstrap.mutable_static_resources()->mutable_clusters(0);
    cluster->mutable_circuit_breakers()->add_thresholds()->mutable_max_connections()->set_value(1);
  });

  initialize();
  auto creator = getSourceIpConnectionCreator("127.0.0.2");
  sendHeaderOnlyRequest(creator, default_request_headers_);
  sendHeaderOnlyRequest(creator, default_request_headers_);
  establishUpstreamInformation(1);
  sendHeaderOnlyResponse(0, *parallel_responses_[0], default_response_headers_);
  sendHeaderOnlyResponse(0, *parallel_responses_[1], default_response_headers_);
  EXPECT_TRUE(parallel_requests_[0]->complete());

  auto expected_ip = Network::Utility::parseInternetAddress("127.0.0.2");
  EXPECT_EQ(expected_ip->ip()->ipv4()->address(), parallel_addresses_[0]->ip()->ipv4()->address());
}

TEST_F(SrcTransparentIntegrationTest, parallelDownstreamParallelUpstreamDifferentIPs) {
  enableSrcTransparency(0);
  initialize();
  auto creator1 = getSourceIpConnectionCreator("127.0.0.2");
  auto creator2 = getSourceIpConnectionCreator("127.0.1.3");
  sendHeaderOnlyRequest(creator1, default_request_headers_);
  sendHeaderOnlyRequest(creator2, default_request_headers_);
  establishUpstreamInformation(2);
  sendHeaderOnlyResponse(0, *parallel_responses_[0], default_response_headers_);
  sendHeaderOnlyResponse(1, *parallel_responses_[1], default_response_headers_);

  EXPECT_TRUE(parallel_requests_[0]->complete());
  EXPECT_TRUE(parallel_requests_[1]->complete());

  auto expected_ip1 = Network::Utility::parseInternetAddress("127.0.0.2");
  auto expected_ip2 = Network::Utility::parseInternetAddress("127.0.1.3");
  EXPECT_EQ(expected_ip1->ip()->ipv4()->address(), parallel_addresses_[0]->ip()->ipv4()->address());
  EXPECT_EQ(expected_ip2->ip()->ipv4()->address(), parallel_addresses_[1]->ip()->ipv4()->address());
}

// this test differs from parallelDownstreamSameUpstream in that the IPs are different, which should
// force it to create new connections.
TEST_F(SrcTransparentIntegrationTest, parallelDownstreamSerialUpstream) {
  enableSrcTransparency(0);

  // Force use of the same upstream connection by only allowing one at a time.
  config_helper_.addConfigModifier([this](envoy::config::bootstrap::v2::Bootstrap& bootstrap) {
    auto* cluster = bootstrap.mutable_static_resources()->mutable_clusters(0);
    cluster->mutable_circuit_breakers()->add_thresholds()->mutable_max_connections()->set_value(1);
  });

  initialize();
  auto creator1 = getSourceIpConnectionCreator("127.1.0.2");
  auto creator2 = getSourceIpConnectionCreator("127.0.1.4");
  sendHeaderOnlyRequest(creator1, default_request_headers_);
  sendHeaderOnlyRequest(creator2, default_request_headers_);
  establishUpstreamInformation(1);
  sendHeaderOnlyResponse(0, *parallel_responses_[0], default_response_headers_);
  EXPECT_TRUE(parallel_requests_[0]->complete());
  // There'll be a new connection now, but we need to wait for the first to finish!
  establishUpstreamInformation(1);
  sendHeaderOnlyResponse(1, *parallel_responses_[1], default_response_headers_);
  EXPECT_TRUE(parallel_requests_[1]->complete());

  auto expected_ip1 = Network::Utility::parseInternetAddress("127.1.0.2");
  auto expected_ip2 = Network::Utility::parseInternetAddress("127.0.1.4");
  EXPECT_EQ(expected_ip1->ip()->ipv4()->address(), parallel_addresses_[0]->ip()->ipv4()->address());
  EXPECT_EQ(expected_ip2->ip()->ipv4()->address(), parallel_addresses_[1]->ip()->ipv4()->address());
}

TEST_F(SrcTransparentIntegrationTest, upstreamFailureDoesNotStopNewConnectionsDifferentIPs) {
  enableSrcTransparency(0);
  initialize();
  auto creator1 = getSourceIpConnectionCreator("127.6.0.2");
  auto creator2 = getSourceIpConnectionCreator("127.0.1.9");
  sendHeaderOnlyRequest(creator1, default_request_headers_);
  establishUpstreamInformation(1);

  // Clean everything up before we send a response.
  cleanupConnections();

  // Get a new upstream -- replaces the first one, since it is now closed.
  sendHeaderOnlyRequest(creator2, default_request_headers_);
  establishUpstreamInformation(1);
  sendHeaderOnlyResponse(0, *parallel_responses_[0], default_response_headers_);
  EXPECT_TRUE(parallel_requests_[0]->complete());

  auto expected_ip1 = Network::Utility::parseInternetAddress("127.6.0.2");
  auto expected_ip2 = Network::Utility::parseInternetAddress("127.0.1.9");
  EXPECT_EQ(expected_ip1->ip()->ipv4()->address(), parallel_addresses_[0]->ip()->ipv4()->address());
  EXPECT_EQ(expected_ip2->ip()->ipv4()->address(), parallel_addresses_[1]->ip()->ipv4()->address());
}

TEST_F(SrcTransparentIntegrationTest, upstreamFailureDoesNotStopNewConnectionsSameIPs) {
  enableSrcTransparency(0);
  initialize();
  auto creator1 = getSourceIpConnectionCreator("127.6.0.2");
  sendHeaderOnlyRequest(creator1, default_request_headers_);
  establishUpstreamInformation(1);

  // Clean everything up before we send a response.
  cleanupConnections();

  // Get a new upstream -- replaces the first one, since it is now closed.
  sendHeaderOnlyRequest(creator1, default_request_headers_);
  establishUpstreamInformation(1);
  sendHeaderOnlyResponse(0, *parallel_responses_[0], default_response_headers_);
  EXPECT_TRUE(parallel_requests_[0]->complete());

  auto expected_ip1 = Network::Utility::parseInternetAddress("127.6.0.2");
  EXPECT_EQ(expected_ip1->ip()->ipv4()->address(), parallel_addresses_[0]->ip()->ipv4()->address());
}

TEST_F(SrcTransparentIntegrationTest, upstreamFailureDoesNotStopParallelConnectionsSameIPs) {
  enableSrcTransparency(0);
  initialize();
  auto creator1 = getSourceIpConnectionCreator("127.6.0.2");
  sendHeaderOnlyRequest(creator1, default_request_headers_);
  sendHeaderOnlyRequest(creator1, default_request_headers_);
  establishUpstreamInformation(2);

  cleanupSingleUpstreamConnection(0);

  sendHeaderOnlyResponse(1, *parallel_responses_[1], default_response_headers_);
  EXPECT_TRUE(parallel_requests_[1]->complete());

  auto expected_ip = Network::Utility::parseInternetAddress("127.6.0.2");
  EXPECT_EQ(expected_ip->ip()->ipv4()->address(), parallel_addresses_[1]->ip()->ipv4()->address());
}

TEST_F(SrcTransparentIntegrationTest, upstreamFailureDoesNotStopSerialConnectionsSameIPs) {
  // Force the second request to queue by only allowing one active at a time.
  config_helper_.addConfigModifier([this](envoy::config::bootstrap::v2::Bootstrap& bootstrap) {
    auto* cluster = bootstrap.mutable_static_resources()->mutable_clusters(0);
    cluster->mutable_circuit_breakers()->add_thresholds()->mutable_max_connections()->set_value(1);
  });

  enableSrcTransparency(0);
  initialize();
  auto creator1 = getSourceIpConnectionCreator("127.6.0.2");
  sendHeaderOnlyRequest(creator1, default_request_headers_);
  sendHeaderOnlyRequest(creator1, default_request_headers_);
  establishUpstreamInformation(1);

  cleanupSingleUpstreamConnection(0);

  establishUpstreamInformation(1);
  sendHeaderOnlyResponse(1, *parallel_responses_[1], default_response_headers_);
  EXPECT_TRUE(parallel_requests_[1]->complete());

  auto expected_ip = Network::Utility::parseInternetAddress("127.6.0.2");
  EXPECT_EQ(expected_ip->ip()->ipv4()->address(), parallel_addresses_[1]->ip()->ipv4()->address());
}
} // namespace Envoy
