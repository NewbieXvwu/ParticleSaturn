# Install script for directory: E:/Users/User/Desktop/ParticleSaturn/libs/DiligentCore/Graphics/GraphicsEngineD3D12

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/install")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Debug")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/libs/DiligentCore/Debug" TYPE STATIC_LIBRARY OPTIONAL FILES "E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/Graphics/GraphicsEngineD3D12/GraphicsEngineD3D12_64d.lib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin/libs/DiligentCore/Debug" TYPE SHARED_LIBRARY FILES "E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/Graphics/GraphicsEngineD3D12/GraphicsEngineD3D12_64d.dll")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/libs/DiligentCore/Graphics/GraphicsEngineD3D12/" TYPE DIRECTORY FILES "E:/Users/User/Desktop/ParticleSaturn/libs/DiligentCore/Graphics/GraphicsEngineD3D12/interface")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/libs/DiligentCore/Graphics/GraphicsEngineD3D12/" TYPE DIRECTORY FILES "E:/Users/User/Desktop/ParticleSaturn/libs/DiligentCore/Graphics/GraphicsEngineD3D12/interface")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/Graphics/GraphicsEngineD3D12/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
