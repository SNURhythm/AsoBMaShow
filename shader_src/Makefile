# Keep backend profiles, include dependencies, and ESSL compatibility handling
# in the same compiler path used by CI and direct local builds.
PYTHON ?= python3

.PHONY: all metal spirv essl dx11 clean print-shaders FORCE

all:
	$(PYTHON) make.py

metal spirv essl dx11:
	$(PYTHON) make.py --backend $@

clean:
	$(PYTHON) make.py clean

print-shaders:
	@echo "$(patsubst %.sc,../shaders/metal/%.bin,$(wildcard fs_*.sc */fs_*.sc))"

# Always consult the Python dependency check, including for an existing target.
../shaders/metal/%.bin: FORCE
	$(PYTHON) make.py --backend metal --shader "$*.sc"

../shaders/spirv/%.bin: FORCE
	$(PYTHON) make.py --backend spirv --shader "$*.sc"

../shaders/essl/%.bin: FORCE
	$(PYTHON) make.py --backend essl --shader "$*.sc"

../shaders/dx11/%.bin: FORCE
	$(PYTHON) make.py --backend dx11 --shader "$*.sc"

FORCE:
