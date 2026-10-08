file(GLOB_RECURSE core_sources "src/mc/*.cpp")
idf_component_register(SRCS ${core_sources} INCLUDE_DIRS "src" REQUIRES freertos)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
# Preserve cross-platform generator arithmetic and the existing S3 capacity limits.
# Keep the core's pre-existing bounded-string truncation diagnostics as warnings,
# as in the native build; do not change gameplay formatting during the migration.
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -ffp-contract=off
    -Wno-missing-field-initializers -Wno-error=format-truncation)
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    MC_MAX_PLAYERS=10 MC_MAX_VIEW_DISTANCE=32 MC_MAX_ENTITIES=128 MC_PACKET_SCRATCH=6144)
# The HTTP status dashboard (idf.py -D MC_DASHBOARD=ON ...); without it neither its code
# nor its page is in the firmware.
if(MC_DASHBOARD)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC MC_DASHBOARD=1)
endif()
