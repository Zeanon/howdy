#!/bin/bash

read -n1 -p "Install howdy-gtk desktop file? [y,n]" howdy_gtk
echo
case $howdy_gtk in
    y|Y) cp Howdy.desktop ~/.local/share/applications/Howdy.desktop && echo "Created howdy-gtk desktop file" ;;
    *) echo "Did not create howdy-gtk desktop file" ;;
esac
echo

read -n1 -p "Install kde pam file? [y,n]" kde
echo
case $kde in
    y|Y) sudo cp kde /etc/pam.d/kde && echo "Installed kde pam file" ;;
    *) echo "Did not install kde pam file" ;;
esac
echo

read -n1 -p "Install polkit pam file? [y,n]" polkit
echo
case $polkit in
    y|Y) sudo cp polkit-1 /etc/pam.d/polkit-1 && echo "Installed polkit pam file" ;;
    *) echo "Did not install polkit pam file" ;;
esac
echo

echo "Done"
