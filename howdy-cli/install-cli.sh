#!/bin/bash

echo "Installing howdy-cli"

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

cat bin/howdy-cli.in | sudo tee /usr/local/bin/howdy-cli > /dev/null
echo "$SCRIPT_DIR/howdy-manager.sh" | sudo tee -a /usr/local/bin/howdy-cli > /dev/null

sudo chmod 755 /usr/local/bin/howdy-cli

echo "Done"
