all:
	xa disk_boot.asm -o disk_boot.bin -l disk_boot.txt
	python3 boot_h.py
	python3 atari_font_gen.py
