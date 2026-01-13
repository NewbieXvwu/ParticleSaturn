# Install script for directory: E:/Users/User/Desktop/ParticleSaturn/libs/DiligentCore/ThirdParty

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
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/Licenses/ThirdParty/libs/DiligentCore" TYPE FILE RENAME "Vulkan-Headers-License.md" FILES "E:/Users/User/Desktop/ParticleSaturn/libs/DiligentCore/ThirdParty/Vulkan-Headers/LICENSE.md")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/Licenses/ThirdParty/libs/DiligentCore" TYPE FILE RENAME "SPIRV-Headers-License.txt" FILES "E:/Users/User/Desktop/ParticleSaturn/libs/DiligentCore/ThirdParty/SPIRV-Headers/LICENSE")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/Licenses/ThirdParty/libs/DiligentCore" TYPE FILE RENAME "SPIRV-Tools-License.txt" FILES "E:/Users/User/Desktop/ParticleSaturn/libs/DiligentCore/ThirdParty/SPIRV-Tools/LICENSE")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/Licenses/ThirdParty/libs/DiligentCore" TYPE FILE RENAME "GLSLang-License.txt" FILES "E:/Users/User/Desktop/ParticleSaturn/libs/DiligentCore/ThirdParty/glslang/LICENSE.txt")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/Licenses/ThirdParty/libs/DiligentCore" TYPE FILE RENAME "SPIRV-Cross-License.txt" FILES "E:/Users/User/Desktop/ParticleSaturn/libs/DiligentCore/ThirdParty/SPIRV-Cross/LICENSE")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/Licenses/ThirdParty/libs/DiligentCore" TYPE FILE RENAME "Volk-License.md" FILES "E:/Users/User/Desktop/ParticleSaturn/libs/DiligentCore/ThirdParty/volk/LICENSE.md")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/Licenses/ThirdParty/libs/DiligentCore" TYPE FILE RENAME "xxHash-License.txt" FILES "E:/Users/User/Desktop/ParticleSaturn/libs/DiligentCore/ThirdParty/xxHash/build/cmake/../../LICENSE")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/libs/DiligentCore/Debug" TYPE STATIC_LIBRARY FILES "E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/ThirdParty/SPIRV-Tools/source/SPIRV-Tools.lib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/libs/DiligentCore/Debug" TYPE STATIC_LIBRARY FILES "E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/ThirdParty/SPIRV-Tools/source/opt/SPIRV-Tools-opt.lib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/libs/DiligentCore/Debug" TYPE STATIC_LIBRARY FILES "E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/ThirdParty/glslang/glslang/GenericCodeGend.lib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/libs/DiligentCore/Debug" TYPE STATIC_LIBRARY FILES "E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/ThirdParty/glslang/glslang/glslangd.lib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/libs/DiligentCore/Debug" TYPE STATIC_LIBRARY FILES "E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/ThirdParty/glslang/glslang/OSDependent/Windows/OSDependentd.lib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/libs/DiligentCore/Debug" TYPE STATIC_LIBRARY FILES "E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/ThirdParty/glslang/SPIRV/SPIRVd.lib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/libs/DiligentCore/Debug" TYPE STATIC_LIBRARY FILES "E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/ThirdParty/glslang/glslang/MachineIndependentd.lib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/libs/DiligentCore/Debug" TYPE STATIC_LIBRARY FILES "E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/ThirdParty/SPIRV-Cross/spirv-cross-cored.lib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/libs/DiligentCore/Debug" TYPE STATIC_LIBRARY FILES "E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/ThirdParty/volk/volk.lib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/libs/DiligentCore/Debug" TYPE STATIC_LIBRARY FILES "E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/ThirdParty/xxHash/build/cmake/xxhash.lib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/Licenses/ThirdParty/libs/DiligentCore" TYPE FILE FILES "E:/Users/User/Desktop/ParticleSaturn/libs/DiligentCore/ThirdParty/stb/stb_image_write_license.txt")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/Licenses/ThirdParty/libs/DiligentCore" TYPE FILE RENAME "DXC-License.txt" FILES "E:/Users/User/Desktop/ParticleSaturn/libs/DiligentCore/ThirdParty/DirectXShaderCompiler/LICENSE.TXT")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/Licenses/ThirdParty/libs/DiligentCore" TYPE FILE RENAME "DXC-ThirdPartyNotices.txt" FILES "E:/Users/User/Desktop/ParticleSaturn/libs/DiligentCore/ThirdParty/DirectXShaderCompiler/ThirdPartyNotices.txt")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/ThirdParty/SPIRV-Tools/cmake_install.cmake")
  include("E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/ThirdParty/glslang/cmake_install.cmake")
  include("E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/ThirdParty/SPIRV-Cross/cmake_install.cmake")
  include("E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/ThirdParty/volk/cmake_install.cmake")
  include("E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/ThirdParty/xxHash/build/cmake/cmake_install.cmake")

endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "E:/Users/User/Desktop/ParticleSaturn/build_diligent_ninja/libs/DiligentCore/ThirdParty/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
