add_executable(agmesh_unit_tests
    unit/test_main.cpp
)

target_link_libraries(agmesh_unit_tests
    PRIVATE
        agmesh::core
        agmesh::catch2
)
agmesh_apply_compiler_warnings(agmesh_unit_tests)
agmesh_apply_sanitizers(agmesh_unit_tests)

add_executable(agmesh_integration_tests
    integration/test_cluster_5nodes.cpp
)

target_link_libraries(agmesh_integration_tests
    PRIVATE
        agmesh::core
        agmesh::catch2
)
agmesh_apply_compiler_warnings(agmesh_integration_tests)
agmesh_apply_sanitizers(agmesh_integration_tests)

include(CTest)
add_test(NAME agmesh_unit_tests COMMAND agmesh_unit_tests)
add_test(NAME agmesh_integration_tests COMMAND agmesh_integration_tests)
