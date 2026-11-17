add_executable(core_tests CoreTests.cpp ${PROJECT_SOURCE_DIR}/src/app/KrdcImport.cpp ${PROJECT_SOURCE_DIR}/src/app/DomainXml.cpp
    ${PROJECT_SOURCE_DIR}/src/app/Addresses.cpp ${PROJECT_SOURCE_DIR}/src/app/Proxmox.cpp)
target_link_libraries(core_tests PRIVATE rdbackends Qt6::Core)
target_compile_definitions(core_tests PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME core_tests COMMAND core_tests)
