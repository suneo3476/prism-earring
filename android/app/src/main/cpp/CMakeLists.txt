cmake_minimum_required(VERSION 3.22.1)
project(prism LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# DSP コアはリポジトリ直下の dsp/include にある。
# このファイルの位置 android/app/src/main/cpp から 5 段上がるとリポジトリのルート。
get_filename_component(PRISM_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(PRISM_DSP_INCLUDE "${PRISM_REPO_ROOT}/dsp/include")

if(NOT EXISTS "${PRISM_DSP_INCLUDE}/prism/PitchShifter.h")
    message(FATAL_ERROR
        "prism::PitchShifter が見つかりません: ${PRISM_DSP_INCLUDE}/prism/PitchShifter.h\n"
        "android/ はリポジトリ直下に置いてください(dsp/ と兄弟の関係)。")
endif()

# Oboe は Maven の AAR から prefab で取り込む(app/build.gradle.kts の
# buildFeatures.prefab = true と com.google.oboe:oboe の依存が前提)。
find_package(oboe REQUIRED CONFIG)

add_library(prism SHARED
    PrismEngine.cpp
    jni_bridge.cpp
)

target_include_directories(prism PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PRISM_DSP_INCLUDE}"
)

# team.md「Code Style」: -Wall -Wextra はエラー扱い。
#
# -ffast-math は使わない。PitchShifter は std::isfinite による非有限値の弾きと
# 到達スナップによるデノーマル対策を自前で持っており、fast-math はその前提を壊す。
# オフライン検証(verify/)が素の -O2 で緑になっている以上、Android 側も同じ
# 数値挙動でビルドしておかないと「検証済み」と言えなくなる。
target_compile_options(prism PRIVATE
    -Wall
    -Wextra
    -Werror
    -O2
)

target_link_libraries(prism PRIVATE
    oboe::oboe
    android
    log
)
