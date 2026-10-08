#pragma once

#include <cstdint>

#define XGC2_ADAPTER_RUNTIME_CLIENT_VERSION_MAJOR 0
#define XGC2_ADAPTER_RUNTIME_CLIENT_VERSION_MINOR 7
#define XGC2_ADAPTER_RUNTIME_CLIENT_VERSION_PATCH 0
#define XGC2_ADAPTER_RUNTIME_CLIENT_ABI_VERSION 3

namespace xgc2 {
namespace adapter_runtime {

constexpr const char* kClientVersion = "0.7.0";
constexpr int kClientAbiVersion = XGC2_ADAPTER_RUNTIME_CLIENT_ABI_VERSION;
constexpr std::uint32_t kAdapterBootstrapFormatVersion = 3;
constexpr std::uint32_t kRuntimeLinkProtocolVersion = 2;

}  // namespace adapter_runtime
}  // namespace xgc2
