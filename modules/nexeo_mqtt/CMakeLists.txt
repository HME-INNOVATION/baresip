project(nexeo_mqtt)

find_package(MOSQUITTO)
find_package(ZMS)

if(NOT MOSQUITTO_FOUND)
  return()
endif()

if(NOT ZMS_FOUND)
  return()
endif()

list(APPEND MODULES_DETECTED ${PROJECT_NAME})
set(MODULES_DETECTED ${MODULES_DETECTED} PARENT_SCOPE)

set(SRCS nexeo_mqtt.cpp NexeoMqttConnection.cpp NexeoMqttPublisher.cpp NexeoMqttSubscriber.cpp)

if(STATIC)
    add_library(${PROJECT_NAME} OBJECT ${SRCS})
else()
    add_library(${PROJECT_NAME} MODULE ${SRCS})
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${MOSQUITTO_INCLUDE_DIRS})
target_include_directories(${PROJECT_NAME} PRIVATE ${ZMS_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${MOSQUITTO_LIBRARY})
target_link_libraries(${PROJECT_NAME} PRIVATE ${ZMS_LIBRARIES})

