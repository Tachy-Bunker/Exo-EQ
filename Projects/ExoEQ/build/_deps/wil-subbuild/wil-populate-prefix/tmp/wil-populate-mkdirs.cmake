# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "F:/Work/Exomusica/Tools/ExoEQV1/Projects/ExoEQ/build/_deps/wil-src")
  file(MAKE_DIRECTORY "F:/Work/Exomusica/Tools/ExoEQV1/Projects/ExoEQ/build/_deps/wil-src")
endif()
file(MAKE_DIRECTORY
  "F:/Work/Exomusica/Tools/ExoEQV1/Projects/ExoEQ/build/_deps/wil-build"
  "F:/Work/Exomusica/Tools/ExoEQV1/Projects/ExoEQ/build/_deps/wil-subbuild/wil-populate-prefix"
  "F:/Work/Exomusica/Tools/ExoEQV1/Projects/ExoEQ/build/_deps/wil-subbuild/wil-populate-prefix/tmp"
  "F:/Work/Exomusica/Tools/ExoEQV1/Projects/ExoEQ/build/_deps/wil-subbuild/wil-populate-prefix/src/wil-populate-stamp"
  "F:/Work/Exomusica/Tools/ExoEQV1/Projects/ExoEQ/build/_deps/wil-subbuild/wil-populate-prefix/src"
  "F:/Work/Exomusica/Tools/ExoEQV1/Projects/ExoEQ/build/_deps/wil-subbuild/wil-populate-prefix/src/wil-populate-stamp"
)

set(configSubDirs Debug)
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "F:/Work/Exomusica/Tools/ExoEQV1/Projects/ExoEQ/build/_deps/wil-subbuild/wil-populate-prefix/src/wil-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "F:/Work/Exomusica/Tools/ExoEQV1/Projects/ExoEQ/build/_deps/wil-subbuild/wil-populate-prefix/src/wil-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
