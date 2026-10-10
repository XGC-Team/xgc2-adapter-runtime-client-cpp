# xgc2-adapter-runtime-client-cpp

C++14 SDK for connecting adapter processes to XGC2 Adapter Runtime Link.

## Install

```bash
sudo apt install libxgc2-adapter-runtime-client-dev
```

## CMake

```cmake
find_package(xgc2_adapter_runtime_client 0.7.0 EXACT REQUIRED CONFIG)
target_link_libraries(my_adapter PRIVATE xgc2::adapter_runtime_client)
```

Public header: `<xgc2/adapter_runtime/client.hpp>`.
