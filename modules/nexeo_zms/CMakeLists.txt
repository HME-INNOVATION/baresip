project(nexeo_zms)

find_package(GST)
find_package(ZMS)

if(NOT GST_FOUND)
  return()
endif()

if(NOT ZMS_FOUND)
  return()
endif()

list(APPEND MODULES_DETECTED ${PROJECT_NAME})
set(MODULES_DETECTED ${MODULES_DETECTED} PARENT_SCOPE)

set(SRCS nexeo_zms.cpp nexeo_zms_inbound.cpp nexeo_zms_outbound.cpp)

if(STATIC)
    add_library(${PROJECT_NAME} OBJECT ${SRCS})
else()
    add_library(${PROJECT_NAME} MODULE ${SRCS})
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${GST_INCLUDE_DIRS})
target_include_directories(${PROJECT_NAME} PRIVATE ${ZMS_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${GST_LIBRARIES})
target_link_libraries(${PROJECT_NAME} PRIVATE ${ZMS_LIBRARIES})
target_link_libraries(${PROJECT_NAME} PRIVATE gstapp-1.0 )
target_link_libraries(${PROJECT_NAME} PRIVATE gobject-2.0 )
target_link_libraries(${PROJECT_NAME} PRIVATE glib-2.0 )
target_link_libraries(${PROJECT_NAME} PRIVATE gstreamer-1.0 )
target_link_libraries(${PROJECT_NAME} PRIVATE gstaudio-1.0 )
target_compile_options(${PROJECT_NAME} PRIVATE -Wno-cast-align)

