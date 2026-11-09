#!/bin/bash
F=/home/hcy/hakios-arm64e-xuanyuan/userland/userland/src/inject26.c
echo "=== button-related identifiers ==="
grep -nE "button|Button|consumer|Consumer|HIDPage|usage" "$F" | head -40
