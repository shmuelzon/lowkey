# Generates the browsable hardware artifacts from their sources:
#
#   make pcb   - schematic PDF and 3D render of the board (KiCad 10)
#   make 3d    - printable STLs and preview renders of the enclosures (OpenSCAD)
#   make       - both
#
# The tools are looked up in PATH first, then in their default install
# locations on macOS and Windows (from WSL or Git Bash). Override any of them
# if it lives elsewhere:
#
#   make KICAD_CLI=/path/to/kicad-cli OPENSCAD=/path/to/openscad MAGICK=/path/to/magick
#
# ImageMagick is optional. With it, the renders get a transparent background
# and are trimmed to the model; without it they keep OpenSCAD's background.

PCB_DIR := pcb
3D_DIR  := 3d

PROJECT := smart-lock
MODELS  := controller ui

# Files each model imports, beyond its own .scad
controller_DEPS := controller_pcb.stl
ui_DEPS         := ui_pcb.stl lock.svg unlock.svg

# Per-model camera for the preview render (translate x,y,z, rotate x,y,z, distance)
controller_VIEW := --camera=0,0,0,55,0,25,0
ui_VIEW         := --camera=0,0,0,50,0,-20,0

# Render size before trimming
IMG_W := 1600
IMG_H := 1200

# Print the first candidate that exists and is executable. Globs are
# version-sorted newest first, so KiCad/10.0 wins over KiCad/9.0.
find-tool = $(shell { command -v $(1); \
    printf '%s\n' $(2); \
    printf '%s\n' $(3) | sort -rV; \
  } 2>/dev/null | while IFS= read -r f; do \
    [ -f "$$f" ] && [ -x "$$f" ] && { echo "$$f"; break; }; \
  done)

ifeq ($(origin KICAD_CLI),undefined)
KICAD_CLI := $(call find-tool,kicad-cli, \
  /Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli, \
  /mnt/c/Program\ Files/KiCad/*/bin/kicad-cli.exe \
  /c/Program\ Files/KiCad/*/bin/kicad-cli.exe)
endif

# On Windows prefer openscad.com, the console wrapper that forwards output.
ifeq ($(origin OPENSCAD),undefined)
OPENSCAD := $(call find-tool,openscad, \
  /Applications/OpenSCAD.app/Contents/MacOS/OpenSCAD, \
  /Applications/OpenSCAD*.app/Contents/MacOS/OpenSCAD \
  /mnt/c/Program\ Files/OpenSCAD*/openscad.com \
  /mnt/c/Program\ Files/OpenSCAD*/openscad.exe \
  /c/Program\ Files/OpenSCAD*/openscad.com \
  /c/Program\ Files/OpenSCAD*/openscad.exe)
endif

# The Manifold backend (development snapshots since 2023) is required: the
# legacy CGAL one rejects the imported PCB meshes as not closed and silently
# skips subtracting them, producing wrong geometry after minutes of work.
ifneq ($(OPENSCAD),)
OPENSCAD_BACKEND := $(if $(shell "$(OPENSCAD)" --help 2>&1 | grep -i manifold),--backend=manifold)
endif

ifeq ($(origin MAGICK),undefined)
MAGICK := $(call find-tool,magick, \
  /opt/homebrew/bin/magick /usr/local/bin/magick, \
  /mnt/c/Program\ Files/ImageMagick*/magick.exe \
  /c/Program\ Files/ImageMagick*/magick.exe)
ifeq ($(MAGICK),)
# ImageMagick 6; Windows also ships an unrelated convert.exe, hence the check
MAGICK := $(shell convert -version 2>/dev/null | grep -q ImageMagick && command -v convert)
endif
endif

# With ImageMagick the model is rendered at twice the size and scaled down
# after the background is removed, which keeps its edges anti-aliased.
SCALE := $(if $(MAGICK),2,1)
RW := $(shell echo $$(($(IMG_W) * $(SCALE))))
RH := $(shell echo $$(($(IMG_H) * $(SCALE))))

STL_ARGS := --export-format binstl
PNG_ARGS  = --render --imgsize=$(RW),$(RH) --viewall --autocenter --projection=p \
  --colorscheme=Cornfield $($*_VIEW)

# Flood-fill the background from each corner, then crop to what is left
X1 := $(shell echo $$(($(RW) - 1)))
Y1 := $(shell echo $$(($(RH) - 1)))
CORNERS := 0,0 $(X1),0 0,$(Y1) $(X1),$(Y1)
TRANSPARENT := -alpha set -fuzz 6% -fill none \
  $(foreach c,$(CORNERS),-draw 'color $(c) floodfill') \
  -trim +repage -resize 50%

# A Windows OpenSCAD cannot read from the WSL filesystem (it fails to resolve
# \\wsl.localhost\... paths), so from WSL the sources are staged through a
# directory under the Windows %TEMP%. KiCad copes with those paths directly.
ifneq ($(filter %.exe %.com,$(OPENSCAD)),)
ifneq ($(findstring \\,$(shell wslpath -w . 2>/dev/null)),)
OPENSCAD_STAGE := $(shell wslpath "$$(cmd.exe /c 'echo %TEMP%' 2>/dev/null | tr -d '\r')" 2>/dev/null)
endif
endif

SCHEMATICS := $(wildcard $(PCB_DIR)/*.kicad_sch)
PCB_OUTPUTS := $(PCB_DIR)/$(PROJECT).pdf $(PCB_DIR)/$(PROJECT).png
STL_OUTPUTS := $(foreach m,$(MODELS),$(3D_DIR)/$(m).stl)
3D_OUTPUTS  := $(STL_OUTPUTS) $(STL_OUTPUTS:.stl=.png)

.PHONY: all pcb 3d clean check-kicad check-openscad

all: pcb 3d

pcb: $(PCB_OUTPUTS)

3d: $(3D_OUTPUTS)

check-kicad:
	@[ -n "$(KICAD_CLI)" ] || { echo "kicad-cli not found; install KiCad 10 or pass KICAD_CLI=..." >&2; exit 1; }

check-openscad:
	@[ -n "$(OPENSCAD)" ] || { echo "OpenSCAD not found; install it or pass OPENSCAD=..." >&2; exit 1; }
	@[ -n "$(OPENSCAD_BACKEND)" ] || { echo "$(OPENSCAD) has no Manifold backend; use a development snapshot" >&2; exit 1; }

$(PCB_DIR)/$(PROJECT).pdf: $(SCHEMATICS) $(PCB_DIR)/$(PROJECT).kicad_pro | check-kicad
	cd $(PCB_DIR) && "$(KICAD_CLI)" sch export pdf -o $(PROJECT).pdf $(PROJECT).kicad_sch

$(PCB_DIR)/$(PROJECT).png: $(PCB_DIR)/$(PROJECT).kicad_pcb $(PCB_DIR)/$(PROJECT).kicad_pro | check-kicad
	cd $(PCB_DIR) && "$(KICAD_CLI)" pcb render -o $(PROJECT).png \
	  --width 1600 --height 1200 --quality high --floor --perspective \
	  --rotate '-30,0,20' $(PROJECT).kicad_pcb >/dev/null
	$(if $(MAGICK),"$(MAGICK)" $@ -trim +repage $@)

# $(call openscad,output,inputs,script,ARGS_VARIABLE[,setup]): run OpenSCAD on
# <script> in a scratch directory holding copies of <inputs>, after running the
# optional shell <setup> there, and copy the result back. The arguments are
# passed by variable name since they contain commas.
define openscad
	@echo "OpenSCAD $(3) -> $(1)"
	@set -e; \
	dir=$$(mktemp -d $(if $(OPENSCAD_STAGE),-p "$(OPENSCAD_STAGE)")); \
	trap 'rm -rf "$$dir"' EXIT; \
	cp $(2) "$$dir"/; \
	$(5) \
	(cd "$$dir" && "$(OPENSCAD)" $(OPENSCAD_BACKEND) -q $($(4)) -o out$(suffix $(1)) $(3)); \
	cat "$$dir"/out$(suffix $(1)) > $(1)
endef

$(3D_DIR)/%.stl: $(3D_DIR)/%.scad | check-openscad
	$(call openscad,$@,$< $(addprefix $(3D_DIR)/,$($*_DEPS)),$*.scad,STL_ARGS)

# Rendered from the exported mesh rather than the .scad: a mesh has no cut
# faces for OpenSCAD to tint, so the whole model comes out in one colour.
$(3D_DIR)/%.png: $(3D_DIR)/%.stl | check-openscad
	$(call openscad,$@,$<,render.scad,PNG_ARGS,echo 'import("$*.stl");' > "$$dir"/render.scad;)
	$(if $(MAGICK),"$(MAGICK)" $@ $(TRANSPARENT) $@,@echo "ImageMagick not found; $@ keeps its background")

# Secondary expansion lets each model's STL depend on the files it imports
.SECONDEXPANSION:
$(STL_OUTPUTS): $$(addprefix $(3D_DIR)/,$$($$(basename $$(notdir $$@))_DEPS))

clean:
	rm -f $(PCB_OUTPUTS) $(3D_OUTPUTS)
