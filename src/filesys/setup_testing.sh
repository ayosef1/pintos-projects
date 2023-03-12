#!/bin/bash
rm -f filesys.dsk
rm -f swap.dsk
rm -f filesys.dsk.lock
rm -f swap.dsk.lock
pintos-mkdisk filesys.dsk --filesys-size=2
pintos-mkdisk swap.dsk --swap-size=4
pintos -f -q
pintos -p build/tests/userprog/args-none -a test -- -q

