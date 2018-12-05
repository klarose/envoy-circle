#pragma once
#include <functional>

#include "envoy/network/address.h"
#include "envoy/network/connection.h"

#include "common/http/codec_client.h"

#include "test/integration/http_integration.h"
#include "test/integration/server.h"

#include "gtest/gtest.h"

namespace Envoy {
class SrcTransparentIntegrationTest : public HttpIntegrationTest,
                                      public testing::TestWithParam<Http::CodecClient::Type> {
public:
  SrcTransparentIntegrationTest()
      // Note the v4 here. Unfortunately we can't test easily with V6 as the V6 loopback address
      // is a /128 -- there is no easy way to bind our sender to anything else without getting
      // CAP_NET_ADMIN permissions. While it's not ideal, the risk is fairly low, since very little
      // of the logic we are using is specific to IPv6.
      : HttpIntegrationTest(GetParam(), Network::Address::IpVersion::v4, realTime()) {
    // we will control the downstream remote address using proxy protocol. So, configure a listener
    // to do this.
    config_helper_.addConfigModifier(
        [&](envoy::config::bootstrap::v2::Bootstrap& bootstrap) -> void {
          auto* listener = bootstrap.mutable_static_resources()->mutable_listeners(0);
          auto* filter_chain = listener->mutable_filter_chains(0);
          filter_chain->mutable_use_proxy_proto()->set_value(true);
        });

    auto version = GetParam();
    setDownstreamProtocol(version);
    switch(version) {
      case Http::CodecClient::Type::HTTP1:
        setUpstreamProtocol(FakeHttpConnection::Type::HTTP1);
      break;
      case Http::CodecClient::Type::HTTP2:
        setUpstreamProtocol(FakeHttpConnection::Type::HTTP2);
        break;
    }
  }

  ~SrcTransparentIntegrationTest();

protected:
  void enableSrcTransparency(size_t cluster_index = 0);
  ConnectionCreationFunction getSourceIpConnectionCreator(const std::string& ip);
  ConnectionCreationWithPortFunction getPerPortSourceIpConnectionCreator(const std::string& ip);

  void sendAndReceiveRepeatable(ConnectionCreationFunction creator);
  //! Connects to the server using @c creator, then sends the headers from @c. The resulting client
  //! and response are stored in @c parallel_clients_ and parallel_responses_
  void sendHeaderOnlyRequest(ConnectionCreationFunction creator, const Http::HeaderMap& headers);

  // Sends a response comprising @c headers down the first upstream hosts's upstream_index-th
  // connection. Waits for the response to be received by @c expected_response.
  // Note that we decouple response and upstream_index here so that multiple downstream requests
  // can be muxed to the same upstream connection.
  void sendHeaderOnlyResponse(size_t upstream_index, IntegrationStreamDecoder& expected_response,
                              const Http::HeaderMapImpl& headers);

  //! Waits for @c num_upstream requests to come into the first upstream host.
  //! For each connection, stores their information in parallel_requests_, parallel_addresses_ and
  //! parallel_connections_
  void establishUpstreamInformation(size_t num_upstreams);

  void cleanupConnections();
  void cleanupUpstreamConnectionsRange(size_t start, size_t end);
  void cleanupSingleUpstreamConnection(size_t index);
  std::vector<IntegrationCodecClientPtr> parallel_clients_;
  std::vector<IntegrationStreamDecoderPtr> parallel_responses_;
  std::vector<FakeStreamPtr> parallel_requests_;
  std::vector<Network::Address::InstanceConstSharedPtr> parallel_addresses_;
  std::vector<FakeHttpConnectionPtr> parallel_connections_;
  Http::TestHeaderMapImpl default_request_headers_ =
      Http::TestHeaderMapImpl{{":method", "GET"},
                              {":path", "/test/long/url"},
                              {":scheme", "http"},
                              {":authority", "host"},
                              {"x-lyft-user-id", "123"}};
};

//! Used to run the base http integration tests with source transparency configured.
//! The general strategy is to shim in a proxy protocol connection builder to the existing
//! infrastructure so that we can just reuse the tests without any extra work.
class SrcTransparentHttpIntegrationTest : public SrcTransparentIntegrationTest {
public:
  SrcTransparentHttpIntegrationTest();

protected:
  //! Creates a proxy protocol connection with a fixed IP and port.
  ConnectionCreationFunction proxy_protocol_creator_;
};
} // namespace Envoy
