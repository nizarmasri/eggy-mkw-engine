# The baton build's machinery without the game (EGGY.md, "iOS 26: the baton build"): aurora,
# SDL and everything else compiled with -pthread, the pthread HostContext backend, aurora's
# main-thread split and the baton JS library, linked with the same options as the product
# (runtime/cmake/WebBaton.cmake). Built and run by web/tests/baton/run_baton_test.sh.
#
#   emcmake cmake -S web/tests/baton -B BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release \
#       -DCMAKE_C_FLAGS=-pthread -DCMAKE_CXX_FLAGS=-pthread -DSDL_PTHREADS=ON
#   cmake --build BUILD --target baton_test       # -> BUILD/baton_test.html/.js/.wasm
cmake_minimum_required(VERSION 3.25)
project(mkw_baton_test LANGUAGES C CXX)
if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Configure this with emcmake (it is a browser test)")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(MKW_REPO "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

# As runtime/CMakeLists.txt builds aurora for the web (native wasm exceptions everywhere).
add_compile_options(-fwasm-exceptions -sSUPPORT_LONGJMP=wasm)
add_link_options(-fwasm-exceptions -sSUPPORT_LONGJMP=wasm)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
add_subdirectory("${MKW_REPO}/aurora-main" aurora-build EXCLUDE_FROM_ALL)
set(CMAKE_DISABLE_PRECOMPILE_HEADERS OFF)

set(MKW_USE_WEB_PREBUILT FALSE)
include("${MKW_REPO}/runtime/cmake/WebBaton.cmake")
mkw_web_baton_check()

add_executable(baton_test
    baton_test.cpp
    "${MKW_REPO}/runtime/src/host_context.cpp"
    "${MKW_REPO}/runtime/src/platform/web/baton_main_thread.cpp")
target_include_directories(baton_test PRIVATE "${MKW_REPO}/runtime/include")
target_link_libraries(baton_test PRIVATE aurora::core aurora::gx)
# Emscripten's default shell page (it has <canvas id="canvas">).
set_target_properties(baton_test PROPERTIES SUFFIX ".html")
target_link_options(baton_test PRIVATE
    -sENVIRONMENT=web
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=64MB
    -sSTACK_SIZE=1MB
    "--js-library=${MKW_REPO}/web/js/library_mkw_host.js"
    "--js-library=${CMAKE_CURRENT_LIST_DIR}/library_baton_test.js")
mkw_web_baton_link_options(baton_test "${MKW_REPO}/web")
