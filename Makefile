# Common tasks. Run `make` to list them.

APP    := app
ENGINE := packages/looper_engine
DEVICE ?= macos

# CocoaPods fails without a UTF-8 locale.
export LANG := en_US.UTF-8

.DEFAULT_GOAL := help
.PHONY: help up release test test-engine analyze bindings clean

help: ## Show available targets
	@grep -E '^[a-z-]+:.*## ' $(MAKEFILE_LIST) | awk 'BEGIN {FS = ":.*## "} {printf "  make %-12s %s\n", $$1, $$2}'

up: ## Run the app (debug, hot reload) on DEVICE (default: macos)
	cd $(APP) && flutter pub get && flutter run -d $(DEVICE)

release: ## Build a release Looper.app and open its folder
	cd $(APP) && flutter build macos --release
	open $(APP)/build/macos/Build/Products/Release

test: test-engine analyze ## Run all checks

test-engine: ## Build and run the C++ engine tests
	cmake -S $(ENGINE)/test_native -B $(ENGINE)/build/test_native
	cmake --build $(ENGINE)/build/test_native --target engine_test
	$(ENGINE)/build/test_native/engine_test

analyze: ## Static analysis of the Dart code
	cd $(ENGINE) && flutter pub get >/dev/null && dart analyze lib
	cd $(APP) && flutter pub get >/dev/null && flutter analyze

bindings: ## Regenerate Dart FFI bindings from looper_api.h
	cd $(ENGINE) && dart run ffigen --config ffigen.yaml

clean: ## Remove build outputs
	cd $(APP) && flutter clean
	rm -rf $(ENGINE)/build
