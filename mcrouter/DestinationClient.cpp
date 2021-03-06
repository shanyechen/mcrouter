/**
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 */
#include "DestinationClient.h"

#include <mutex>

#include <folly/Conv.h>
#include <folly/Memory.h>

#include "mcrouter/lib/network/ThreadLocalSSLContextProvider.h"
#include "mcrouter/proxy.h"
#include "mcrouter/ProxyDestination.h"
#include "mcrouter/route.h"

namespace facebook { namespace memcache { namespace mcrouter {

DestinationClient::DestinationClient(std::shared_ptr<ProxyDestination> pdstn)
    : proxy_(pdstn->proxy),
      pdstn_(pdstn) {
}

void DestinationClient::resetInactive() {
  // No need to reset non-existing client.
  if (asyncMcClient_) {
    asyncMcClient_->closeNow();
    asyncMcClient_.reset();
  }
}

void DestinationClient::onReply(McReply reply, mc_op_t op, void* req_ctx,
                                std::weak_ptr<ProxyDestination> pdstn) {
  auto pdstnPtr = pdstn.lock();
  // ProxyDestination is already dead, just return.
  if (!pdstnPtr) {
    return;
  }
  auto proxy = pdstnPtr->proxy;

  if (reply.result() == mc_res_local_error) {
    update_send_stats(proxy, op, PROXY_SEND_LOCAL_ERROR);
  } else {
    stat_incr(proxy->stats, sum_server_queue_length_stat, 1);
    update_send_stats(proxy, op, PROXY_SEND_OK);
  }

  pdstnPtr->on_reply(std::move(reply),
                     req_ctx);
}

void DestinationClient::initializeAsyncMcClient() {
  FBI_ASSERT(proxy_->eventBase);

  auto pdstn = pdstn_.lock();
  assert(pdstn != nullptr);

  ConnectionOptions options(pdstn->accessPoint);
  options.noNetwork = proxy_->opts.no_network;
  options.tcpKeepAliveCount = proxy_->opts.keepalive_cnt;
  options.tcpKeepAliveIdle = proxy_->opts.keepalive_idle_s;
  options.tcpKeepAliveInterval = proxy_->opts.keepalive_interval_s;
  options.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::seconds(pdstn->server_timeout.tv_sec) +
    std::chrono::microseconds(pdstn->server_timeout.tv_usec));
  if (proxy_->opts.enable_qos) {
    options.enableQoS = true;
    options.qos = pdstn->qos;
  }

  if (pdstn->use_ssl) {
    auto& opts = proxy_->opts;
    checkLogic(!opts.pem_cert_path.empty() &&
               !opts.pem_key_path.empty() &&
               !opts.pem_ca_path.empty(),
               "Some of ssl key paths are not set!");
    options.sslContextProvider = [&opts] {
      return getSSLContext(opts.pem_cert_path, opts.pem_key_path,
                           opts.pem_ca_path);
    };
  }

  asyncMcClient_ = folly::make_unique<AsyncMcClient>(*proxy_->eventBase,
                                                     std::move(options));

  auto pdstnWeakPtr = pdstn_;
  asyncMcClient_->setStatusCallbacks(
    [pdstnWeakPtr] () {
      auto pdstnPtr = pdstnWeakPtr.lock();
      if (!pdstnPtr) {
        return;
      }
      pdstnPtr->on_up();
    },
    [pdstnWeakPtr] (const apache::thrift::transport::TTransportException&) {
      auto pdstnPtr = pdstnWeakPtr.lock();
      if (!pdstnPtr) {
        return;
      }
      pdstnPtr->on_down();
    });

  if (proxy_->opts.target_max_inflight_requests > 0) {
    asyncMcClient_->setThrottle(proxy_->opts.target_max_inflight_requests,
                                proxy_->opts.target_max_pending_requests);
  }
}

AsyncMcClient& DestinationClient::getAsyncMcClient() {
  assert(!pdstn_.expired());
  if (!asyncMcClient_) {
    initializeAsyncMcClient();
  }
  return *asyncMcClient_;
}

size_t DestinationClient::getPendingRequestCount() const {
  return asyncMcClient_ ? asyncMcClient_->getPendingRequestCount() : 0;
}

size_t DestinationClient::getInflightRequestCount() const {
  return asyncMcClient_ ? asyncMcClient_->getInflightRequestCount() : 0;
}

std::pair<uint64_t, uint64_t> DestinationClient::getBatchingStat() const {
  return asyncMcClient_ ? asyncMcClient_->getBatchingStat()
                        : std::make_pair(0UL, 0UL);
}

DestinationClient::~DestinationClient() {
  if (asyncMcClient_) {
    asyncMcClient_->setStatusCallbacks(nullptr, nullptr);
    asyncMcClient_->closeNow();
  }
}

}}}  // facebook::memcache::mcrouter
