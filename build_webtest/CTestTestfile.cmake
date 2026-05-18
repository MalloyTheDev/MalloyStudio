# CMake generated Testfile for 
# Source directory: E:/MalloyStudio
# Build directory: E:/MalloyStudio/build_webtest
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test([=[MalloyStudioTests]=] "E:/MalloyStudio/build_webtest/Debug/MalloyStudioTests.exe")
  set_tests_properties([=[MalloyStudioTests]=] PROPERTIES  _BACKTRACE_TRIPLES "E:/MalloyStudio/CMakeLists.txt;146;add_test;E:/MalloyStudio/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test([=[MalloyStudioTests]=] "E:/MalloyStudio/build_webtest/Release/MalloyStudioTests.exe")
  set_tests_properties([=[MalloyStudioTests]=] PROPERTIES  _BACKTRACE_TRIPLES "E:/MalloyStudio/CMakeLists.txt;146;add_test;E:/MalloyStudio/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test([=[MalloyStudioTests]=] "E:/MalloyStudio/build_webtest/MinSizeRel/MalloyStudioTests.exe")
  set_tests_properties([=[MalloyStudioTests]=] PROPERTIES  _BACKTRACE_TRIPLES "E:/MalloyStudio/CMakeLists.txt;146;add_test;E:/MalloyStudio/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test([=[MalloyStudioTests]=] "E:/MalloyStudio/build_webtest/RelWithDebInfo/MalloyStudioTests.exe")
  set_tests_properties([=[MalloyStudioTests]=] PROPERTIES  _BACKTRACE_TRIPLES "E:/MalloyStudio/CMakeLists.txt;146;add_test;E:/MalloyStudio/CMakeLists.txt;0;")
else()
  add_test([=[MalloyStudioTests]=] NOT_AVAILABLE)
endif()
