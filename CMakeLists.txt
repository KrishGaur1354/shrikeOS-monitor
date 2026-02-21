cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(shrike_monitor)

target_sources(app PRIVATE
  src/main.c
  src/watchdog.c
  src/sysinfo.c
  src/command.c
  src/logger.c
)
