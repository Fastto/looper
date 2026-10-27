#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint looper_engine.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'looper_engine'
  s.version          = '0.1.0'
  s.summary          = 'Real-time looper audio engine (C++, miniaudio).'
  s.description      = <<-DESC
Real-time looper audio engine (C++, miniaudio).
                       DESC
  s.homepage         = 'https://github.com/Fastto/looper'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Looper contributors' => 'https://github.com/Fastto/looper' }

  # This will ensure the source files in Classes/ are included in the native
  # builds of apps using this FFI plugin. Podspec does not support relative
  # paths, so Classes contains a forwarder C file that relatively imports
  # `../src/*` so that the C sources can be shared among all target platforms.
  s.source           = { :path => '.' }
  s.source_files = 'Classes/**/*'

  # If your plugin requires a privacy manifest, for example if it collects user
  # data, update the PrivacyInfo.xcprivacy file to describe your plugin's
  # privacy impact, and then uncomment this line. For more information,
  # see https://developer.apple.com/documentation/bundleresources/privacy_manifest_files
  # s.resource_bundles = {'looper_engine_privacy' => ['Resources/PrivacyInfo.xcprivacy']}

  s.dependency 'FlutterMacOS'

  s.platform = :osx, '12.0'
  s.frameworks = 'CoreAudio', 'AudioToolbox', 'AudioUnit'
  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17',
    'GCC_OPTIMIZATION_LEVEL' => '3',
  }
  s.swift_version = '5.0'
end
