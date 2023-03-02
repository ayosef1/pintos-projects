#!/bin/bash
rm -f filesys.dsk
rm -f swap.dsk
pintos-mkdisk filesys.dsk --filesys-size=2
pintos-mkdisk swap.dsk --swap-size=4
pintos -f -q
for arg in "$@"; do
	pintos -p build/tests/vm/$arg -a $arg -- -q
done
