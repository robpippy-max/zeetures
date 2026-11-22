cmake_minimum_required(VERSION 3.5)
project(openc2e)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(SRC "${CMAKE_CURRENT_SOURCE_DIR}")
set(BIN "${CMAKE_CURRENT_BINARY_DIR}")
set(GEN "${BIN}/generated")

list(APPEND CMAKE_MODULE_PATH "${SRC}/cmake")

if (NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING
		"The type of build to perform. Valid values are: RelWithDebInfo (default), Debug, Release, MinSizeRel" FORCE)
endif()

if(NOT BUILD_SHARED_LIBS)
	set(BUILD_SHARED_LIBS "FALSE" CACHE STRING "" FORCE)
endif()

if(WINDOWS AND NOT BUILD_SHARED_LIBS)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CheckCXXSourceCompiles)
# AddressSanitizer is on by default (as upstream); OPENC2E_ASAN=OFF builds
# without it, for playing: it slows everything severalfold
option(OPENC2E_ASAN "Build with AddressSanitizer" ON)
if(OPENC2E_ASAN AND NOT WIN32)
	set(CMAKE_REQUIRED_FLAGS "-fsanitize=address")
	check_cxx_source_compiles("int main(int, char**) { return 0; }" have_fsanitize_address)
	if(have_fsanitize_address)
		string(APPEND CMAKE_CXX_FLAGS " -fsanitize=address")
	endif()
endif()

if(APPLE)
	enable_language(OBJC)
endif()

option(OPENC2E_DEV_HOOKS "Build the developer switches (see src/common/devenv.h); off for players" ON)
option(OPENC2E_PROFILE_ALLOCATION "Collect allocation profile stats for DBG: SIZO" OFF)
if (OPENC2E_PROFILE_ALLOCATION)
	add_definitions("-DPROFILE_ALLOCATION_COUNT -DPROFILE_ALLOCATION_THREAD_SAFE")
endif()

# Turn off warnings for external dependencies
set(original_c_flags "${CMAKE_C_FLAGS}")
set(original_cxx_flags "${CMAKE_CXX_FLAGS}")
if (MSVC)
	string(REGEX REPLACE "/W[^ ]+" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
	string(REGEX REPLACE "/W[^ ]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
	string(APPEND CMAKE_C_FLAGS " /w")
	string(APPEND CMAKE_CXX_FLAGS " /w")
else()
string(REGEX REPLACE "-W[^ ]+" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
string(REGEX REPLACE "-W[^ ]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
	string(APPEND CMAKE_C_FLAGS " -w")
	string(APPEND CMAKE_CXX_FLAGS " -w")
endif()

# Find / include dependencies
find_package(Python REQUIRED)
if(WIN32)
	add_subdirectory(externals/zlib EXCLUDE_FROM_ALL)
else()
	find_package(ZLIB REQUIRED)
endif()

# The SDL3 in externals is built on Windows and macOS; elsewhere the system's
# is used unless OPENC2E_BUNDLED_SDL is on (for example a 32-bit Linux build
# on a 64-bit system, which may have no 32-bit SDL3 package)
if(WIN32 OR APPLE)
	set(OPENC2E_BUNDLED_SDL_DEFAULT ON)
else()
	set(OPENC2E_BUNDLED_SDL_DEFAULT OFF)
endif()
option(OPENC2E_BUNDLED_SDL "Build the SDL3 in externals instead of using the system's" ${OPENC2E_BUNDLED_SDL_DEFAULT})
if(OPENC2E_BUNDLED_SDL)
	add_subdirectory(externals/SDL3 EXCLUDE_FROM_ALL)
else()
	find_package(SDL3 REQUIRED)
endif()
set(SDLMIXER_AIFF OFF)
set(SDLMIXER_AU OFF)
set(SDLMIXER_FLAC OFF)
set(SDLMIXER_GME OFF)
set(SDLMIXER_MIDI OFF) # TODO
set(SDLMIXER_MOD OFF)
set(SDLMIXER_MP3 OFF)
set(SDLMIXER_OPUS OFF)
set(SDLMIXER_VOC OFF)
set(SDLMIXER_VORBIS_STB OFF)
set(SDLMIXER_VORBIS_VORBISFILE OFF)
set(SDLMIXER_WAVPACK OFF)
add_subdirectory(externals/SDL3_mixer EXCLUDE_FROM_ALL)
add_subdirectory(externals/SDL3_native_midi EXCLUDE_FROM_ALL)

add_subdirectory(externals/cxxopts EXCLUDE_FROM_ALL)
add_subdirectory(externals/fmt EXCLUDE_FROM_ALL)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE) # fix 'mismatch detected for RuntimeLibrary'
set(INSTALL_GTEST OFF)
add_subdirectory(externals/googletest EXCLUDE_FROM_ALL)
add_subdirectory(externals/imgui EXCLUDE_FROM_ALL)

# turn warnings back on
set(CMAKE_C_FLAGS "${original_c_flags}")
set(CMAKE_CXX_FLAGS "${original_cxx_flags}")

# Pass flags to the compiler
if(OPENC2E_DEV_HOOKS)
	add_compile_definitions("OPENC2E_DEV_HOOKS")
endif()
if(WIN32)
	add_compile_definitions("NOMINMAX")
endif()
if(NOT WIN32)
	# 64-bit file offsets (off_t, fseeko) on 32-bit systems too; a no-op on
	# 64-bit ones
	add_compile_definitions("_FILE_OFFSET_BITS=64")
endif()
if (CMAKE_CXX_COMPILER_ID MATCHES "(Clang|GNU)")
	add_compile_options(-Wextra -Wall -Wno-conversion)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# make sure assert() works
string(REPLACE "/DNDEBUG" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
string(REPLACE "-DNDEBUG" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")

find_package(Threads REQUIRED) # stupid GCC needs explicit flags to use std::thread
link_libraries(Threads::Threads)

include_directories(BEFORE
	${SRC}/src
)

# Targets
add_library(openc2e-common OBJECT
	src/common/audio/AudioBackend.cpp
	src/common/audio/NullAudioBackend.cpp
	src/common/backend/Backend.cpp
	src/common/backend/NullBackend.cpp
	src/common/io/FileReader.cpp
	src/common/io/FileWriter.cpp
	src/common/io/SpanReader.cpp
	src/common/io/StringWriter.cpp
	src/common/io/VectorWriter.cpp
	src/common/ioutil/Getline.cpp
	src/common/ioutil/Scanf.cpp
	src/common/render/RenderItem.cpp
	src/common/render/RenderSystem.cpp
	src/common/case_insensitive_filesystem.cpp
	src/common/creaturesImage.cpp
	src/common/encoding.cpp
	src/common/mappedfile.cpp
	src/common/NumericCast.cpp
	src/common/readfile.cpp
	src/common/Random.cpp
	src/common/userlocale.cpp
	src/common/WindowsRegistry.cpp
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
	target_compile_options(openc2e-common BEFORE PRIVATE -Wconversion -Wsign-conversion -Wsign-compare)
endif()
target_link_libraries(openc2e-common
	PUBLIC fmt
	PRIVATE "$<$<PLATFORM_ID:Darwin>:-framework Foundation>"
)

add_executable(test_common
	src/common/io/FileIOTest.cpp
	src/common/io/SpanReaderTest.cpp
	src/common/io/StringWriterTest.cpp
	src/common/io/VectorWriterTest.cpp
	src/common/ioutil/GetlineTest.cpp
	src/common/ioutil/ScanfTest.cpp
	src/common/math/ModularArithmeticTest.cpp
	src/common/slotmap/DenseSlotMapTest.cpp
	src/common/DemangleTest.cpp
	src/common/EncodingTest.cpp
	src/common/EndsWithTest.cpp
	src/common/NumericCastTest.cpp
	src/common/WildcardMatchTest.cpp
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
	target_compile_options(test_common BEFORE PRIVATE -Wconversion -Wsign-conversion -Wsign-compare)
endif()
target_link_libraries(test_common openc2e-common gtest_main)

add_library(openc2e-fileformats STATIC
	src/fileformats/attFile.cpp
	src/fileformats/blkImage.cpp
	src/fileformats/bmpImage.cpp
	src/fileformats/caoslexer.cpp
	src/fileformats/caostoken.cpp
	src/fileformats/catalogueFile.cpp
	src/fileformats/cfgFile.cpp
	src/fileformats/charsetdta.cpp
	src/fileformats/c1cobfile.cpp
	src/fileformats/c1defaultpalette.cpp
	src/fileformats/c16Image.cpp
	src/fileformats/c2cobfile.cpp
	src/fileformats/genomeFile.cpp
	src/fileformats/hedfile.cpp
	src/fileformats/MFCReader.cpp
	src/fileformats/MFCWriter.cpp
	src/fileformats/mngfile.cpp
	src/fileformats/mngparser.cpp
	src/fileformats/paletteFile.cpp
	src/fileformats/peFile.cpp
	src/fileformats/pngImage.cpp
	src/fileformats/PrayFileReader.cpp
	src/fileformats/PrayFileWriter.cpp
	src/fileformats/PraySourceParser.cpp
	src/fileformats/s16Image.cpp
	src/fileformats/sprImage.cpp
	src/fileformats/strFile.cpp
	src/fileformats/verticalbarFile.cpp
	src/fileformats/ImageUtils.cpp
	src/fileformats/sfc/EXPFile.cpp
	src/fileformats/sfc/SFCFile.cpp
)
target_include_directories(openc2e-fileformats PRIVATE src/fileformats)
target_link_libraries(openc2e-fileformats
	PUBLIC openc2e-common
	PRIVATE ZLIB::ZLIB
)

add_executable(test_fileformats
	src/fileformats/tests/CaosLexerTest.cpp
	src/fileformats/tests/CatalogueFileTest.cpp
	src/fileformats/tests/CfgFileTest.cpp
	src/fileformats/tests/GenomeFileTest.cpp
	src/fileformats/tests/MngTest.cpp
	src/fileformats/tests/PrayTest.cpp
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
	target_compile_options(test_fileformats BEFORE PRIVATE -Wconversion -Wsign-conversion -Wsign-compare)
endif()
target_link_libraries(test_fileformats openc2e-fileformats gtest_main)

add_library(openc2e-mngmusic STATIC
	src/libmngmusic/MNGMusic.cpp
)
target_link_libraries(openc2e-mngmusic PRIVATE openc2e-fileformats)

add_library(openc2e_sdlbackend OBJECT
	src/sdlbackend/SDLBackend.cpp
	src/sdlbackend/SDLMixerBackend.cpp
)
target_link_libraries(openc2e_sdlbackend
	PUBLIC
		openc2e-common
		imgui
		SDL3::SDL3
	PRIVATE
		SDL3_mixer::SDL3_mixer
		SDL_native_midi
		${CMAKE_DL_LIBS}
)

add_library(openc2e_core OBJECT
	src/openc2e/Agent.cpp
	src/openc2e/AgentHelpers.cpp
	src/openc2e/AgentRef.cpp
	src/openc2e/alloc_count.cpp
	src/openc2e/creatures/Biochemistry.cpp
	src/openc2e/Blackboard.cpp
	src/openc2e/Bubble.cpp
	src/openc2e/bytecode.cpp
	src/openc2e/creatures/c2eBrain.cpp
	src/openc2e/CallButton.cpp
	src/openc2e/Camera.cpp
	src/openc2e/caosException.cpp
	src/openc2e/caosparser.cpp
	src/openc2e/caosScript.cpp
	src/openc2e/caosValue.cpp
	src/openc2e/caos/caosVM_agent.cpp
	src/openc2e/caos/caosVM_camera.cpp
	src/openc2e/caos/caosVM_cdplayer.cpp
	src/openc2e/caos/caosVM_compound.cpp
	src/openc2e/caos/caosVM_core.cpp
	src/openc2e/caos/caosVM_creatures.cpp
	src/openc2e/caos/caosVM_debug.cpp
	src/openc2e/caos/caosVM_files.cpp
	src/openc2e/caos/caosVM_flow.cpp
	src/openc2e/caos/caosVM_genetics.cpp
	src/openc2e/caos/caosVM_history.cpp
	src/openc2e/caos/caosVM_input.cpp
	src/openc2e/caos/caosVM_map.cpp
	src/openc2e/caos/caosVM_motion.cpp
	src/openc2e/caos/caosVM_net.cpp
	src/openc2e/caosVM.cpp
	src/openc2e/caos/caosVM_ports.cpp
	src/openc2e/caos/caosVM_resources.cpp
	src/openc2e/caos/caosVM_scripts.cpp
	src/openc2e/caos/caosVM_sounds.cpp
	src/openc2e/caos/caosVM_time.cpp
	src/openc2e/caos/caosVM_variables.cpp
	src/openc2e/caos/caosVM_vectors.cpp
	src/openc2e/caos/caosVM_vehicles.cpp
	src/openc2e/caos/caosVM_world.cpp
	src/openc2e/Catalogue.cpp
	${GEN}/cmddata.cpp
	${GEN}/embedded_files.cpp
	${GEN}/embedded_additions.cpp
	src/openc2e/Additions.cpp
	src/openc2e/madeart/MadeArt.cpp
	src/openc2e/madeart/pillarfoot.cpp
	src/openc2e/madeart/cavepath.cpp
	src/openc2e/madeart/cave_ramp.cpp
	src/openc2e/madeart/stream_bed.cpp
	src/openc2e/madeart/volcano_trunk.cpp
	src/openc2e/madeart/westlock_rock.cpp
	src/openc2e/madeart/treelift_top.cpp
	src/openc2e/madeart/tube_short.cpp
	src/openc2e/madeart/volcano_passage.cpp
	src/openc2e/madeart/ettinhead.cpp
	src/openc2e/madeart/golden_desert_heads.cpp
	src/openc2e/madeart/npcore.cpp
	src/openc2e/madeart/tube_walk_paint.cpp
	src/openc2e/madeart/tube_walk_lab.cpp
	src/openc2e/madeart/tube_walk_parts.cpp
	src/openc2e/madeart/tube_walk.cpp
	src/openc2e/madeart/sea_collar.cpp
	src/openc2e/madeart/tree_sway.cpp
	src/openc2e/CompoundAgent.cpp
	src/openc2e/AnimatablePart.cpp
	src/openc2e/ButtonPart.cpp
	src/openc2e/CameraPart.cpp
	src/openc2e/ChemicalNamesManager.cpp
	src/openc2e/CobManager.cpp
	src/openc2e/CompoundPart.cpp
	src/openc2e/DullPart.cpp
	src/openc2e/FixedTextPart.cpp
	src/openc2e/GraphPart.cpp
	src/openc2e/SpritePart.cpp
	src/openc2e/TextPart.cpp
	src/openc2e/TextEntryPart.cpp
	src/openc2e/creatures/CompoundCreature.cpp
	src/openc2e/creatures/Breeding.cpp
	src/openc2e/creatures/CreatureSpeech.cpp
	src/openc2e/creatures/Pheromones.cpp
	src/openc2e/creatures/Disease.cpp
	src/openc2e/creatures/Exposure.cpp
	src/openc2e/creatures/Thoughts.cpp
	src/openc2e/creatures/Relationships.cpp
	src/openc2e/creatures/Temperament.cpp
	src/openc2e/creatures/GenomeTraits.cpp
	src/openc2e/creatures/MindGenes.cpp
	src/openc2e/creatures/BrainProfiles.cpp
	src/openc2e/creatures/Creature.cpp
	src/openc2e/creatures/CreatureAgent.cpp
	src/openc2e/creatures/CreatureAI.cpp
	src/openc2e/creatures/CreatureMind.cpp
	src/openc2e/creatures/HomeostatMind.cpp
	src/openc2e/creatures/LlmMind.cpp
	src/openc2e/dialect.cpp
	src/openc2e/Engine.cpp
	src/openc2e/historyManager.cpp
	src/openc2e/imageManager.cpp
	src/openc2e/Lift.cpp
	src/openc2e/Map.cpp
	src/openc2e/MetaRoom.cpp
	src/openc2e/MusicManager.cpp
	src/openc2e/net/socket.cpp
	src/openc2e/NetBackend.cpp
	src/openc2e/creatures/oldBrain.cpp
	src/openc2e/parseException.cpp
	src/openc2e/partzorder.cpp
	src/openc2e/GameFolder.cpp
	src/openc2e/PathResolver.cpp
	src/openc2e/physics.cpp
	src/openc2e/PointerAgent.cpp
	src/openc2e/Port.cpp
	src/openc2e/prayManager.cpp
	src/openc2e/renderable.cpp
	src/openc2e/renderablezorder.cpp
	src/openc2e/Room.cpp
	src/openc2e/Scriptorium.cpp
	src/openc2e/SkyTint.cpp
	src/openc2e/BirthPortrait.cpp
	src/openc2e/SFCFile.cpp
	src/openc2e/SimpleAgent.cpp
	src/openc2e/creatures/SkeletalCreature.cpp
	src/openc2e/Sound.cpp
	src/openc2e/SoundManager.cpp
	src/openc2e/Vehicle.cpp
	src/openc2e/VoiceData.cpp
	src/openc2e/World.cpp
	src/openc2e/WorldSave.cpp
	src/openc2e/openc2eimgui/AgentInjector.cpp
	src/openc2e/openc2eimgui/BrainViewer.cpp
	src/openc2e/openc2eimgui/C1ToolBar.cpp
	src/openc2e/openc2eimgui/C2EyeView.cpp
	src/openc2e/openc2eimgui/BreedersKit.cpp
	src/openc2e/openc2eimgui/BreedPacks.cpp
	src/openc2e/openc2eimgui/Breeds.cpp
	src/openc2e/openc2eimgui/C2Kits.cpp
	src/openc2e/openc2eimgui/HealthKit.cpp
	src/openc2e/openc2eimgui/ObservationKit.cpp
	src/openc2e/openc2eimgui/OwnersKit.cpp
	src/openc2e/openc2eimgui/RelationshipsPage.cpp
	src/openc2e/openc2eimgui/ChroniclesKit.cpp
	src/openc2e/openc2eimgui/GraveyardKit.cpp
	src/openc2e/openc2eimgui/MindKit.cpp
	src/openc2e/openc2eimgui/EcologyKit.cpp
	src/openc2e/openc2eimgui/HelpWindow.cpp
	src/openc2e/openc2eimgui/ScienceKit.cpp
	src/openc2e/openc2eimgui/C2ScrollBars.cpp
	src/openc2e/openc2eimgui/C2StatusBar.cpp
	src/openc2e/openc2eimgui/WorldChooser.cpp
	src/openc2e/openc2eimgui/C2ToolBar.cpp
	src/openc2e/openc2eimgui/CreatureGrapher.cpp
	src/openc2e/openc2eimgui/Hatchery.cpp
	src/openc2e/openc2eimgui/ImGuiUtils.cpp
	src/openc2e/openc2eimgui/ImportCreature.cpp
	src/openc2e/openc2eimgui/MainMenu.cpp
	src/openc2e/openc2eimgui/Openc2eImGui.cpp
)
target_include_directories(openc2e_core PRIVATE src/openc2e)
target_link_libraries(openc2e_core PUBLIC
	cxxopts
	imgui
	openc2e-common
	openc2e-fileformats
	openc2e-mngmusic
)
# (the Breeder's Kit reads zip files: on Windows zlib is built from
# externals, and its headers come only with the link)
target_link_libraries(openc2e_core PRIVATE ZLIB::ZLIB)
target_sources(openc2e_core PRIVATE src/openc2e/caos/caosVM_ser_stub.cpp)

add_executable(openc2e src/openc2e/main.cpp)
target_include_directories(openc2e PRIVATE src/openc2e)
target_link_libraries(openc2e openc2e_core openc2e_sdlbackend)
# Windows (owner, 2026-11-20: players start it from the Start menu): the
# program is Zeetures.exe, with its icon, and a windowed program, so no
# console window opens behind the game (main.cpp: its output goes to the
# command prompt it was started from, or to zeetures-log.txt)
if(WIN32)
	target_sources(openc2e PRIVATE src/openc2e/zeetures.rc)
	set_target_properties(openc2e PROPERTIES WIN32_EXECUTABLE TRUE OUTPUT_NAME Zeetures)
	if(MSVC)
		target_link_options(openc2e PRIVATE /ENTRY:mainCRTStartup)
	endif()
endif()

add_executable(test_openc2e
	src/openc2e/tests/CaosTest.cpp
	src/openc2e/tests/WorldSaveTest.cpp
)
target_link_libraries(test_openc2e openc2e_core gtest_main)

add_executable(caos_to_bytecode src/tools/caos_to_bytecode.cpp)
target_link_libraries(caos_to_bytecode openc2e_core)

add_executable(cobdumper src/tools/cobdumper.cpp)
target_link_libraries(cobdumper openc2e_core)

add_executable(mngdumper src/tools/mngdumper.cpp)
target_link_libraries(mngdumper openc2e-fileformats)

add_executable(praydumper src/tools/praydumper.cpp)
target_link_libraries(praydumper openc2e-fileformats)

add_executable(praybuilder src/tools/praybuilder.cpp)
target_link_libraries(praybuilder openc2e-fileformats)

add_executable(spritedumper src/tools/spritedumper.cpp)
target_link_libraries(spritedumper openc2e-fileformats)

# tests
enable_testing()
include(GoogleTest)
include(Openc2eOutputOnTestFailure)

gtest_discover_tests(test_openc2e DISCOVERY_TIMEOUT 30)
gtest_discover_tests(test_common DISCOVERY_TIMEOUT 30)
gtest_discover_tests(test_fileformats DISCOVERY_TIMEOUT 30)

# Generated files
get_target_property(CAOSVM_FILES openc2e_core SOURCES)
list(FILTER CAOSVM_FILES INCLUDE REGEX "^src/openc2e/caos/caosVM_.*\.cpp$")
add_custom_command(
	OUTPUT ${GEN}/commandinfo.json
	COMMAND ${CMAKE_COMMAND} -E make_directory "${GEN}"
	COMMAND ${Python_EXECUTABLE} scripts/parsedocs.py ${CAOSVM_FILES} > ${GEN}/commandinfo.json
	DEPENDS scripts/parsedocs.py ${CAOSVM_FILES}
	WORKING_DIRECTORY ${SRC})

add_custom_command(
	OUTPUT ${BIN}/caosdocs.html
	COMMAND ${Python_EXECUTABLE} scripts/writehtml.py ${GEN}/commandinfo.json > ${BIN}/caosdocs.html
	DEPENDS scripts/writehtml.py scripts/docs.css ${GEN}/commandinfo.json
	WORKING_DIRECTORY ${SRC})

add_custom_command(
	OUTPUT ${GEN}/cmddata.cpp
	COMMAND ${CMAKE_COMMAND} -E make_directory "${GEN}"
	COMMAND ${Python_EXECUTABLE} scripts/writecmds.py ${GEN}/commandinfo.json > ${GEN}/cmddata.cpp
	DEPENDS scripts/writecmds.py ${GEN}/commandinfo.json
	WORKING_DIRECTORY ${SRC})

add_custom_target(docs ALL DEPENDS ${BIN}/caosdocs.html)

# openc2e's own art (the Hatchery nursery), built into the program
set(EMBEDDED_FILES
	${SRC}/data/hatchery/wall.png
	${SRC}/data/hatchery/board.png
	${SRC}/data/hatchery/nests.png
	${SRC}/data/hatchery/nests.txt
	${SRC}/data/thoughts/icons.png
	${SRC}/CITATIONS.md)
add_custom_command(
	OUTPUT ${GEN}/embedded_files.cpp
	COMMAND ${Python_EXECUTABLE} scripts/embed_files.py ${GEN}/embedded_files.cpp ${EMBEDDED_FILES}
	DEPENDS scripts/embed_files.py ${EMBEDDED_FILES}
	WORKING_DIRECTORY ${SRC})

# Zeetures's world additions (additions/: the scripts run after a C2 world
# loads, and the text files read with them), built into the program
file(GLOB ADDITION_FILES CONFIGURE_DEPENDS ${SRC}/additions/*.cos ${SRC}/additions/*.txt)
list(SORT ADDITION_FILES)
add_custom_command(
	OUTPUT ${GEN}/embedded_additions.cpp
	COMMAND ${Python_EXECUTABLE} scripts/embed_files.py ${GEN}/embedded_additions.cpp --func EmbeddedAddition ${ADDITION_FILES}
	DEPENDS scripts/embed_files.py ${ADDITION_FILES}
	WORKING_DIRECTORY ${SRC})

# Zeetures's own sprites for the world additions (art/sprites, drawn from
# scratch), beside the program as "zeetures-art": copied next to the built
# program, and installed into every download
add_custom_command(TARGET openc2e POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${SRC}/art/sprites $<TARGET_FILE_DIR:openc2e>/zeetures-art)
install(DIRECTORY art/sprites/ DESTINATION "zeetures-art")

# What players get: the game, its art, and the license and source notice.
# The developer tools and the CAOS reference are installed only when asked
# for (cmake --install build --component devtools); the packages leave them
# out (owner, 2026-11-21: "I want this to run well on people's computers").
install(FILES
	AUTHORS
	COPYING
	README.md
	DESTINATION "."
)
install(TARGETS
	openc2e
	DESTINATION "."
)
install(FILES
	${BIN}/caosdocs.html
	${GEN}/commandinfo.json
	DESTINATION "."
	COMPONENT devtools
	EXCLUDE_FROM_ALL
)
install(TARGETS
	cobdumper
	mngdumper
	praybuilder
	praydumper
	spritedumper
	DESTINATION "."
	COMPONENT devtools
	EXCLUDE_FROM_ALL
)

if(WIN32)
	set(CPACK_GENERATOR ZIP)
else()
	set(CPACK_GENERATOR TGZ)
endif()
include(Openc2eReadGitRev)
set(CPACK_PACKAGE_VERSION "${git_short_rev}")
include(CPack)
