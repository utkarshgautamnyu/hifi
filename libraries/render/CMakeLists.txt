set(TARGET_NAME render)
AUTOSCRIBE_SHADER_LIB(gpu model)
setup_hifi_library()
#CLIMAX_MERGE_START
link_hifi_libraries(shared ktx gpu model octree)
#CLIMAX_MERGE_END
# render needs octree only for getAccuracyAngle(float, int)
include_hifi_library_headers(octree)

target_nsight()
