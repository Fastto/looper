// CocoaPods cannot reference ../src directly, so this forwarder pulls the
// shared C++ sources into the macOS build. Compiled as Objective-C++ because
// miniaudio uses Objective-C on Apple platforms.
#include "../../src/miniaudio_impl.cpp"
#include "../../src/engine.cpp"
#include "../../src/api.cpp"
