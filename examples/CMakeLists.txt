add_executable(iris-example-features features.c)
target_compile_features(iris-example-features PRIVATE c_std_11)
target_link_libraries(iris-example-features PRIVATE iris)
target_compile_options(iris-example-features PRIVATE -Wall -Wextra -Werror)

add_executable(iris-example-configure-and-acquire configure_and_acquire.cpp)
target_compile_features(iris-example-configure-and-acquire PRIVATE cxx_std_17)
target_link_libraries(iris-example-configure-and-acquire PRIVATE iris Threads::Threads)
target_compile_options(iris-example-configure-and-acquire PRIVATE -Wall -Wextra -Werror)

add_executable(iris-example-software-trigger software_trigger.c)
target_compile_features(iris-example-software-trigger PRIVATE c_std_11)
target_link_libraries(iris-example-software-trigger PRIVATE iris)
target_compile_options(iris-example-software-trigger PRIVATE -Wall -Wextra -Werror)
