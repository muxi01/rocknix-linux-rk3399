#!/bin/bash

if [ "$INSTALL_MOD_PATH" = "" ] ; then 
    echo "INSTALL_MOD_PATH is not set"
else
    echo "INSTALL_MOD_PATH is set to $INSTALL_MOD_PATH"
    make modules 
    make modules_install 
    # depmod -a -s $INSTALL_MOD_PATH
fi 