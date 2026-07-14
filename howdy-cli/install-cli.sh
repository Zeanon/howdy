#!/bin/bash

echo "Installing howdy-cli"
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
sudo ln -s $SCRIPT_DIR/howdy-manager.sh /usr/local/bin/howdy-cli
