# Build-local patch of JUCE's X11 backend (JUCE 8: AGPL-3.0 or commercial).
# The original JUCE source and its copyright/license remain intact.
# JUCE 8.0.15's Linux Xdnd receiver passes data.l[2] to XConvertSelection
# for every message. In XdndPosition this is packed screen coordinates, not a
# timestamp (the timestamp is data.l[3]); in XdndEnter it is a MIME atom. A
# selection owner that validates request times therefore refuses the hover
# request. XdndDrop has its timestamp in data.l[2].
#
# Patch a generated copy in the build directory, NEVER the user's JUCE tree.
# If the dependency changes, warn and leave it untouched; the strict external
# Xdnd integration test verifies whether the dependency needs an updated fix.
function(orogen_patch_juce_xdnd)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        return()
    endif()
    get_target_property(_sources juce_gui_basics INTERFACE_SOURCES)
    set(_main "")
    foreach(_source IN LISTS _sources)
        if(_source MATCHES "/juce_gui_basics/juce_gui_basics\\.cpp$")
            set(_main "${_source}")
        endif()
    endforeach()
    if(NOT _main)
        message(WARNING "OROGEN: cannot locate JUCE Xdnd source; run the strict native drop test")
        return()
    endif()
    get_filename_component(_module_dir "${_main}" DIRECTORY)
    set(_drag_source "${_module_dir}/native/juce_DragAndDrop_linux.cpp")
    if(NOT EXISTS "${_drag_source}")
        message(WARNING "OROGEN: JUCE Xdnd implementation not found; run the strict native drop test")
        return()
    endif()
    file(READ "${_drag_source}" _drag)
    file(READ "${_main}" _wrapper)
    string(REPLACE "\r\n" "\n" _drag "${_drag}")
    string(REPLACE "\r\n" "\n" _wrapper "${_wrapper}")
    set(_old_time "requestor, (::Time) clientMsg.data.l[2]);")
    set(_new_time "requestor, (::Time) (clientMsg.message_type == getAtoms().XdndPosition ? clientMsg.data.l[3] : (clientMsg.message_type == getAtoms().XdndDrop ? clientMsg.data.l[2] : CurrentTime)));")
    set(_old_callback "if (finishAfterDropDataReceived)\n                handleDragAndDropDataReceived();")
    set(_new_callback "if (finishAfterDropDataReceived)\n                handleDragAndDropDataReceived();\n            else if (! dragInfo.isEmpty())\n                if (auto* peer = getPeerFor (windowH))\n                    peer->handleDragMove (dragInfo);")
    set(_old_include "#include \"native/juce_DragAndDrop_linux.cpp\"")
    string(FIND "${_drag}" "${_old_time}" _has_time)
    string(FIND "${_drag}" "${_old_callback}" _has_callback)
    string(FIND "${_wrapper}" "${_old_include}" _has_include)
    if(_has_time LESS 0 OR _has_callback LESS 0 OR _has_include LESS 0)
        message(WARNING "OROGEN: JUCE Xdnd code changed; patch skipped, run the strict native drop test")
        return()
    endif()
    string(REPLACE "${_old_time}" "${_new_time}" _drag "${_drag}")
    string(REPLACE "${_old_callback}" "${_new_callback}" _drag "${_drag}")
    set(_patch_dir "${CMAKE_CURRENT_BINARY_DIR}/orogen-juce-xdnd")
    file(MAKE_DIRECTORY "${_patch_dir}")
    set(_patched_drag "${_patch_dir}/juce_DragAndDrop_linux.cpp")
    set(_patched_wrapper "${_patch_dir}/juce_gui_basics.cpp")
    string(REPLACE "${_old_include}" "#include \"${_patched_drag}\"" _wrapper "${_wrapper}")
    # configure_file copies only on content changes, so reconfiguring doesn't
    # force recompilation of this relatively large module every time.
    file(WRITE "${_patched_drag}.in" "${_drag}")
    file(WRITE "${_patched_wrapper}.in" "${_wrapper}")
    configure_file("${_patched_drag}.in" "${_patched_drag}" COPYONLY)
    configure_file("${_patched_wrapper}.in" "${_patched_wrapper}" COPYONLY)
    set(_patched_sources "")
    foreach(_source IN LISTS _sources)
        if(_source STREQUAL _main)
            list(APPEND _patched_sources "${_patched_wrapper}")
        else()
            list(APPEND _patched_sources "${_source}")
        endif()
    endforeach()
    set_property(TARGET juce_gui_basics PROPERTY INTERFACE_SOURCES "${_patched_sources}")
    # All other quoted includes in the generated wrapper refer to JUCE's
    # unchanged original module directory.
    set_source_files_properties("${_patched_wrapper}" PROPERTIES INCLUDE_DIRECTORIES "${_module_dir}")
    message(STATUS "OROGEN: using build-local JUCE Xdnd timestamp/hover fix")
endfunction()
