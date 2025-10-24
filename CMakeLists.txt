cmake_minimum_required(VERSION 3.15)
project(median_filter C)

# Параметры
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(ENABLE_SANITIZERS "Enable Address/UndefinedBehavior sanitizers (only GCC/Clang)" OFF)

# Исполняемый файл (подправьте путь, если ваш источник не в src/)
add_executable(median_filter src/main.c)

# Включаем полезные warning'и
if (MSVC)
    target_compile_options(median_filter PRIVATE /W4 /permissive-)
else()
    target_compile_options(median_filter PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Подключаем реализацию потоков (Threads::Threads корректно работает и на Unix, и на Windows (MSYS/pthread))
find_package(Threads REQUIRED)
target_link_libraries(median_filter PRIVATE Threads::Threads)

# Опциональные санитайзеры (только для GCC/Clang)
if (ENABLE_SANITIZERS)
    if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(median_filter PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(median_filter PRIVATE -fsanitize=address,undefined)
    else()
        message(WARNING "Sanitizers requested but unsupported for this compiler")
    endif()
endif()

# Установка
install(TARGETS median_filter
        RUNTIME DESTINATION bin)

# Простейший тест / example через CTest
enable_testing()

# Создадим примерный входной файл в каталоге сборки
set(TEST_INPUT ${CMAKE_BINARY_DIR}/example_in.txt)
set(TEST_OUTPUT ${CMAKE_BINARY_DIR}/example_out.txt)
file(WRITE ${TEST_INPUT}
"4 5
1 2 3 4 5
6 7 8 9 10
11 12 13 14 15
16 17 18 19 20
")

add_test(NAME run_example
         COMMAND $<TARGET_FILE:median_filter> -m 2 -k 1 -w 3 -i ${TEST_INPUT} -o ${TEST_OUTPUT})
set_tests_properties(run_example PROPERTIES PASS_REGULAR_EXPRESSION "4 5")

# Удобные переменные для packaging / cpack (по желанию)
include(GNUInstallDirs)