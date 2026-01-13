# Install script for directory: E:/Users/User/Desktop/ParticleSaturn/libs/DiligentCore/Graphics

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

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/Graphics/GraphicsAccessories/cmake_install.cmake")
  include("E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/Graphics/GraphicsEngine/cmake_install.cmake")
  include("E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/Graphics/HLSL2GLSLConverterLib/cmake_install.cmake")
  include("E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/Graphics/ShaderTools/cmake_install.cmake")
  include("E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/Graphics/GraphicsEngineNextGenBase/cmake_install.cmake")
  include("E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/Graphics/GraphicsEngineD3DBase/cmake_install.cmake")
  include("E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/Graphics/GraphicsEngineD3D12/cmake_install.cmake")
  include("E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/Graphics/GraphicsEngineVulkan/cmake_install.cmake")
  include("E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/Graphics/GraphicsTools/cmake_install.cmake")

endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/Graphics/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
