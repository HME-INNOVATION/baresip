find_path(BOOST_INCLUDE_DIR
  NAME boost/version.hpp
  PATHS /usr/include
)

find_library(BOOST_LIBRARY_SYSTEM
  NAMES boost_system
  PATHS /lib/x86_64-linux-gnu ${BOOST_LIBRARY_DIR}
)

find_library(BOOST_LIBRARY_THREAD
  NAMES boost_thread
  PATHS /lib/x86_64-linux-gnu ${BOOST_LIBRARY_DIR}
)

include(FindPackageHandleStandardArgs)

if(BOOST_INCLUDE_DIR AND BOOST_LIBRARY_SYSTEM AND BOOST_LIBRARY_THREAD)
  set(BOOST_INCLUDE_DIRS ${BOOST_INCLUDE_DIR})
  set(BOOST_LIBRARIES  ${BOOST_LIBRARY_SYSTEM} ${BOOST_LIBRARY_THREAD})
  set(BOOST_FOUND ON)
else()
  set(BOOST_INCLUDE_DIRS "")
  set(BOOST_LIBRARIES "")
  set(BOOST_FOUND OFF)
endif()

find_package_handle_standard_args(BOOST DEFAULT_MSG BOOST_FOUND)

mark_as_advanced(BOOST_LIBRARIES BOOST_INCLUDE_DIRS)

