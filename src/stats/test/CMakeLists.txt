set(MY_PROJ_NAME dsn.stats.test)

set(MY_SRC_SEARCH_MODE "GLOB")

set(MY_PROJ_LIBS
        dsn.stats
        gtest
        )

dsn_add_executable()
