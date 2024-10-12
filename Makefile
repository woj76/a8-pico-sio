# The Makefile to generate some of the source files, you can run this before
# building the project with cmake (approriate tools are needed), but the
# pre-generated files are included in the project.

all:
	xa disk_boot.asm -o disk_boot.bin -l disk_boot.txt
	python3 boot_h.py
	python3 atari_font_gen.py
