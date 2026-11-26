#!/bin/bash

if [[ "$EUID" != 0 ]]; then
    echo "This script needs to be run as root."
    exit 1
fi

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
# The files come from next to this script, not from the directory it is started from
cd -- "$SCRIPT_DIR" || exit 1

read -p "Install kde-fingerprint pam file? [y|N]" kde
kde="${kde:=N}"
case $kde in
    y|Y)
        if [ -f /etc/pam.d/kde-fingerprint ]; then
            read -p "Back up kde-fingerprint pam file? [y|N]" backup
            backup="${backup:=N}"
            case $backup in
                y|Y) cp /etc/pam.d/kde-fingerprint /etc/pam.d/kde-fingerprint.bak && echo "Backed up kde-fingerprint pam file to kde-fingerprint.bak" ;;
                *) echo "Did not back up kde-fingerprint pam file" ;;
            esac
        fi
        cp bin/kde-fingerprint /etc/pam.d/ && echo "Installed kde-fingerprint pam file" ;;
    *) echo "Did not install kde-fingerprint pam file" ;;
esac
echo

read -p "Install polkit pam file? [y|N]" polkit
polkit="${polkit:=N}"
case $polkit in
    y|Y)
        if [ -f /etc/pam.d/polkit-1 ]; then
            read -p "Back up polkit pam file? [y|N]" backup
            backup="${backup:=N}"
            case $backup in
                y|Y) cp /etc/pam.d/polkit-1 /etc/pam.d/polkit-1.bak && echo "Backed up polkit pam file to polkit-1.bak" ;;
                *) echo "Did not back up polkit pam file" ;;
            esac
        fi 
        cp bin/polkit-1 /etc/pam.d/ && echo "Installed polkit pam file" ;;
    *) echo "Did not install polkit pam file" ;;
esac
echo

echo "Done"
