#!/bin/bash

video=build/video

test -d ${video}/ppm || mkdir -p ${video}/ppm
test -d ${video}/mp4 || mkdir -p ${video}/mp4

rm -f ${video}/ppm/* ${video}/mp4/binpack.mp4

./build/bin/glbinpack --offline \
	--bin-size 1024x1024 --random 32,32 \
	--frame-size 1920x1080 --frame-step 5 --frame-count 10000 \
	--template "${video}/ppm/binpack-%09d.ppm"

ffmpeg -framerate 25 -i "${video}/ppm/binpack-%09d.ppm" \
	-s 1920x1080 -crf 0 \
	-preset veryslow -threads 36 \
	${video}/mp4/binpack.mp4 -hide_banner
