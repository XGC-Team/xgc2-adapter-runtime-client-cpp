#include "rpc_policy.hpp"

#include "xgc2/adapter_runtime/client.hpp"

namespace xgc2::adapter_runtime {
void ClientConfig::ApplyXrpcEnvironment(
    const std::vector<std::pair<std::string, std::string>>& environment) {
  xgc2::xrpc::RuntimePolicyOptions options;
  options.environment = environment;
  options.capabilities = {"rpc", "transport", "grpc"};
  options.ceilings = {{"MAX_REQUEST_BYTES", 1048576},
                      {"MAX_RESPONSE_BYTES", 1048576},
                      {"CALL_TIMEOUT_MS", 30000},
                      {"GRPC_MAX_STREAMS_PER_CONNECTION", 32}};
  auto transport = std::make_shared<const internal::RpcTransportPolicy>(
      xgc2::xrpc::resolve_runtime_policy(options));
  xgc::adapter::v1::RuntimePolicySnapshot snapshot;
  snapshot.set_revision(transport->policy.revision());
  for (const auto& field : transport->policy.fields()) {
    auto* wire = snapshot.add_fields();
    wire->set_name(std::string(field.name));
    if (const auto* value = std::get_if<std::int64_t>(&field.value))
      wire->set_integer_value(*value);
    else
      wire->set_text_value(std::get<std::string>(field.value));
    wire->set_source(std::string(field.source));
    wire->set_source_detail(field.source_detail);
    wire->set_dynamic(field.dynamic);
    wire->set_unit(std::string(field.unit));
    wire->set_capability(std::string(field.capability));
    if (field.ceiling) {
      wire->set_has_ceiling(true);
      wire->set_ceiling(*field.ceiling);
    }
    if (field.maximum) {
      wire->set_has_maximum(true);
      wire->set_maximum(*field.maximum);
    }
  }
  xrpc_transport_ = std::move(transport);
  xrpc_runtime_policy_ = std::move(snapshot);
}
}  // namespace xgc2::adapter_runtime
