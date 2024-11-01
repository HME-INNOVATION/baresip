find_path(LADSPA_INCLUDE_DIR
    NAME ladspa.h
    HINTS
        ${LADSPA_INCLUDE_DIRS}
    PATHS
        /usr/include
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LADSPA DEFAULT_MSG LADSPA_INCLUDE_DIR)

if(LADSPA_INCLUDE_DIR)
    set(LADSPA_INCLUDE_DIRS ${LADSPA_INCLUDE_DIR})
else()
    set(LADSPA_INCLUDE_DIRS "")
endif()

mark_as_advanced(LADSPA_INCLUDE_DIRS)
