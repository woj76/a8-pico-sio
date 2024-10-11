#!/usr/bin/python3

f = open("disk_boot.bin", "rb")
d = f.read()
f.close()

s = "const uint8_t boot_loader[256] = {\n\t"

i = 0

for c in d:
	s += f"0x{c:02X},"
	i += 1
	if i % 16 == 0:
		s += "\n\t"
s = s[:-3]
s += "\n};\n\nconst uint8_t boot_reloc_locs[] = {"

f = open("disk_boot.txt", "rt")
d = f.read().split("\n")
f.close()

cnt = 0;
for x in d:
	if x[:5] == "reloc":
		s += "0x"+x[13:15]+", "
		cnt += 1

s = s[:-2]+f"}};\n\n#define boot_reloc_locs_size {cnt}\n\n"

f = open("boot_loader.h","wt")
f.write(s)
f.close();
