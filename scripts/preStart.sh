#!/bin/sh

echo "Running fpp-PixelRadio PreStart Script"

BASEDIR=$(dirname $0)
cd $BASEDIR
cd ..
make "SRCDIR=${SRCDIR}"
