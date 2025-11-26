add_library(mtcreceiver mtcreceiver.cpp)

# Include directories
target_include_directories(mtcreceiver PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RTMIDI_INCLUDE_DIRS}
)

# Link RtMidi
if(RTMIDI_LIBRARIES)
    target_link_libraries(mtcreceiver ${RTMIDI_LIBRARIES})
endif()
