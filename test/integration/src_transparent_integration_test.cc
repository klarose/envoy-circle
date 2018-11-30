#include "test/integration/src_transparent_integration_test.h"

#include "common/buffer/buffer_impl.h"

namespace Envoy {

void SrcTransparentIntegrationTest::enableSrcTransparency(size_t cluster_index) {
    config_helper_.addConfigModifier([this, cluster_index](envoy::config::bootstrap::v2::Bootstrap&) {
      //auto* cluster = bootstrap.mutable_static_resources()->mutable_clusters(cluster_index);
      //cluster->mutable_upstream_connection_options()->set_src_transparent(true);
      //cluster->mutable_upstream_bind_config()->mutable_freebind()->set_value(true);
      //cluster->mutable_upstream_bind_config()->mutable_source_address()->set_address("0.0.0.0");
      //cluster->mutable_upstream_bind_config()->mutable_source_address()->set_port_value(0);
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

}
