target_sources(am_exclusive_hook PRIVATE src/output_device/EndpointRedirect.cpp)

target_sources(am_exclusive_ui PRIVATE src/output_device/OutputDeviceUi.cpp)
target_link_libraries(am_exclusive_ui PRIVATE uuid)
