cmake_minimum_required(VERSION 3.29)
project(QuestRewardReset LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(D2RPLUGIN_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../PluginSDK/SDK-0.4.0" CACHE PATH "Path to D2RLoader PluginSDK 0.4.0")

add_library(quest-reward-reset SHARED src/plugin.cpp src/installer.cpp src/plugin.rc)
target_include_directories(quest-reward-reset PRIVATE "${D2RPLUGIN_SDK_ROOT}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(quest-reward-reset PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(quest-reward-reset PRIVATE
	"$<$<COMPILE_LANGUAGE:CXX>:/W4>"
	"$<$<COMPILE_LANGUAGE:CXX>:/permissive->"
	"$<$<COMPILE_LANGUAGE:CXX>:/EHsc>"
	"$<$<COMPILE_LANGUAGE:CXX>:/utf-8>"
)
target_link_options(quest-reward-reset PRIVATE /INCREMENTAL:NO /Brepro /MACHINE:X64)
target_link_libraries(quest-reward-reset PRIVATE user32)
set_target_properties(quest-reward-reset PROPERTIES PREFIX "" OUTPUT_NAME "d2rl-quest-reward-reset")
