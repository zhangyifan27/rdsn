set(MY_PROJ_NAME dsn.stats)

set(MY_SRC_SEARCH_MODE "GLOB")

add_subdirectory(test)

dsn_add_static_library()
