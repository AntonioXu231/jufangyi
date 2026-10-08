cmake_minimum_required(VERSION 3.21)
project(pd_scope_prpd_qt6 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Network)

qt_add_executable(pd_scope_prpd_qt6
    main.cpp
    scope_types.h
    demo_source.h demo_source.cpp
    zynq_scope_source.h zynq_scope_source.cpp
    waveform_widget.h waveform_widget.cpp
    phase_ellipse_widget.h phase_ellipse_widget.cpp
    channel_panel.h channel_panel.cpp
    main_window.h main_window.cpp
    event_table_model.h event_table_model.cpp
    fft_widget.h fft_widget.cpp
    single_channel_page.h single_channel_page.cpp
)

target_link_libraries(pd_scope_prpd_qt6 PRIVATE Qt6::Widgets Qt6::Network)
