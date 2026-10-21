add_executable(comlink-cli main.c)
target_link_libraries(comlink-cli PRIVATE comlink_core)
target_include_directories(comlink-cli PRIVATE ${CMAKE_SOURCE_DIR}/core/include)

include(GNUInstallDirs)
install(TARGETS comlink-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_SOURCE_DIR}/packaging/comlink.example.yaml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/comlink)
