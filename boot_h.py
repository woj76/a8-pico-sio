#!/usr/bin/python3

f = open("a.o65", "rb")
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
s += "\n};\n"

print(s)
