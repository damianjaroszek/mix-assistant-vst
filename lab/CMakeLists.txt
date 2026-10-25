# Offline lab: runs the same analysis as the plugins on WAV stems exported from FL.
add_executable(ma-lab main.cpp)
target_link_libraries(ma-lab PRIVATE ma_core ma_io)
