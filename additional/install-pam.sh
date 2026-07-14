#!/bin/bash

read -n1 -p "Install kde pam file? [y,n]" kde
case $kde in
    y|Y) sudo cp kde /usr/share/pam-configs/kde && echo "Installed kde pam file" ;;
    *) echo "Did not install kde pam file" ;;
esac

read -n1 -p "Install polkit pam file? [y,n]" polkit
case $polkit in
    y|Y) sudo cp polkit-1 /usr/share/pam-configs/polkit-1 && echo "Installed polkit pam file" ;;
    *) echo "Did not install polkit pam file" ;;
esac

echo "Done"
