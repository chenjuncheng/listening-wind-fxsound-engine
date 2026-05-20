cmake_minimum_required(VERSION 3.20)
project(fxsound_engine VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 99)

# ============================================================================
# FxSound source root (original, unmodified)
# ============================================================================
if(NOT DEFINED FXSOUND_ROOT)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fxsound-app/dsp/DfxDsp.cpp")
        set(FXSOUND_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/fxsound-app")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../fxsound-app/dsp/DfxDsp.cpp")
        set(FXSOUND_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../fxsound-app")
    else()
        message(FATAL_ERROR "FxSound source not found. Put fxsound-app under this repo or next to fxsound-engine, or pass -DFXSOUND_ROOT=<path>.")
    endif()
endif()

message(STATUS "Using FxSound source root: ${FXSOUND_ROOT}")

# ============================================================================
# Our override directory (patches that take priority via include path)
# ============================================================================
set(OVERRIDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/overrides")

# Use Unicode (FxSound source was originally built with Unicode character set)
add_definitions(-DUNICODE -D_UNICODE)

# ============================================================================
# DSP Static Library (DfxDsp + ptechDsp + ptutil)
# ============================================================================
add_library(dfxdsp STATIC
    # DfxDsp core
    ${FXSOUND_ROOT}/dsp/DfxDsp.cpp
    ${FXSOUND_ROOT}/dsp/DfxDspEq.cpp
    ${FXSOUND_ROOT}/dsp/DfxDspPreset.cpp
    ${FXSOUND_ROOT}/dsp/DfxDspPrivate.cpp
    ${FXSOUND_ROOT}/dsp/DfxDspRegistry.cpp

    # ptComSftDfx
    ${FXSOUND_ROOT}/dsp/ptComSftDfx/Comsftwr.c
    ${FXSOUND_ROOT}/dsp/ptComSftDfx/ComsftwrCPP.cpp

    # ptechDsp - only the files included in the original vcxproj
    ${FXSOUND_ROOT}/dsp/ptechDsp/Aural/Aural032/Auralp32.c
    ${FXSOUND_ROOT}/dsp/ptechDsp/Aural/Aural0/Auralp.c
    ${FXSOUND_ROOT}/dsp/ptechDsp/Dly32/Dly832/dly8p32.c
    ${FXSOUND_ROOT}/dsp/ptechDsp/Dly8/Dly8p.c
    ${FXSOUND_ROOT}/dsp/ptechDsp/Lex/Lex16/Lex16.c
    ${FXSOUND_ROOT}/dsp/ptechDsp/Lex/Lex32/Lex32.c
    ${FXSOUND_ROOT}/dsp/ptechDsp/Maximizer/Maxi16/Maxi16.c
    ${FXSOUND_ROOT}/dsp/ptechDsp/Maximizer/Maxi32/Maxi32.c
    ${FXSOUND_ROOT}/dsp/ptechDsp/Peq/Peq832/peq8p32.c
    ${FXSOUND_ROOT}/dsp/ptechDsp/Peq/Peq8/peq8p.c
    ${FXSOUND_ROOT}/dsp/ptechDsp/Play/Play16/Play16.c
    ${FXSOUND_ROOT}/dsp/ptechDsp/Play/Play32/Play32.c
    ${FXSOUND_ROOT}/dsp/ptechDsp/wide/Wide16/Wide16.c
    ${FXSOUND_ROOT}/dsp/ptechDsp/wide/Wide32/Wide32.c

    # ptutil - COM
    ${FXSOUND_ROOT}/dsp/ptutil/COM/Com.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/COM/Comeprom.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/COM/Comget.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/COM/ComMem.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/COM/Compass.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/COM/Comread.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/COM/Comwave.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/COM/Comwrite.cpp

    # ptutil - dfxp
    ${FXSOUND_ROOT}/dsp/ptutil/dfxp/dfxpComm.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/dfxp/dfxpEq.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/dfxp/dfxpGet.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/dfxp/dfxpInit.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/dfxp/dfxpProcess.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/dfxp/dfxpProcessClear.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/dfxp/dfxpProcessInt.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/dfxp/dfxpProcessReal.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/dfxp/dfxpQnt.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/dfxp/dfxpQuit.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/dfxp/dfxpRegistryStandard.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/dfxp/dfxpSession.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/dfxp/dfxpSet.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/dfxp/dfxpSpectrum.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/dfxp/dfxpUniversal.cpp

    # ptutil - dfxSharedUtil
    ${FXSOUND_ROOT}/dsp/ptutil/dfxSharedUtil/dfxSharedUtil.cpp

    # ptutil - DspUtil/BinauralSync
    ${FXSOUND_ROOT}/dsp/ptutil/DspUtil/BinauralSync/BinauralSynGet.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/DspUtil/BinauralSync/BinauralSynInit.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/DspUtil/BinauralSync/BinauralSynProcess.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/DspUtil/BinauralSync/BinauralSynSet.cpp

    # ptutil - DspUtil/GraphicEq
    ${FXSOUND_ROOT}/dsp/ptutil/DspUtil/GraphicEq/GraphicEqGet.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/DspUtil/GraphicEq/GraphicEqInit.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/DspUtil/GraphicEq/GraphicEqInitBands.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/DspUtil/GraphicEq/GraphicEqInitSections.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/DspUtil/GraphicEq/GraphicEqProcess.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/DspUtil/GraphicEq/GraphicEqSet.cpp

    # ptutil - DspUtil/spectrum
    ${FXSOUND_ROOT}/dsp/ptutil/DspUtil/spectrum/spectrumGet.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/DspUtil/spectrum/spectrumInit.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/DspUtil/spectrum/spectrumMessageValues.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/DspUtil/spectrum/spectrumProcess.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/DspUtil/spectrum/spectrumReset.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/DspUtil/spectrum/spectrumSet.cpp

    # ptutil - DspUtil/SurroundSyn
    ${FXSOUND_ROOT}/dsp/ptutil/DspUtil/SurroundSyn/SurroundSynInit.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/DspUtil/SurroundSyn/SurroundSynProcess.cpp

    # ptutil - Filt
    ${FXSOUND_ROOT}/dsp/ptutil/Filt/Fil12But.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/Filt/FiltbiqdSos.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/Filt/FiltCalcBiqd.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/Filt/FiltCalcFilterResponse.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/Filt/Filtpoly.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/Filt/FiltRun.cpp

    # ptutil - PRELST
    ${FXSOUND_ROOT}/dsp/ptutil/PRELST/Prelst.cpp

    # ptutil - PWAV
    ${FXSOUND_ROOT}/dsp/ptutil/PWAV/Pwav.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/PWAV/PwavConvert.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/PWAV/PwavIO.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/PWAV/PwavUtil.cpp

    # ptutil - Qnt
    ${FXSOUND_ROOT}/dsp/ptutil/Qnt/Qnt.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/Qnt/Qnt2But.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/Qnt/QntitoBoostCut.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/Qnt/Qntitol.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/Qnt/Qntitor.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/Qnt/Qntitor2.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/Qnt/Qntrtoi.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/Qnt/Qntrtol.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/Qnt/Qntrtor.cpp

    # ptutil - realSample
    ${FXSOUND_ROOT}/dsp/ptutil/realSample/realSampleForceLegalValues.cpp

    # ptutil - SOS
    ${FXSOUND_ROOT}/dsp/ptutil/SOS/Sos.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/SOS/SosGet.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/SOS/SosProcess.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/SOS/SosSet.cpp

    # ptutil - VALS
    ${FXSOUND_ROOT}/dsp/ptutil/VALS/Vals.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/VALS/Valscfg.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/VALS/Valsfile.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/VALS/Valsget.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/VALS/Valsset.cpp
    ${FXSOUND_ROOT}/dsp/ptutil/VALS/Valswarp.cpp
)

target_include_directories(dfxdsp PRIVATE
    ${OVERRIDE_DIR}                # Our overrides take priority
    ${FXSOUND_ROOT}/dsp/ptutil/include
    ${FXSOUND_ROOT}/dsp/include
    ${FXSOUND_ROOT}/audiopassthru/include
    ${FXSOUND_ROOT}/dsp/ptComSftDfx
    ${FXSOUND_ROOT}/dsp
)

target_compile_definitions(dfxdsp PRIVATE
    PT_NON_MFC
    DSPSOFT_TARGET
    PT_DSP_BUILD=PT_DSP_DFX
    NO_REGISTRY                # Disable registry operations
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

# Disable SDL checks for the legacy C files (Play16.c, Play32.c, etc.)
set_source_files_properties(
    ${FXSOUND_ROOT}/dsp/ptechDsp/Play/Play16/Play16.c
    ${FXSOUND_ROOT}/dsp/ptechDsp/Play/Play32/Play32.c
    PROPERTIES COMPILE_FLAGS "/SDL-"
)

# Use static CRT
set_property(TARGET dfxdsp PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ============================================================================
# AudioPassthru Static Library
# ============================================================================
add_library(audiopassthru STATIC
    ${FXSOUND_ROOT}/audiopassthru/src/AudioPassthru/AudioPassthru.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/AudioPassthru/AudioPassthruPrivate.cpp

    # FILE
    ${FXSOUND_ROOT}/audiopassthru/src/FILE/FileDate.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/FILE/FileGeneral.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/FILE/fileWin32Handle.cpp

    # MRY
    ${FXSOUND_ROOT}/audiopassthru/src/MRY/Mry.cpp

    # MTH
    ${FXSOUND_ROOT}/audiopassthru/src/MTH/MthBuffer.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/MTH/Mthcrypt.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/MTH/Mthfreq.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/MTH/MthLinearRegression.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/MTH/MthRand.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/MTH/MthRegion.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/MTH/MthStat.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/MTH/MthTrig.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/MTH/MthUtil.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/MTH/MthWarp.cpp

    # operatingSystem
    ${FXSOUND_ROOT}/audiopassthru/src/operatingSystem/operatingSystem.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/operatingSystem/operatingSystemMouse.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/operatingSystem/operatingSystemSounds.cpp

    # pstr
    ${FXSOUND_ROOT}/audiopassthru/src/pstr/pstr.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/pstr/pstrArray.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/pstr/pstrArraySort.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/pstr/pstrCompare.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/pstr/pstrExtension.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/pstr/pstrHidden.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/pstr/pstrTimeDisplay.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/pstr/pstrURL.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/pstr/pstrUserLibraryFilename.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/pstr/pstrWide.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/pstr/pstrXML.cpp

    # ptime
    ${FXSOUND_ROOT}/audiopassthru/src/ptime/Ptime.cpp

    # reg
    ${FXSOUND_ROOT}/audiopassthru/src/reg/regRecursiveDelete.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/reg/regWithKeyname.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/reg/regWithoutKeyname.cpp

    # SLOUT
    ${FXSOUND_ROOT}/audiopassthru/src/SLOUT/Slout.cpp

    # sndDevices
    ${FXSOUND_ROOT}/audiopassthru/src/sndDevices/sndDevicesDeviceCallbacks.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/sndDevices/sndDevicesDoCapture.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/sndDevices/sndDevicesDoPlayback.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/sndDevices/sndDevicesGet.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/sndDevices/sndDevicesImplementDeviceRules.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/sndDevices/sndDevicesInit.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/sndDevices/sndDevicesReg.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/sndDevices/sndDevicesReInit.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/sndDevices/sndDevicesSet.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/sndDevices/sndDevicesSetupDevices.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/sndDevices/sndDevicesVolCallbacks.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/sndDevices/sndDevices_GetAll.cpp
    ${FXSOUND_ROOT}/audiopassthru/src/sndDevices/sndDevices_Utils.cpp
)

target_include_directories(audiopassthru PRIVATE
    ${OVERRIDE_DIR}                # Our overrides take priority
    ${FXSOUND_ROOT}/audiopassthru/include
    ${FXSOUND_ROOT}/dsp/include
    ${FXSOUND_ROOT}/dsp/ptutil/include
    ${FXSOUND_ROOT}/audiopassthru/src/sndDevices
    ${FXSOUND_ROOT}/audiopassthru/src
)

target_compile_definitions(audiopassthru PRIVATE
    NO_REGISTRY
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

target_link_libraries(audiopassthru PRIVATE dfxdsp)

# Use static CRT
set_property(TARGET audiopassthru PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ============================================================================
# Main executable
# ============================================================================
add_executable(fxsound_engine
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fxsound_engine_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/driver_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/crash_handler.cpp
)

target_include_directories(fxsound_engine PRIVATE
    ${OVERRIDE_DIR}
    ${FXSOUND_ROOT}/audiopassthru/include
    ${FXSOUND_ROOT}/dsp/include
    ${FXSOUND_ROOT}/dsp/ptutil/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(fxsound_engine PRIVATE
    NO_REGISTRY
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

if(MSVC)
    target_compile_options(fxsound_engine PRIVATE /utf-8)
endif()

target_link_libraries(fxsound_engine PRIVATE
    audiopassthru
    dfxdsp
    Mmdevapi.lib
    Ole32.lib
    Propsys.lib
    ksuser.lib
    Shlwapi.lib
    SetupAPI.lib
    Newdev.lib
    Advapi32.lib
    Dbghelp.lib
    Version.lib
)

# Use static CRT
set_property(TARGET fxsound_engine PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Console subsystem (not Windows GUI)
set_target_properties(fxsound_engine PROPERTIES
    LINK_FLAGS "/SUBSYSTEM:CONSOLE"
)

# ============================================================================
# Copy preset files to build output directory for testing
# ============================================================================
add_custom_command(TARGET fxsound_engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${FXSOUND_ROOT}/dsp/ptComSftDfx"
        "$<TARGET_FILE_DIR:fxsound_engine>/presets"
    COMMENT "Copying preset files to output directory"
)
