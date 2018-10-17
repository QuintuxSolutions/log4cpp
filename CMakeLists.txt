project(log4cpp)

file(GLOB _log4cpp_files src/*.cpp)
add_library(log4cpp SHARED ${_log4cpp_files})
add_definitions(-DLOG4CPP_HAS_DLL)
include_directories(include)
target_include_directories(${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(log4cpp ws2_32)
set_target_properties(log4cpp PROPERTIES DEFINE_SYMBOL "LOG4CPP_BUILD_DLL")
target_compile_definitions(log4cpp PUBLIC LOG4CPP_HAS_DLL)

install(FILES $<TARGET_FILE:log4cpp>
        DESTINATION bin
        COMPONENT core)
