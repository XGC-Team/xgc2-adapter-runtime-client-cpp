#pragma once

#include "xgc2/xrpc/grpc.hpp"
#include "xgc2/xrpc/runtime_policy.hpp"

namespace xgc2::adapter_runtime::internal {
struct RpcTransportPolicy {
  xgc2::xrpc::RuntimePolicy policy;
  xgc2::xrpc::GrpcLimits limits;
  explicit RpcTransportPolicy(xgc2::xrpc::RuntimePolicy value)
      : policy(std::move(value)),
        limits(xgc2::xrpc::grpc_client_limits(policy,
                                              {"GRPC_MAX_STREAMS_PER_CONNECTION"})) {
    if (limits.streams_per_connection < 2)
      throw xgc2::xrpc::RuntimePolicyError(
          "GRPC_MAX_STREAMS_PER_CONNECTION",
          "Runtime Link requires exactly one Control/Work pair");
  }
};
}  // namespace xgc2::adapter_runtime::internal
