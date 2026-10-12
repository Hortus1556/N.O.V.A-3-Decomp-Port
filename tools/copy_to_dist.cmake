# After a build: refresh nova3.exe in the packaged folder (tools/package.py), if there is one.
# cmake -DSRC=<nova3.exe> -DDIST=<folder> -P copy_to_dist.cmake
if(EXISTS "${DIST}/nova3_data")
    file(COPY "${SRC}" DESTINATION "${DIST}")
endif()
