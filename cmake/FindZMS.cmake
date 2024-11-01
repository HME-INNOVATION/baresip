find_path(ZMS_INCLUDE_DIR
  NAME ZMS.hpp
  HINTS
    ../BS7000-ZMSLib/inc
    ${PC_LIBZMS_INCLUDEDIR}
    ${PC_LIBZMS_INCLUDE_DIRS}
  PATHS /usr/include/zoltar/ZMSLib /usr/local/include/zoltar/ZMSLib
)

find_path(ZMSAPP_INCLUDE_DIR
  NAME Address.hpp
  HINTS
    ../BS7000-ZMSApp/inc
    ${PC_ZMSAPP_INCLUDEDIR}
    ${PC_ZMSAPP_INCLUDE_DIRS}
  PATHS /usr/include/zoltar/ZMSApp /usr/local/include/zoltar/ZMSApp
)

find_library(ZMS_LIBRARY
  NAMES zms libzms
  HINTS
    ../BS7000-ZMSLib
    ../BS7000-ZMSLib/build
    ../BS7000-ZMSLib/build/Debug
    ${PC_LIBZMS_LIBDIR}
    ${PC_LIBZMS_LIBRARY_DIRS}
  PATHS /usr/lib/zoltar /usr/local/lib/zoltar
)

include(FindPackageHandleStandardArgs)

if(ZMS_INCLUDE_DIR AND ZMSAPP_INCLUDE_DIR)
  set(ZMS_INCLUDE_DIRS ${ZMS_INCLUDE_DIR} ${ZMSAPP_INCLUDE_DIR})
  set(ZMS_LIBRARIES  ${ZMS_LIBRARY})
  set(ZMS_FOUND ON)
else()
  set(ZMS_INCLUDE_DIRS "")
  set(ZMS_LIBRARIES "")
  set(ZMS_FOUND OFF)
endif()

find_package_handle_standard_args(ZMS DEFAULT_MSG ZMS_FOUND)

mark_as_advanced(ZMS_LIBRARIES ZMS_INCLUDE_DIRS)

