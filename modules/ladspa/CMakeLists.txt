project(ladspa)

find_package(LADSPA)

if(NOT LADSPA_FOUND)
  return()
endif()

list(APPEND MODULES_DETECTED ${PROJECT_NAME})
set(MODULES_DETECTED ${MODULES_DETECTED} PARENT_SCOPE)

set(SRCS ladspa_filter.cpp)

if(STATIC)
    add_library(${PROJECT_NAME} OBJECT ${SRCS})
else()
    add_library(${PROJECT_NAME} MODULE ${SRCS})
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${LADSPA_INCLUDE_DIRS})
#target_compile_options(${PROJECT_NAME} PRIVATE
#  -isystem $(LADSPA_INCLUDE_DIRS)
#  -fPIC
#  -Wno-shadow
#  -Wno-pedantic
#  -Wno-missing-field-initializers
#  -Wno-unused-parameter
#  -DBOOST_NO_CXX11_SCOPED_ENUMS
#)
target_link_libraries(${PROJECT_NAME} PRIVATE boost_system)
target_link_libraries(${PROJECT_NAME} PRIVATE boost_filesystem)
target_link_libraries(${PROJECT_NAME} PRIVATE stdc++fs)
#target_link_options(${PROJECT_NAME} PUBLIC -lstdc++)
