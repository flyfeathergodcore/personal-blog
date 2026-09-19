# CMake generated Testfile for 
# Source directory: /Users/Zhuanz1/Documents/github/vue-web/webcpp-engine
# Build directory: /Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/build-asan
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[runtime_test]=] "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/build-asan/runtime_test")
set_tests_properties([=[runtime_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;164;add_test;/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;0;")
add_test([=[net_test]=] "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/build-asan/net_test")
set_tests_properties([=[net_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;282;add_test;/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;0;")
add_test([=[log_test]=] "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/build-asan/log_test")
set_tests_properties([=[log_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;283;add_test;/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;0;")
add_test([=[router_test]=] "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/build-asan/router_test")
set_tests_properties([=[router_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;284;add_test;/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;0;")
add_test([=[flow_control_test]=] "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/build-asan/flow_control_test")
set_tests_properties([=[flow_control_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;285;add_test;/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;0;")
add_test([=[stream_manager_test]=] "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/build-asan/stream_manager_test")
set_tests_properties([=[stream_manager_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;286;add_test;/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;0;")
add_test([=[h2_frame_reader_test]=] "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/build-asan/h2_frame_reader_test")
set_tests_properties([=[h2_frame_reader_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;287;add_test;/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;0;")
add_test([=[h2_e2e_test]=] "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/build-asan/h2_e2e_test")
set_tests_properties([=[h2_e2e_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;288;add_test;/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;0;")
add_test([=[demo_smoke]=] "bash" "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/test/demo_smoke.sh" "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/build-asan/demo_server" "18443" "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/test/certs/server.crt" "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/test/certs/server.key")
set_tests_properties([=[demo_smoke]=] PROPERTIES  TIMEOUT "60" _BACKTRACE_TRIPLES "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;289;add_test;/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;0;")
add_test([=[tcp_smoke]=] "bash" "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/test/tcp_smoke.sh" "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/build-asan/tcp_server" "18082")
set_tests_properties([=[tcp_smoke]=] PROPERTIES  TIMEOUT "20" _BACKTRACE_TRIPLES "/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;295;add_test;/Users/Zhuanz1/Documents/github/vue-web/webcpp-engine/CMakeLists.txt;0;")
subdirs("coro")
