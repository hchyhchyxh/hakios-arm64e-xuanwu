#!/bin/bash
X=/home/hcy/hakios-arm64e-xuanyuan/xnu-src/isolated/xnu-xnu-12377.121.6
sed -n '1780,1910p' "$X/iokit/Kernel/MiniIOMobileFramebuffer.cpp"
