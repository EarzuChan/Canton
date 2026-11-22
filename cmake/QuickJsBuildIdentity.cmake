file(SHA256 "${INPUT}" identity)
file(CONFIGURE OUTPUT "${OUTPUT}/QuickJsBuildIdentity.h"
  CONTENT "#pragma once\n#define CANTON_QUICKJS_BUILD_IDENTITY \"@identity@\"\n" @ONLY)
file(CONFIGURE OUTPUT "${OUTPUT}/identity.txt" CONTENT "@identity@\n" @ONLY)
