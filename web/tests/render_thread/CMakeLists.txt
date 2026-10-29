# [eggy] The render thread's browser test (EGGY.md, "The render thread"), with no game data:
# aurora built with -pthread, a GX program on the JSPI main thread (render_thread_test.cpp), the
# canvas and every WebGPU object on the render thread. web/tests/render_thread/run.sh builds and
# runs it (emsdk 6.0.12, Ninja, and Playwright's Chromium: npm i playwright@1.63.0).
#
#   emcmake cmake -S web/tests/render_thread -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_C_FLAGS=-pthread -DCMAKE_CXX_FLAGS=-pthread -DSDL_PTHREADS=ON
#   ninja -C build render_thread_test
cmake_minimum_required(VERSION 3.25)
project(render_thread_test C CXX)
set(CMAKE_CXX_STANDARD 20)

if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "The render thread test is a browser build: configure it with emcmake")
endif()
foreach(lang C CXX)
  if(NOT CMAKE_${lang}_FLAGS MATCHES "(^| )-pthread( |$)")
    message(FATAL_ERROR "Every object needs -pthread: configure with -DCMAKE_C_FLAGS=-pthread -DCMAKE_CXX_FLAGS=-pthread")
  endif()
endforeach()

set(repo "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
add_subdirectory("${repo}/aurora-main" aurora EXCLUDE_FROM_ALL)

add_executable(render_thread_test render_thread_test.cpp)
target_link_libraries(render_thread_test PRIVATE aurora::core aurora::gx)
set_target_properties(render_thread_test PROPERTIES SUFFIX ".html")
# As aurora's own objects (aurora-main/CMakeLists.txt): native wasm exceptions and longjmp.
target_compile_options(render_thread_test PRIVATE -fwasm-exceptions -sSUPPORT_LONGJMP=wasm)
# The game's link options for the render thread (runtime/cmake/PublicProducts.cmake).
target_link_options(render_thread_test PRIVATE
  -fwasm-exceptions -sSUPPORT_LONGJMP=wasm
  -sJSPI -sALLOW_MEMORY_GROWTH=1
  -pthread -sPTHREAD_POOL_SIZE=2 -sOFFSCREENCANVAS_SUPPORT
  "--js-library=${repo}/web/js/render/library_mkw_render.js")
set_property(TARGET render_thread_test APPEND PROPERTY LINK_DEPENDS "${repo}/web/js/render/library_mkw_render.js")
