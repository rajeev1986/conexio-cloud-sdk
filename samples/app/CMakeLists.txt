cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app)

# Application only provides main.c — all SDK modules are compiled by the
# SDK Zephyr module CMakeLists.txt based on Kconfig selections.
target_sources(app PRIVATE
  src/main.c
)
