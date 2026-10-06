# Merges static libraries into one with GNU ar (N64Recomp.cmake, live mode off Windows):
#   cmake -DOUT=<lib> -DMRI=<script> -DPARTS=<lib;lib;...> -DAR=<ar> -P N64RecompMerge.cmake
set(script "create ${OUT}\n")
foreach(part ${PARTS})
    string(APPEND script "addlib ${part}\n")
endforeach()
string(APPEND script "save\nend\n")
file(WRITE "${MRI}" "${script}")
execute_process(COMMAND "${AR}" -M INPUT_FILE "${MRI}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "ar -M failed (${result})")
endif()
