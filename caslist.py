#!/usr/bin/python3

import sys

assert len(sys.argv) == 2

file_name = sys.argv[1]

f = open(file_name, "rb")
d = f.read()
f.close()

sample = 0

while d:
	header = d[:8]
	header_type = header[:4]
	header_length = int.from_bytes(header[4:6],"little")
	header_aux = header[6:8]
	d = d[8:]
	if header_type == b'FUJI':
		print(f"FUJI '{d[8:8+header_length].decode()}'")
	elif header_type == b'baud':
		assert header_length == 0
		print(f"Baud rate: {int.from_bytes(header_aux, 'little')}")
	elif header_type == b'fsk ':
		print(f"FSK, silence {int.from_bytes(header_aux, 'little')} ms, length {header_length}")
	elif header_type == b'data':
		print(f"DATA, silence {int.from_bytes(header_aux, 'little')} ms, length {header_length}")
	elif header_type == b'pwmc':
		print(f"PWMC, silence {int.from_bytes(header_aux, 'little')} ms, length {header_length}")
	elif header_type == b'pwml':
		print(f"PWML, silence {int.from_bytes(header_aux, 'little')} ms, length {header_length}")
		if header_length % 2 != 0:
			assert False
	elif header_type == b'pwmd':
		print(f"PWMD, 0 duration {header_aux[0]}, 1 duration {header_aux[1]}, length {header_length}")
	elif header_type == b'pwms':
		assert header_length == 2
		sample = int.from_bytes(d[:2], 'little')
		print(f"PWMS, bit order {'MSB' if (header_aux[0] >> 2 & 1) == 1 else 'LSB'}, pwm bit order {(header_aux[0] & 3) :2b}, sample {sample}")
	d = d[header_length:]
