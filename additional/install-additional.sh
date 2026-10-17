#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

read -p "Install kde-fingerprint pam file? [y|N]" kde
kde="${kde:=N}"
case $kde in
    y|Y) sudo cp bin/kde-fingerprint /etc/pam.d/ && echo "Installed kde-fingerprint pam file" ;;
    *) echo "Did not install kde-fingerprint pam file" ;;
esac
echo

read -p "Install polkit pam file? [y|N]" polkit
polkit="${polkit:=N}"
case $polkit in
    y|Y) sudo cp bin/polkit-1 /etc/pam.d/ && echo "Installed polkit pam file" ;;
    *) echo "Did not install polkit pam file" ;;
esac
echo

echo "Done"
