#!/bin/bash

mkdir -p firmware
rm -rf firmware/*_$1.uf2

rm -rf build
mkdir build
cd build

cmake ..
make
mv a8_pico_sio.uf2 ../firmware/a8_pico1_sio_$1.uf2
rm -rf *

cmake -DPICO_BOARD=pico2 ..
make
mv a8_pico_sio.uf2 ../firmware/a8_pico2_sio_$1.uf2
rm -rf *

cmake -DPICO_BOARD=pimoroni_picolipo_16mb ..
make
mv a8_pico_sio.uf2 ../firmware/a8_pimoroni16mb_sio_$1.uf2
rm -rf *
