
# TODO: compile all shaders for all profiles
# example: shaderc -f shaders/fs_blurV.sc -o shaders/metal/fs_blurV.bin --platform osx --type f --profile metal -O 3

# get all shader files (fs_*.sc, vs_*.sc) with wildcard, patsubst

FRAGMENT_SHADERS := $(wildcard fs_*.sc */fs_*.sc)


FRAGMENT_SHADERS_BIN_METAL := $(foreach shader,$(FRAGMENT_SHADERS),../shaders/metal/$(subst .sc,.bin,$(shader)))

FRAGMENT_SHADERS_BIN_VULKAN := $(foreach shader,$(FRAGMENT_SHADERS),../shaders/spirv/$(subst .sc,.bin,$(shader)))
FRAGMENT_SHADERS_BIN_ESSL := $(foreach shader,$(FRAGMENT_SHADERS),../shaders/essl/$(subst .sc,.bin,$(shader)))
VERTEX_SHADERS := $(wildcard vs_*.sc */vs_*.sc)
VERTEX_SHADERS_BIN_METAL := $(foreach shader,$(VERTEX_SHADERS),../shaders/metal/$(subst .sc,.bin,$(shader)))
VERTEX_SHADERS_BIN_VULKAN := $(foreach shader,$(VERTEX_SHADERS),../shaders/spirv/$(subst .sc,.bin,$(shader)))
VERTEX_SHADERS_BIN_ESSL := $(foreach shader,$(VERTEX_SHADERS),../shaders/essl/$(subst .sc,.bin,$(shader)))

all: metal spirv essl $(FRAGMENT_SHADERS_BIN_METAL) $(VERTEX_SHADERS_BIN_METAL) $(FRAGMENT_SHADERS_BIN_VULKAN) $(VERTEX_SHADERS_BIN_VULKAN) $(FRAGMENT_SHADERS_BIN_ESSL) $(VERTEX_SHADERS_BIN_ESSL)

print-shaders:
	@echo "$(FRAGMENT_SHADERS_BIN_METAL)"

clean:
	rm -rf ../shaders/metal ../shaders/spirv ../shaders/essl

metal: metal_dir $(FRAGMENT_SHADERS_BIN_METAL) $(VERTEX_SHADERS_BIN_METAL)

metal_dir:
	mkdir -p ../shaders/metal


$(FRAGMENT_SHADERS_BIN_METAL): $(FRAGMENT_SHADERS)
	mkdir -p $(dir $@)
	$(SHADERC) -f $< -o $@ --platform osx --type f --profile metal -O 3 -i .

$(VERTEX_SHADERS_BIN_METAL): $(VERTEX_SHADERS)
	mkdir -p $(dir $@)
	$(SHADERC) -f $< -o $@ --platform osx --type v --profile metal -O 3 -i .

spirv: spirv_dir $(FRAGMENT_SHADERS_BIN_VULKAN) $(VERTEX_SHADERS_BIN_VULKAN)

spirv_dir:
	mkdir -p ../shaders/spirv

$(FRAGMENT_SHADERS_BIN_VULKAN): $(FRAGMENT_SHADERS)
	mkdir -p $(dir $@)
	$(SHADERC) -f $< -o $@ --platform windows --type f --profile spirv -O 3 -i .

$(VERTEX_SHADERS_BIN_VULKAN): $(VERTEX_SHADERS)
	mkdir -p $(dir $@)
	$(SHADERC) -f $< -o $@ --platform windows --type v --profile spirv -O 3 -i .

essl: essl_dir $(FRAGMENT_SHADERS_BIN_ESSL) $(VERTEX_SHADERS_BIN_ESSL)

essl_dir:
	mkdir -p ../shaders/essl

$(FRAGMENT_SHADERS_BIN_ESSL): $(FRAGMENT_SHADERS)
	mkdir -p $(dir $@)
	$(SHADERC) -f $< -o $@ --platform android --type f --profile 100_es -O 3 -i .

$(VERTEX_SHADERS_BIN_ESSL): $(VERTEX_SHADERS)
	mkdir -p $(dir $@)
	$(SHADERC) -f $< -o $@ --platform android --type v --profile 100_es -O 3 -i .
