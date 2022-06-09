#!/bin/bash

# fpp-PixelRadio install script

BASEDIR=$(dirname $0)
cd $BASEDIR
cd ..
make "SRCDIR=${SRCDIR}"


. ${FPPDIR}/scripts/common
setSetting restartFlag 1
