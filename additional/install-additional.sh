#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

read -p "Install howdy-gtk desktop file? [Y|n]" howdy_gtk
howdy_gtk="${howdy_gtk:=Y}"
case $howdy_gtk in
    y|Y) cp bin/Howdy.desktop ~/.local/share/applications/ && echo "Icon=${SCRIPT_DIR%/*}/howdy-gtk/src/logo.png" | tee -a ~/.local/share/applications/Howdy.desktop > /dev/null && echo "Created howdy-gtk desktop file" ;;
    *) echo "Did not create howdy-gtk desktop file" ;;
esac
echo

read -p "Install kde pam file? [y|N]" kde
kde="${kde:=N}"
case $kde in
    y|Y) sudo cp bin/kde /etc/pam.d/ && echo "Installed kde pam file" ;;
    *) echo "Did not install kde pam file" ;;
esac
echo

read -p "Install kde-fingerprint pam file? [y|N]" kde
kde="${kde:=N}"
case $kde in
    y|Y) sudo cp bin/kde-fingerprint /etc/pam.d/ && echo "Installed kde-fingerprint pam file" ;;
    *) echo "Did not install kde-fingerprint pam file" ;;
esac
echo

read -p "Install kde-smartcard pam file? [y|N]" kde
kde="${kde:=N}"
case $kde in
    y|Y) sudo cp bin/kde-smartcard /etc/pam.d/ && echo "Installed kde-smartcard pam file" ;;
    *) echo "Did not install kde-smartcard pam file" ;;
esac
echo

read -p "Install howdy-fingerprint pam file? [y|N]" kde
kde="${kde:=N}"
case $kde in
    y|Y) sudo cp bin/howdy-fingerprint /etc/pam.d/ && echo "Installed howdy-fingerprint pam file" ;;
    *) echo "Did not install howdy-fingerprint pam file" ;;
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
