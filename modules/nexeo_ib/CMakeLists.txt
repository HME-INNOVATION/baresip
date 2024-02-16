project(nexeo_ib)

find_package(GST)

if(NOT GST_FOUND)
  return()
endif()

list(APPEND MODULES_DETECTED ${PROJECT_NAME})
set(MODULES_DETECTED ${MODULES_DETECTED} PARENT_SCOPE)

set(SRCS nexeo_ib.c nexeo_ib_inbound.c nexeo_ib_outbound.c)

if(STATIC)
    add_library(${PROJECT_NAME} OBJECT ${SRCS})
else()
    add_library(${PROJECT_NAME} MODULE ${SRCS})
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${GST_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${GST_LIBRARIES})
target_link_libraries(${PROJECT_NAME} PRIVATE gstapp-1.0 )
target_link_libraries(${PROJECT_NAME} PRIVATE gobject-2.0 )
target_link_libraries(${PROJECT_NAME} PRIVATE glib-2.0 )
target_link_libraries(${PROJECT_NAME} PRIVATE gstreamer-1.0 )
target_link_libraries(${PROJECT_NAME} PRIVATE gstaudio-1.0 )
target_compile_options(${PROJECT_NAME} PRIVATE -Wno-cast-align)

