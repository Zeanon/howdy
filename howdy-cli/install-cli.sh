#!/bin/bash

echo "Installing howdy-cli"

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

sudo cp bin/howdy-cli.in /usr/local/bin/howdy-cli
echo "$SCRIPT_DIR/howdy-manager.sh" | sudo tee -a /usr/local/bin/howdy-cli > /dev/null

sudo chmod 755 /usr/local/bin/howdy-cli

echo "Done"
