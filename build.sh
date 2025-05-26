#! /bin/env bash

[ -z "${SHARED_LIB_EXTENSION}" ] && SHARED_LIB_EXTENSION="so"
[ -z "${CC}" ] && CC="gcc"
[ -z "${PLATFORM}" ] && PLATFORM="linux-amd64"

ARTIFACT_DIRECTORY="build/${PLATFORM}"

mkdir -p "${ARTIFACT_DIRECTORY}"

${CC} -Iinclude ${CFLAGS} -shared -fPIC -std=c23 -Wall \
    -Wextra -Werror cjson.c -o "${ARTIFACT_DIRECTORY}/json.${SHARED_LIB_EXTENSION}"