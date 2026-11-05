project(SatoriLocalCollectionOutcomesNative)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_library(SatoriLocalCollectionOutcomesNative SHARED LocalCollectionOutcomes.cpp)
    target_include_directories(SatoriLocalCollectionOutcomesNative PRIVATE
        ${INC_PLATFORM_DIR}
        ${CLR_REPO_ROOT_DIR}/src/coreclr/gc/satori)
    target_compile_definitions(SatoriLocalCollectionOutcomesNative PRIVATE SATORI_LOCAL_COLLECTION_TEST_LIBRARY)
    target_link_libraries(SatoriLocalCollectionOutcomesNative PRIVATE ${LINK_LIBRARIES_ADDITIONAL})
    install(TARGETS SatoriLocalCollectionOutcomesNative DESTINATION bin)
endif()
