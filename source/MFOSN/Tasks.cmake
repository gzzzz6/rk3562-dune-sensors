foreach(task Sensors/MFOSN Monitors/HostResources Monitors/Uptime Transports/Announce Transports/UDP Transports/Logging)
  dune_add_task(${PROJECT_SOURCE_DIR}/src ${task}/Task.cmake)
endforeach()
