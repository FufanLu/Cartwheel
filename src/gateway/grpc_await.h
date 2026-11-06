#pragma once

#include <functional>
#include <memory>
#include <utility>

#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <grpcpp/support/status.h>

namespace cartwheel::gateway {

// Bridges gRPC's callback API into an Asio C++20 coroutine:
//
//   grpc::Status s = co_await AwaitGrpc([&](auto done) {
//     stub->async()->GetCart(&ctx, &req, &resp, std::move(done));
//   });
//
// gRPC completes the call on one of its own threads. The completion is
// posted back to the coroutine's executor, so the coroutine always resumes on
// its connection's io_context thread and never on a gRPC thread. No thread
// blocks while the RPC is in flight, so one gateway thread can keep thousands
// of upstream calls outstanding.
template <typename Start>
boost::asio::awaitable<grpc::Status> AwaitGrpc(Start start) {
  return boost::asio::async_initiate<const boost::asio::use_awaitable_t<>, void(grpc::Status)>(
      [start = std::move(start)](auto handler) mutable {
        // std::function (what gRPC takes) must be copyable; the coroutine
        // handler is move-only, so share it.
        auto shared = std::make_shared<decltype(handler)>(std::move(handler));
        start(std::function<void(grpc::Status)>([shared](grpc::Status status) {
          auto ex = boost::asio::get_associated_executor(*shared);
          boost::asio::post(ex, [shared, status = std::move(status)]() mutable {
            std::move (*shared)(std::move(status));
          });
        }));
      },
      boost::asio::use_awaitable);
}

}  // namespace cartwheel::gateway
