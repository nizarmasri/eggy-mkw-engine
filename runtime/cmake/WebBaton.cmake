# [Eggy] The baton browser build (MKW_WEB_THREADS=baton): the game without JSPI, for Safari
# before iOS 27. See EGGY.md ("iOS 26: the baton build").
#
# Every guest thread is a pthread and exactly one holds the baton at a time
# (runtime/src/host_context.cpp); main() runs on a worker (-sPROXY_TO_PTHREAD) and the browser
# main thread keeps the canvas, WebGPU and SDL (runtime/src/platform/web/baton_main_thread.cpp,
# web/js/baton/library_mkw_baton.js). Everything must be compiled with -pthread (atomics and
# bulk memory), so the prebuilt web package is never used and SDL needs its pthread backend:
# web/build.sh passes -DCMAKE_C_FLAGS=-pthread -DCMAKE_CXX_FLAGS=-pthread -DSDL_PTHREADS=ON.
#
# Used by cmake/PublicProducts.cmake for the product, and by web/tests/baton/ for the test page.

# aurora's symbols that run on the browser main thread or wait differently without JSPI; each
# has a __wrap_ in baton_main_thread.cpp. The C++ ones are Itanium-mangled:
#   aurora::window::initialize(), create_window(AuroraBackend), destroy_window(), show_window(),
#   create_renderer(), aurora::imgui::initialize().
set(MKW_WEB_BATON_WRAPPED_SYMBOLS
    aurora_begin_frame
    aurora_end_frame
    aurora_update
    aurora_shutdown
    aurora_set_display_mode
    _ZN6aurora6window10initializeEv
    _ZN6aurora6window13create_windowE13AuroraBackend
    _ZN6aurora6window14destroy_windowEv
    _ZN6aurora6window11show_windowEv
    _ZN6aurora6window15create_rendererEv
    _ZN6aurora5imgui10initializeEv
    wgpuCreateInstance
    wgpuInstanceWaitAny
    emscripten_sleep
    # libc's stdio writes (the fd_write import): a worker's console output does not wait.
    __wasi_fd_write)

# Fails the configure early with the reason, instead of a link error about shared memory
# features or, worse, an SDL whose mutexes do nothing.
function(mkw_web_baton_check)
    if(NOT CMAKE_C_FLAGS MATCHES "(^| )-pthread( |$)" OR NOT CMAKE_CXX_FLAGS MATCHES "(^| )-pthread( |$)")
        message(FATAL_ERROR "MKW_WEB_THREADS=baton needs every object compiled with -pthread: "
            "configure with -DCMAKE_C_FLAGS=-pthread -DCMAKE_CXX_FLAGS=-pthread (web/build.sh does)")
    endif()
    if(NOT SDL_PTHREADS)
        message(FATAL_ERROR "MKW_WEB_THREADS=baton needs SDL's pthread backend: configure with -DSDL_PTHREADS=ON")
    endif()
    if(MKW_USE_WEB_PREBUILT)
        message(FATAL_ERROR "MKW_WEB_THREADS=baton cannot use the prebuilt web package (built without -pthread)")
    endif()
endfunction()

# Link options every baton module needs. `web_dir` is the repository's web/ directory. Call it
# after the other --js-library options: the baton library decorates their imports.
function(mkw_web_baton_link_options target web_dir)
    target_link_options(${target} PRIVATE
        -pthread
        # main() on a worker, so the guest may block; the browser main thread stays free.
        -sPROXY_TO_PTHREAD
        # Workers started before main() runs: main() itself and the first guest threads. More
        # are started on demand.
        -sPTHREAD_POOL_SIZE=8
        # A shared memory must declare its maximum; 2 GB keeps every pointer a positive JS
        # number and asks less of Safari than 4 GB.
        -sMAXIMUM_MEMORY=2GB)
    foreach(symbol IN LISTS MKW_WEB_BATON_WRAPPED_SYMBOLS)
        target_link_options(${target} PRIVATE "-Wl,--wrap=${symbol}")
    endforeach()
    target_link_options(${target} PRIVATE "--js-library=${web_dir}/js/baton/library_mkw_baton.js")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${web_dir}/js/baton/library_mkw_baton.js")
endfunction()
