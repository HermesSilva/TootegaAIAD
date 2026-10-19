# Visual Studio solution layout (src/_build/TootegaAIAD.slnx): AIAD projects at the top, unit tests in
# "Tests", the dependency forks changed for AIAD in "ThirdParty".
include_guard(GLOBAL)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Puts every buildsystem target of DIR (recursively) in FOLDER, keeping a target's own sub-folder
# (Aurora's "Deployment" helpers stay apart from its libraries).
function(aiad_set_folder dir folder)
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(t IN LISTS targets)
        get_target_property(own ${t} FOLDER)
        if(own AND NOT own STREQUAL "Libraries")
            set_target_properties(${t} PROPERTIES FOLDER "${folder}/${own}")
        else()
            set_target_properties(${t} PROPERTIES FOLDER "${folder}")
        endif()
    endforeach()
    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(sub IN LISTS subdirs)
        aiad_set_folder("${sub}" "${folder}")
    endforeach()
endfunction()

# Executables named *_test go to "Tests".
function(aiad_set_test_folder dir)
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(t IN LISTS targets)
        if(t MATCHES "_test$")
            set_target_properties(${t} PROPERTIES FOLDER "Tests")
        endif()
    endforeach()
    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(sub IN LISTS subdirs)
        aiad_set_test_folder("${sub}")
    endforeach()
endfunction()
