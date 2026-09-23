# aae engine: arm64-v8a only — thirdparty/mbedtls ships a prebuilt
# arm64 static lib (libmbedcrypto.a). Adding ABIs needs per-ABI rebuilds.
APP_ABI := arm64-v8a
APP_PLATFORM := android-21
APP_STL := c++_static
APP_OPTIM := release
APP_THIN_ARCHIVE := true
APP_PIE := true
