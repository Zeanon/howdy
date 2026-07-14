#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

read -p "Install howdy-gtk desktop file? [Y|n]" howdy_gtk
howdy_gtk="${howdy_gtk:=Y}"
case $howdy_gtk in
    y|Y) cp Howdy.desktop ~/.local/share/applications/Howdy.desktop && echo "Icon=${SCRIPT_DIR%/*}/howdy-gtk/src/logo.png" | tee -a ~/.local/share/applications/Howdy.desktop > /dev/null && echo "Created howdy-gtk desktop file" ;;
    *) echo "Did not create howdy-gtk desktop file" ;;
esac
echo

read -p "Install kde pam file? [y|N]" kde
kde="${kde:=N}"
case $kde in
    y|Y) sudo cp kde /etc/pam.d/kde && echo "Installed kde pam file" ;;
    *) echo "Did not install kde pam file" ;;
esac
echo

read -p "Install polkit pam file? [y|N]" polkit
polkit="${polkit:=N}"
case $polkit in
    y|Y) sudo cp polkit-1 /etc/pam.d/polkit-1 && echo "Installed polkit pam file" ;;
    *) echo "Did not install polkit pam file" ;;
esac
echo

echo "Done"
