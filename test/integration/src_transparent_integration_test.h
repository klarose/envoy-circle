#pragma once
#include <functional>

#include "envoy/network/connection.h"

#include "common/http/codec_client.h"

#include "test/integration/http_integration.h"
#include "test/integration/server.h"

#include "gtest/gtest.h"

namespace Envoy {
class SrcTransparentIntegrationTest : public HttpIntegrationTest,
                                      public testing::Test {
public:
  SrcTransparentIntegrationTest()
    // Note the v4 here. Unfortunately we can't test easily with V6 as the V6 loopback address
    // is a /128 -- there is no easy way to bind our sender to anything else without getting
    // CAP_NET_ADMIN permissions. While it's not ideal, the risk is fairly low, since very little
    // of the logic we are using is specific to IPv6.
    : HttpIntegrationTest(Http::CodecClient::Type::HTTP1, Network::Address::IpVersion::v4,
      realTime()) {
    // we will control the downstream remote address using proxy protocol. So, configure a listener
    // to do this.
    config_helper_.addConfigModifier(
        [&](envoy::config::bootstrap::v2::Bootstrap& bootstrap) -> void {
          auto* listener = bootstrap.mutable_static_resources()->mutable_listeners(0);
          auto* filter_chain = listener->mutable_filter_chains(0);
          filter_chain->mutable_use_proxy_proto()->set_value(true);
        });
  }
protected:
  void enableSrcTransparency(size_t cluster_index = 0);
  void sendAndReceiveRepeatable(ConnectionCreationFunction creator);

  ConnectionCreationFunction getSourceIpConnectionCreator(const std::string& ip);
};
} // namespace Envoy
