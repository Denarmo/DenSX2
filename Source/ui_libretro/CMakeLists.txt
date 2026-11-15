cmake_minimum_required(VERSION 3.18)

set(CMAKE_MODULE_PATH
	${CMAKE_CURRENT_SOURCE_DIR}/../../deps/Dependencies/cmake-modules
	${CMAKE_MODULE_PATH}
)
include(Header)

project(Play_Libretro_Core)

add_definitions(-DPLAY_VERSION="${PROJECT_Version}")
#EmuDEN ships this core as DenSX2 (based on Play!)
add_definitions(-DDENSX2_VERSION="1.0")

if(NOT TARGET ui_shared)
	add_subdirectory(
		${CMAKE_CURRENT_SOURCE_DIR}/../ui_shared
		${CMAKE_CURRENT_BINARY_DIR}/ui_shared
	)
endif()
list(APPEND UI_LIBRETRO_PROJECT_LIBS ui_shared)

if(NOT TARGET PlayCore)
	add_subdirectory(
		${CMAKE_CURRENT_SOURCE_DIR}/../
		${CMAKE_CURRENT_BINARY_DIR}/Source
	)
endif()
list(APPEND UI_LIBRETRO_PROJECT_LIBS PlayCore)

if(NOT TARGET gsh_opengl)
	add_subdirectory(
		${CMAKE_CURRENT_SOURCE_DIR}/../gs/GSH_OpenGL
		${CMAKE_CURRENT_BINARY_DIR}/gs/GSH_OpenGL
	)
endif()
list(INSERT UI_LIBRETRO_PROJECT_LIBS 0 gsh_opengl)

#Vulkan GS for frontends that give a Vulkan device. Only the headers are needed: Vulkan is loaded
#at run time. Without them (no Vulkan SDK, no Vulkan_INCLUDE_DIR) the core is OpenGL only.
if(NOT TARGET_PLATFORM_IOS)
	if(NOT TARGET_PLATFORM_ANDROID AND NOT Vulkan_INCLUDE_DIR)
		find_package(Vulkan)
	endif()
	if(TARGET_PLATFORM_ANDROID OR Vulkan_INCLUDE_DIR)
		set(PLAY_LIBRETRO_VULKAN ON)
	endif()
endif()
if(PLAY_LIBRETRO_VULKAN)
	if(NOT TARGET gsh_vulkan)
		add_subdirectory(
			${CMAKE_CURRENT_SOURCE_DIR}/../gs/GSH_Vulkan
			${CMAKE_CURRENT_BINARY_DIR}/gs/GSH_Vulkan
		)
	endif()
	list(INSERT UI_LIBRETRO_PROJECT_LIBS 0 gsh_vulkan)
	message("Building the libretro core with the Vulkan GS.")
endif()

set(SRC
	main_libretro.cpp
	GSH_OpenGL_Libretro.cpp
	GSH_OpenGL_Libretro.h
	GSH_Vulkan_Libretro.cpp
	GSH_Vulkan_Libretro.h
	PH_Libretro_Input.cpp
	PH_Libretro_Input.h
	SH_LibreAudio.cpp
	SH_LibreAudio.h
)

if(TARGET_PLATFORM_ANDROID)
	list(APPEND UI_LIBRETRO_PROJECT_LIBS android log GLESv3 EGL)
elseif(TARGET_PLATFORM_IOS)
	list(APPEND UI_LIBRETRO_PROJECT_LIBS "-ObjC -lsqlite3 -framework OpenGLES")
elseif(TARGET_PLATFORM_UNIX)
	list(APPEND UI_LIBRETRO_PROJECT_LIBS "-static-libgcc")
	list(APPEND UI_LIBRETRO_PROJECT_LIBS "-static-libstdc++")
	#Keep the static runtime's symbols private: the frontend's own libstdc++ must not bind to them.
	list(APPEND UI_LIBRETRO_PROJECT_LIBS "-Wl,--exclude-libs,ALL")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 8 AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -lstdc++fs")
	list(APPEND UI_LIBRETRO_PROJECT_LIBS "libstdc++fs.a")
endif()

if(NOT PLAY_LIBRETRO_VULKAN)
	list(REMOVE_ITEM SRC GSH_Vulkan_Libretro.cpp GSH_Vulkan_Libretro.h)
endif()
add_library(play_libretro SHARED ${SRC})
if(PLAY_LIBRETRO_VULKAN)
	target_compile_definitions(play_libretro PRIVATE PLAY_LIBRETRO_VULKAN)
endif()
target_include_directories(play_libretro PRIVATE
	./
	./ext
	../../
	${CMAKE_CURRENT_BINARY_DIR}
)

if(TARGET_PLATFORM_ANDROID)
	set_target_properties(play_libretro PROPERTIES SUFFIX "_android.so")
endif()
if(TARGET_PLATFORM_IOS)
	if(IOS_PLATFORM STREQUAL "TVOS")
		set_target_properties(play_libretro PROPERTIES SUFFIX "_tvos.dylib")
	else()
		set_target_properties(play_libretro PROPERTIES SUFFIX "_ios.dylib")
	endif()
endif()

target_link_libraries(play_libretro ${UI_LIBRETRO_PROJECT_LIBS})
set_target_properties(play_libretro PROPERTIES PREFIX "")
