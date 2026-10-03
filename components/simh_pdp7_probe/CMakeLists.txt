get_filename_component(SIMHP4_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(SIMH_DIR "${SIMHP4_ROOT}/third_party/simh")

idf_component_register(
    SRCS
        "probe_anchor.c"
        "simhp4_machine.c"
        "simhp4_runtime.c"
        "simhp4_media.c"
        "simhp4_g2_local.c"
        "simhp4_type340_surface.c"
        "simhp4_rb09_persist.c"
        "simhp4_st_migration.c"
        "simhp4_btools_migration.c"
        "simhp4_graphics2_core.c"
        "simhp4_graphics2_bridge.c"
    INCLUDE_DIRS
        "."
        "${SIMH_DIR}"
        "${SIMH_DIR}/PDP18B"
        "${SIMH_DIR}/display"
    EMBED_FILES
        "assets/image.fs"
        "assets/boot.rim"
    REQUIRES
        freertos
        heap
        simhp4_remote_tty
)

if(CMAKE_BUILD_EARLY_EXPANSION)
    return()
endif()

if(NOT EXISTS "${SIMH_DIR}/PDP18B/pdp18b_cpu.c")
    message(FATAL_ERROR "SIMHP4 R0A1F: Open SIMH source missing at ${SIMH_DIR}")
endif()

set(SIMHP4_PDP7_MIN_MACHINE_SOURCES
    "${SIMH_DIR}/PDP18B/pdp18b_cpu.c"
    "${SIMH_DIR}/PDP18B/pdp18b_stddev.c"
    "${SIMH_DIR}/PDP18B/pdp18b_rb.c"
    "${SIMH_DIR}/PDP18B/pdp18b_g2tty.c"
    "${SIMH_DIR}/display/type340.c"
    "${CMAKE_CURRENT_LIST_DIR}/simhp4_type340_bridge.c"
)

add_library(simhp4_pdp7_min_machine OBJECT ${SIMHP4_PDP7_MIN_MACHINE_SOURCES})
set_target_properties(simhp4_pdp7_min_machine PROPERTIES C_STANDARD 99)
target_include_directories(simhp4_pdp7_min_machine PRIVATE
    "${SIMH_DIR}"
    "${SIMH_DIR}/PDP18B"
    "${SIMH_DIR}/display"
    "${SIMHP4_ROOT}/components/simhp4_remote_tty/include"
)
target_compile_definitions(simhp4_pdp7_min_machine PRIVATE
    PDP7=1
    NOT_MUX_USING_CODE=1
    SIM_BUILD_TOOL=RetroP4_ESP_IDF_R0A5
)
target_compile_options(simhp4_pdp7_min_machine PRIVATE
    -Wno-error=all
    -Wno-error=format
    -Wno-error=unused-value
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-parameter
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PDP7=1
    NOT_MUX_USING_CODE=1
    SIM_BUILD_TOOL=RetroP4_ESP_IDF_R0A3
)

target_sources(${COMPONENT_LIB} PRIVATE $<TARGET_OBJECTS:simhp4_pdp7_min_machine>)
add_dependencies(${COMPONENT_LIB} simhp4_pdp7_min_machine)

message(STATUS "SIMHP4 R0A5: persistent interactive UNIX V0 G2 shell")

# SIMHP4_VAX630_COMPILE_PROBE
#
# Compile-only MicroVAX II probe. This target is EXCLUDE_FROM_ALL and is never
# linked into the PDP-7 application. PDP-7 and VAX both export simulator-global
# names such as sim_devices, sim_name and cpu_unit, so the machines remain
# isolated at this stage.
#
# The VAX probe deliberately uses a separate exact Open SIMH tree:
#   third_party/simh_vax630_87eb
# This avoids mixing the accepted PDP-7 GRAPHIC-II source lineage with the VAX
# source revision.
set(SIMHP4_VAX_SIMH_DIR
    "${SIMHP4_ROOT}/third_party/simh_vax630_87eb")

set(SIMHP4_VAX630_PROBE_SOURCES
    "${SIMHP4_VAX_SIMH_DIR}/VAX/vax_cpu.c"
    "${SIMHP4_VAX_SIMH_DIR}/VAX/vax_cpu1.c"
    "${SIMHP4_VAX_SIMH_DIR}/VAX/vax_fpa.c"
    "${SIMHP4_VAX_SIMH_DIR}/VAX/vax_cis.c"
    "${SIMHP4_VAX_SIMH_DIR}/VAX/vax_octa.c"
    "${SIMHP4_VAX_SIMH_DIR}/VAX/vax_cmode.c"
    "${SIMHP4_VAX_SIMH_DIR}/VAX/vax_mmu.c"
    "${SIMHP4_VAX_SIMH_DIR}/VAX/vax_sys.c"
    "${SIMHP4_VAX_SIMH_DIR}/VAX/vax_syscm.c"
    "${SIMHP4_VAX_SIMH_DIR}/VAX/vax_watch.c"
    "${SIMHP4_VAX_SIMH_DIR}/VAX/vax630_stddev.c"
    "${SIMHP4_VAX_SIMH_DIR}/VAX/vax630_sysdev.c"
    "${SIMHP4_VAX_SIMH_DIR}/VAX/vax630_io.c"
    "${SIMHP4_VAX_SIMH_DIR}/VAX/vax630_syslist.c"
    "${SIMHP4_VAX_SIMH_DIR}/PDP11/pdp11_rq.c"
)

set(SIMHP4_VAX630_PROBE_MISSING "")
foreach(_src IN LISTS SIMHP4_VAX630_PROBE_SOURCES)
    if(NOT EXISTS "${_src}")
        list(APPEND SIMHP4_VAX630_PROBE_MISSING "${_src}")
    endif()
endforeach()

if(SIMHP4_VAX630_PROBE_MISSING)
    message(STATUS
        "SIMHP4 VAX630 compile probe disabled; run the V0 vendor installer first")
else()
    add_library(simhp4_vax630_compile_objs OBJECT EXCLUDE_FROM_ALL
        ${SIMHP4_VAX630_PROBE_SOURCES})
    set_target_properties(simhp4_vax630_compile_objs PROPERTIES C_STANDARD 99)
    target_include_directories(simhp4_vax630_compile_objs PRIVATE
        "${SIMHP4_VAX_SIMH_DIR}"
        "${SIMHP4_VAX_SIMH_DIR}/VAX"
        "${SIMHP4_VAX_SIMH_DIR}/PDP11"
    )
    target_compile_definitions(simhp4_vax630_compile_objs PRIVATE
        VM_VAX=1
        VAX_630=1
        USE_INT64=1
        USE_ADDR64=1
        SIM_BUILD_TOOL=RetroP4_ESP_IDF_V0
    )
    target_compile_options(simhp4_vax630_compile_objs PRIVATE
        -Wno-error=all
        -Wno-error=format
        -Wno-unused-function
        -Wno-unused-variable
        -Wno-unused-parameter
    )
    add_custom_target(simhp4_vax630_compile_probe
        DEPENDS simhp4_vax630_compile_objs)
    message(STATUS
        "SIMHP4 V0: MicroVAX II VAX_630 compile-only probe target enabled")
endif()
