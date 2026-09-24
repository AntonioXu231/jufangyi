cmake_minimum_required(VERSION 3.21)
project(pd_acquisition_desktop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Network)

# 把"协议解析 / 频谱 / 示波器控件 / 取帧调度"放进一个静态库，
# 让 GUI 与离线自检共用同一份实现，避免两边解析口径发散。
qt_add_library(pd_host_core STATIC
    pd_tcp_client.cpp
    pd_tcp_client.h
    pd_sample_codec.cpp
    pd_sample_codec.h
    pd_reply_parse.cpp
    pd_reply_parse.h
    pd_pulse_detect.cpp
    pd_pulse_detect.h
    ellipse_panel.cpp
    ellipse_panel.h
    pd_analysis_worker.cpp
    pd_analysis_worker.h
    fft_engine.cpp
    fft_engine.h
    scope_widget.cpp
    scope_widget.h
    scope_stream.cpp
    scope_stream.h
    plot_widget.cpp
    plot_widget.h
    prpd_panel.cpp
    prpd_panel.h
)
target_include_directories(pd_host_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pd_host_core PUBLIC Qt6::Widgets Qt6::Network)

qt_add_executable(pd_acquisition_desktop
    main.cpp
    main_window.cpp
    main_window.h
)
target_link_libraries(pd_acquisition_desktop PRIVATE pd_host_core)

set_target_properties(pd_acquisition_desktop PROPERTIES
    WIN32_EXECUTABLE TRUE
)

# 离线自检：控制台程序，不需要板端，失败时返回非零退出码。
qt_add_executable(pd_host_selftest selftest.cpp)
target_link_libraries(pd_host_selftest PRIVATE pd_host_core)
set_target_properties(pd_host_selftest PROPERTIES
    WIN32_EXECUTABLE FALSE
)
