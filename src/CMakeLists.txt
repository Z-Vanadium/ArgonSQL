# recursive files
FILE(GLOB_RECURSE MAIN_SOURCES ${PROJECT_SOURCE_DIR}/src/*/*.cpp
        ${PROJECT_SOURCE_DIR}/src/*/*/*.cpp
        ${PROJECT_SOURCE_DIR}/src/*/*/*/*.cpp
        ${PROJECT_SOURCE_DIR}/src/*/*.c
        ${PROJECT_SOURCE_DIR}/src/*/*/*.c
        )
MESSAGE(STATUS "Source file lists: ${MAIN_SOURCES}")
ADD_LIBRARY(zSql SHARED ${MAIN_SOURCES})
TARGET_LINK_LIBRARIES(zSql glog)

ADD_EXECUTABLE(argonsql_server main.cpp)
TARGET_LINK_LIBRARIES(argonsql_server glog zSql pthread)

# 独立客户端使用与 Server 相同的协议库，但不依赖 Server 的进程入口。
ADD_EXECUTABLE(argonsql_client client_main.cpp)
TARGET_LINK_LIBRARIES(argonsql_client glog zSql pthread)
