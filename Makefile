.PHONY: backend-configure backend-build verify-system verify-immutable start

backend-configure:
	cmake -S backend -B runtime/build

backend-build: backend-configure
	cmake --build runtime/build -j

verify-system:
	bash scripts/check_system_requirements.sh

verify-immutable:
	bash scripts/check_legacy_immutable.sh

start:
	bash scripts/start.sh
